#include "include/zebkit/media_tools.h"
#include <algorithm>
#include <cerrno>
#include <fcntl.h>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <memory>
#include <string>
#include <vector>
#if defined(_WIN32)
#include <io.h>
#include <windows.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

// The vendor variant lets the same implementation be integration-tested
// against a packaged engine, without rebuilding Chromium.
#ifdef ZEBKIT_MEDIA_TOOLS_VENDOR
#include <zebkit/vendor/ffmpeg.h>
using namespace zebkit::vendor::ffmpeg;
#else
extern "C" {
#include "third_party/ffmpeg/libavcodec/avcodec.h"
#include "third_party/ffmpeg/libavformat/avformat.h"
#include "third_party/ffmpeg/libavutil/audio_fifo.h"
#include "third_party/ffmpeg/libavutil/channel_layout.h"
#include "third_party/ffmpeg/libavutil/imgutils.h"
#include "third_party/ffmpeg/libavutil/opt.h"
#include "third_party/ffmpeg/libavutil/pixdesc.h"
#include "third_party/ffmpeg/libavutil/time.h"
#include "third_party/ffmpeg/libswresample/swresample.h"
#include "third_party/ffmpeg/libswscale/swscale.h"
}
#endif

namespace {
// A live stream's address, which is opened as it is: RTMP (FLV) and RTSP,
// each also over TLS, and RTMP through HTTP. Anything else is a local path.
const char* const kSchemes[] = {"rtmp://", "rtmps://", "rtmpt://", "rtmpts://", "rtsp://", "rtsps://"};
bool IsStream(const std::string& path) {
  for (const char* scheme : kSchemes)
    if (path.rfind(scheme, 0) == 0) return true;
  return false;
}
bool IsRtsp(const std::string& path) { return path.rfind("rtsp://", 0) == 0 || path.rfind("rtsps://", 0) == 0; }
std::string Quote(const char* value) {
  std::string out = "\"";
  // Tags and file names are not always UTF-8; JSON has to be.
  std::string text;
  for (const unsigned char* p = reinterpret_cast<const unsigned char*>(value ? value : ""); *p;) {
    const int length = *p < 0x80 ? 1 : *p >= 0xC2 && *p <= 0xDF ? 2 : (*p & 0xF0) == 0xE0 ? 3 : *p >= 0xF0 && *p <= 0xF4 ? 4 : 0;
    bool valid = length > 0;
    for (int i = 1; valid && i < length; ++i) valid = (p[i] & 0xC0) == 0x80;
    if (valid && length == 3) valid = !(*p == 0xE0 && p[1] < 0xA0) && !(*p == 0xED && p[1] > 0x9F);
    if (valid && length == 4) valid = !(*p == 0xF0 && p[1] < 0x90) && !(*p == 0xF4 && p[1] > 0x8F);
    if (valid) { text.append(reinterpret_cast<const char*>(p), length); p += length; }
    else { text += "\xEF\xBF\xBD"; ++p; }
  }
  for (const unsigned char* p = reinterpret_cast<const unsigned char*>(text.c_str()); *p; ++p) {
    if (*p == '"' || *p == '\\') { out += '\\'; out += *p; }
    else if (*p < 32) { char s[7]; snprintf(s, sizeof(s), "\\u%04x", *p); out += s; }
    else out += *p;
  }
  return out + "\"";
}
std::string Tags(AVDictionary* dict) {
  std::string s = "{";
  const AVDictionaryEntry* entry = nullptr;
  while ((entry = av_dict_get(dict, "", entry, AV_DICT_IGNORE_SUFFIX))) {
    if (s.size() > 1) s += ',';
    s += Quote(entry->key) + ':' + Quote(entry->value);
  }
  return s + '}';
}
std::string Rational(AVRational q) { return std::to_string(q.num) + '/' + std::to_string(q.den); }
std::string Time(int64_t value, AVRational q) {
  if (value == AV_NOPTS_VALUE || !q.den) return "null";
  // Decimal strings, like ffprobe, avoid losing integer precision in JSON.
  return Quote(std::to_string(value * av_q2d(q)).c_str());
}
struct Job {
  AVFormatContext* input = nullptr;
  AVFormatContext* output = nullptr;
  AVCodecContext* decoder = nullptr;
  // Per input stream, when it is encoded rather than copied.
  struct Coder {
    AVCodecContext* decoder = nullptr;
    AVCodecContext* encoder = nullptr;
    SwsContext* scaler = nullptr;
    SwrContext* resampler = nullptr;
    AVAudioFifo* fifo = nullptr;
    AVFrame* converted = nullptr;
    int output = -1;
    int64_t next = AV_NOPTS_VALUE;  // audio: the next sample's time, in 1/rate
    int64_t last = AV_NOPTS_VALUE;  // video: the last frame's time, for order
    ~Coder() {
      avcodec_free_context(&decoder);
      avcodec_free_context(&encoder);
      sws_freeContext(scaler);
      swr_free(&resampler);
      if (fifo) av_audio_fifo_free(fifo);
      av_frame_free(&converted);
    }
  };
  std::vector<std::unique_ptr<Coder>> coders;
  // Options for opening a live stream (rtsp_transport, rw_timeout).
  AVDictionary* network = nullptr;
  // -rw_timeout again, as RTSP's own "timeout" (its wait for the server, in
  // the same unit). RTMP has an option of that name too, the time it waits
  // as a server, in seconds, which it implies: so for RTSP only.
  std::string rtsp_timeout;
  // -listen 1, for the input only: RTMP's listen, RTSP's rtsp_flags listen.
  bool listen = false;
  AVDictionary* NetworkFor(const std::string& address) const {
    AVDictionary* options = nullptr;
    av_dict_copy(&options, network, 0);
    if (IsRtsp(address) && !rtsp_timeout.empty()) av_dict_set(&options, "timeout", rtsp_timeout.c_str(), 0);
    return options;
  }
  // When the progress function was last asked from inside FFmpeg, which
  // blocks on a network read or a connect.
  int64_t asked = 0;
  AVFrame* frame = nullptr;
  AVPacket* packet = nullptr;
  SwrContext* resampler = nullptr;
  SwsContext* scaler = nullptr;
  FILE* file = nullptr;
  std::string temporary, failure;
  zebkit_media_progress_func callback = nullptr;
  void* data = nullptr;
  int64_t packets = 0;
  bool cancelled = false;
  ~Job() {
    if (file) fclose(file);
    coders.clear();
    av_dict_free(&network);
    if (output) {
      if (output->pb && !(output->oformat->flags & AVFMT_NOFILE)) avio_closep(&output->pb);
      avformat_free_context(output);
    }
    avformat_close_input(&input);
    avcodec_free_context(&decoder);
    av_frame_free(&frame);
    av_packet_free(&packet);
    swr_free(&resampler);
    sws_freeContext(scaler);
#if defined(_WIN32)
    if (!temporary.empty()) DeleteFileW(Wide(temporary).c_str());
#else
    if (!temporary.empty()) unlink(temporary.c_str());
#endif
  }
  bool Fail(const std::string& s) { failure = s; return false; }
  bool Check(int code, const char* what) {
    if (code >= 0) return true;
    if (cancelled) return Fail("cancelled");
    char s[AV_ERROR_MAX_STRING_SIZE]; av_strerror(code, s, sizeof(s));
    return Fail(std::string(what) + ": " + s);
  }
  bool Progress(int64_t time = INT64_MIN) {
    if (callback && !callback(packets, time, data)) { cancelled = true; return Fail("cancelled"); }
    return true;
  }
  // FFmpeg's interrupt callback: while it waits on the network, the progress
  // function is still asked, ten times a second, and can cancel the wait.
  static int Interrupt(void* opaque) {
    Job* job = static_cast<Job*>(opaque);
    if (job->cancelled) return 1;
    const int64_t now = av_gettime_relative();
    if (now - job->asked < 100000) return 0;
    job->asked = now;
    if (job->callback && !job->callback(job->packets, INT64_MIN, job->data)) {
      job->cancelled = true;
      job->failure = "cancelled";
      return 1;
    }
    return 0;
  }
  AVIOInterruptCB Interrupter() { return AVIOInterruptCB{&Job::Interrupt, this}; }
  bool Open(const std::string& path) {
    input = avformat_alloc_context();
    if (!input) return Fail("allocate input");
    input->interrupt_callback = Interrupter();
    if (IsStream(path)) {
      AVDictionary* options = NetworkFor(path);
      if (listen && IsRtsp(path)) {
        // RTSP's listener passes its listen_timeout, seconds, to TCP as
        // milliseconds; its default, -1 for none, becomes -1000, which TCP
        // refuses (FFmpeg 8). A wait of -rw_timeout, or of 24 days.
        av_dict_set(&options, "rtsp_flags", "listen", 0);
        const long long seconds = rtsp_timeout.empty() ? INT_MAX / 1000 : std::max(1LL, std::stoll(rtsp_timeout) / 1000000);
        av_dict_set_int(&options, "listen_timeout", std::min<long long>(seconds, INT_MAX / 1000), 0);
      } else if (listen) {
        av_dict_set(&options, "listen", "1", 0);
      }
      const int r = avformat_open_input(&input, path.c_str(), nullptr, &options);
      av_dict_free(&options);
      if (!Check(r, "open input") || !Check(avformat_find_stream_info(input, nullptr), "read stream information"))
        return false;
      packet = av_packet_alloc();
      return packet || Fail("allocate packet");
    }
#if defined(_WIN32)
    const DWORD attributes = GetFileAttributesW(Wide(path).c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_DEVICE)))
      return Fail("input must be a local regular file or a stream's rtmp(s):// or rtsp(s):// address: " + path);
#else
    struct stat st;
    if (stat(path.c_str(), &st) || !S_ISREG(st.st_mode))
      return Fail("input must be a local regular file or a stream's rtmp(s):// or rtsp(s):// address: " + path);
#endif
    // Explicit file: avoids interpreting a colon in a local filename as a protocol.
    if (!Check(avformat_open_input(&input, ("file:" + path).c_str(), nullptr, nullptr), "open input") ||
        !Check(avformat_find_stream_info(input, nullptr), "read stream information")) return false;
    packet = av_packet_alloc();
    return packet || Fail("allocate packet");
  }
#if defined(_WIN32)
  // The C API's paths are UTF-8, as FFmpeg's file protocol takes them here.
  static std::wstring Wide(const std::string& text) {
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    std::wstring wide(length > 0 ? length : 1, L'\0');
    if (length > 0) MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, wide.data(), length);
    wide.resize(wcslen(wide.c_str()));
    return wide;
  }
  static bool Identity(const std::string& path, BY_HANDLE_FILE_INFORMATION* info) {
    HANDLE handle = CreateFileW(Wide(path).c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return false;
    const bool ok = GetFileInformationByHandle(handle, info);
    CloseHandle(handle);
    return ok;
  }
  bool Stage(const std::string& path, const std::string& source, bool overwrite) {
    const DWORD attributes = GetFileAttributesW(Wide(path).c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES) {
      BY_HANDLE_FILE_INFORMATION dst, src;
      if (Identity(path, &dst) && Identity(source, &src) && dst.dwVolumeSerialNumber == src.dwVolumeSerialNumber &&
          dst.nFileIndexHigh == src.nFileIndexHigh && dst.nFileIndexLow == src.nFileIndexLow)
        return Fail("input and output refer to the same file");
      if (!overwrite) return Fail("output exists; use -y to replace it");
      if (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_DEVICE | FILE_ATTRIBUTE_REPARSE_POINT))
        return Fail("output must be a regular file");
    }
    for (unsigned attempt = 0; attempt < 1000; ++attempt) {
      const std::string name = path + ".zebkit-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(attempt);
      HANDLE handle = CreateFileW(Wide(name).c_str(), GENERIC_READ | GENERIC_WRITE,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                  CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
      if (handle == INVALID_HANDLE_VALUE) {
        if (GetLastError() == ERROR_FILE_EXISTS) continue;
        return Fail("create output temporary file: error " + std::to_string(GetLastError()));
      }
      const int fd = _open_osfhandle(reinterpret_cast<intptr_t>(handle), 0);
      file = fd < 0 ? nullptr : _fdopen(fd, "w+b");
      if (!file) { if (fd < 0) CloseHandle(handle); else _close(fd); return Fail("open output temporary file"); }
      temporary = name;
      return true;
    }
    return Fail("create output temporary file: too many left behind");
  }
  bool Publish(const std::string& path, bool overwrite) {
    if (file) { int r = fclose(file); file = nullptr; if (r) return Fail("close output failed"); }
    // Without MOVEFILE_REPLACE_EXISTING the move does not clobber.
    if (!MoveFileExW(Wide(temporary).c_str(), Wide(path).c_str(),
                     MOVEFILE_WRITE_THROUGH | (overwrite ? MOVEFILE_REPLACE_EXISTING : 0)))
      return Fail("publish output: error " + std::to_string(GetLastError()));
    temporary.clear();
    return true;
  }
#else
  bool Stage(const std::string& path, const std::string& source, bool overwrite) {
    struct stat dst, src;
    bool replacing = false;
    if (!lstat(path.c_str(), &dst)) {
      struct stat target = dst;
      if (S_ISLNK(dst.st_mode) && stat(path.c_str(), &target)) target = dst;
      if (!stat(source.c_str(), &src) && src.st_dev == target.st_dev && src.st_ino == target.st_ino)
        return Fail("input and output refer to the same file");
      if (!overwrite) return Fail("output exists; use -y to replace it");
      // Renaming over a link would replace the link, not what it names.
      if (!S_ISREG(dst.st_mode)) return Fail("output must be a regular file");
      replacing = true;
    }
    std::string name = path + ".zebkit-XXXXXX";
    std::vector<char> buffer(name.begin(), name.end()); buffer.push_back(0);
    int fd = mkstemp(buffer.data());
    if (fd < 0) return Fail("create output temporary file: " + std::string(strerror(errno)));
    temporary = buffer.data();
    // A replaced file keeps its permissions.
    if (replacing) fchmod(fd, dst.st_mode & 07777);
    file = fdopen(fd, "w+b");
    if (!file) { close(fd); return Fail("open output temporary file"); }
    return true;
  }
  bool Publish(const std::string& path, bool overwrite) {
    if (file) { int r = fclose(file); file = nullptr; if (r) return Fail("close output failed"); }
    // link is an atomic no-clobber publication; rename atomically replaces.
    int r = overwrite ? rename(temporary.c_str(), path.c_str()) : link(temporary.c_str(), path.c_str());
    if (r && !overwrite && (errno == EPERM || errno == ENOTSUP || errno == ENOSYS)) {
      // No hard links here (exFAT, some network mounts). Claiming the name with
      // O_EXCL keeps the no-clobber promise; the rename then fills it in.
      int claim = open(path.c_str(), O_CREAT | O_EXCL | O_WRONLY, 0600);
      if (claim < 0) return Fail("publish output: " + std::string(strerror(errno)));
      close(claim);
      r = rename(temporary.c_str(), path.c_str());
      if (r) { const int failure = errno; unlink(path.c_str()); errno = failure; }
      else { temporary.clear(); return true; }
    }
    if (r) return Fail("publish output: " + std::string(strerror(errno)));
    if (!overwrite) unlink(temporary.c_str());
    temporary.clear();
    return true;
  }
#endif  // defined(_WIN32)
  bool Write(const void* bytes, size_t length) {
    return fwrite(bytes, 1, length, file) == length || Fail("write output failed");
  }
};
int Result(Job& job, bool ok, char** error) {
  if (error) *error = ok ? nullptr : strdup(job.failure.c_str());
  return ok ? 0 : job.cancelled ? 1 : -1;
}
bool Arguments(int argc, const char* const* argv, Job& job) {
  if (argc < 1 || argc > 4096 || !argv) return job.Fail("expected an argument vector without the executable name");
  for (int i = 0; i < argc; ++i) if (!argv[i] || !*argv[i]) return job.Fail("arguments must be nonempty strings");
  return true;
}
bool Number(const std::string& s, int low, int high, int& value) {
  if (s.empty() || s.find_first_not_of("0123456789") != std::string::npos) return false;
  errno = 0; char* end; long n = strtol(s.c_str(), &end, 10);
  if (errno || *end || n < low || n > high) return false;
  value = static_cast<int>(n); return true;
}
struct Options {
  std::string source, target, format;
  // What each kind of stream becomes: "" (left to the other's choice or the
  // default), "copy", or an encoder ("h264", "aac"); or a decoded output
  // ("rawvideo", "pcm_s16le"), which is a mode of its own.
  std::string video, audio;
  std::vector<int> maps;
  bool overwrite = false, no_audio = false, no_video = false;
  int rate = 0, channels = 0, width = 0, height = 0, frames = 0;
  bool pixel_format = false;
  // -ss and -t, in microseconds; a negative duration is none.
  int64_t start = 0, duration = -1;
  // Encoders: bits a second, and frames from one key frame to the next.
  int64_t video_bitrate = 0, audio_bitrate = 0;
  int gop = 0;
  // -re: the input read at its own pace, as a live stream is published.
  bool realtime = false;
  // -listen 1: the input is served, a publisher connecting to it.
  bool listen = false;
  bool Decoded() const { return video == "rawvideo" || audio == "pcm_s16le"; }
  bool Encodes() const { return video == "h264" || audio == "aac"; }
};
// Seconds as ffmpeg's plain form, without its [HH:]MM:SS.
bool Seconds(const std::string& s, int64_t& microseconds) {
  if (s.empty() || s.find_first_not_of("0123456789.") != std::string::npos) return false;
  errno = 0; char* end; double seconds = strtod(s.c_str(), &end);
  if (errno || *end || !(seconds >= 0) || seconds > 1e9) return false;
  microseconds = static_cast<int64_t>(seconds * 1000000); return true;
}
// Bits a second as ffmpeg takes them: digits, with k or M.
bool Bitrate(const std::string& s, int64_t& bits) {
  if (s.empty()) return false;
  int64_t scale = 1;
  std::string digits = s;
  if (s.back() == 'k' || s.back() == 'K') { scale = 1000; digits.pop_back(); }
  else if (s.back() == 'M') { scale = 1000000; digits.pop_back(); }
  int n;
  if (!Number(digits, 1, 1000000000, n)) return false;
  bits = static_cast<int64_t>(n) * scale;
  return bits >= 1000 && bits <= 1000000000;
}
bool Parse(int argc, const char* const* argv, Options& o, Job& job) {
  if (!Arguments(argc, argv, job)) return false;
  bool end = false;
  // A codec named twice for the same kind of stream is an error, as before.
  auto codec = [&](std::string& kind, const std::string& value) {
    if (!kind.empty()) return job.Fail("choose one output mode/codec");
    kind = value;
    return true;
  };
  for (int i = 0; i < argc; ++i) {
    std::string a = argv[i];
    if (!end && a == "--") { end = true; continue; }
    if (!end && a == "-y") { o.overwrite = true; continue; }
    if (!end && a == "-n") { o.overwrite = false; continue; }
    if (!end && a == "-an") { o.no_audio = true; continue; }
    if (!end && a == "-vn") { o.no_video = true; continue; }
    if (!end && a == "-re") { o.realtime = true; continue; }
    if (!end && a[0] == '-') {
      if (a != "-i" && a != "-map" && a != "-c" && a != "-codec" && a != "-c:a" && a != "-c:v" &&
          a != "-f" && a != "-ar" && a != "-ac" && a != "-s" && a != "-frames:v" && a != "-pix_fmt" && a != "-ss" && a != "-t" &&
          a != "-b:v" && a != "-b:a" && a != "-g" && a != "-rtsp_transport" && a != "-rw_timeout" &&
          a != "-listen" && a != "-cert_file" && a != "-key_file" && a != "-ca_file" && a != "-tls_verify")
        return job.Fail("unsupported option: " + a);
      if (++i == argc) return job.Fail("missing value for " + a);
      std::string v = argv[i];
      if (a == "-i") { if (!o.source.empty()) return job.Fail("only one input is supported"); o.source = v; }
      else if (a == "-ss") { if (!Seconds(v, o.start)) return job.Fail("-ss must be seconds"); }
      else if (a == "-t") { if (!Seconds(v, o.duration) || !o.duration) return job.Fail("-t must be seconds above 0"); }
      else if (a == "-c" || a == "-codec") {
        if (v != "copy") return job.Fail("-c takes copy; name encoders per kind: -c:v h264, -c:a aac");
        if (!codec(o.video, v) || !codec(o.audio, v)) return false;
      } else if (a == "-c:v") {
        if (v == "libopenh264") v = "h264";
        if (v != "copy" && v != "rawvideo" && v != "h264") return job.Fail("unsupported codec for stream type");
        if (!codec(o.video, v)) return false;
      } else if (a == "-c:a") {
        if (v != "copy" && v != "pcm_s16le" && v != "aac") return job.Fail("unsupported codec for stream type");
        if (!codec(o.audio, v)) return false;
      } else if (a == "-f") o.format = v;
      else if (a == "-map") { int n; if (v.rfind("0:", 0) || !Number(v.substr(2), 0, 4095, n)) return job.Fail("map must be 0:STREAM_INDEX"); o.maps.push_back(n); }
      else if (a == "-pix_fmt") { if (v != "rgba") return job.Fail("only rgba is supported"); o.pixel_format = true; }
      else if (a == "-ar") { if (!Number(v, 1, 384000, o.rate)) return job.Fail("invalid sample rate"); }
      else if (a == "-ac") { if (!Number(v, 1, 32, o.channels)) return job.Fail("invalid channel count"); }
      else if (a == "-frames:v") { if (!Number(v, 1, INT_MAX, o.frames)) return job.Fail("invalid frame count"); }
      else if (a == "-s") { auto x = v.find('x'); if (x == std::string::npos || !Number(v.substr(0, x), 1, 8192, o.width) || !Number(v.substr(x + 1), 1, 8192, o.height)) return job.Fail("size must be WIDTHxHEIGHT (1..8192)"); }
      else if (a == "-b:v") { if (!Bitrate(v, o.video_bitrate)) return job.Fail("-b:v must be bits a second (1000..1G, k or M)"); }
      else if (a == "-b:a") { if (!Bitrate(v, o.audio_bitrate)) return job.Fail("-b:a must be bits a second (1000..1G, k or M)"); }
      else if (a == "-g") { if (!Number(v, 1, 3000, o.gop)) return job.Fail("-g must be frames (1..3000)"); }
      else if (a == "-rtsp_transport") {
        if (v != "tcp" && v != "udp") return job.Fail("-rtsp_transport must be tcp or udp");
        av_dict_set(&job.network, "rtsp_transport", v.c_str(), 0);
      } else if (a == "-rw_timeout") {
        int n; if (!Number(v, 1, INT_MAX, n)) return job.Fail("-rw_timeout must be microseconds");
        av_dict_set(&job.network, "rw_timeout", v.c_str(), 0);
        job.rtsp_timeout = v;
      } else if (a == "-listen") {
        // The input waits for a publisher, as a server: RTMP's listen, RTSP's
        // rtsp_flags listen (set once the input's scheme is known).
        if (v != "0" && v != "1") return job.Fail("-listen must be 0 or 1");
        o.listen = v == "1";
      } else if (a == "-tls_verify") {
        if (v != "0" && v != "1") return job.Fail("-tls_verify must be 0 or 1");
        av_dict_set(&job.network, "tls_verify", v.c_str(), 0);
      } else {
        // -cert_file, -key_file, -ca_file: TLS's files, for RTMPS and RTSPS.
        av_dict_set(&job.network, a.c_str() + 1, v.c_str(), 0);
      }
    } else { if (!o.target.empty()) return job.Fail("only one output is supported"); o.target = a; }
  }
  if (o.source.empty() || o.target.empty()) return job.Fail("-i INPUT and OUTPUT are required");
  // FFmpeg serves plain RTMP and RTSP only ("rtmp_listen not available" for
  // the others; RTSP's listener passes no TLS options down).
  if (o.listen && o.source.rfind("rtmp://", 0) != 0 && o.source.rfind("rtsp://", 0) != 0)
    return job.Fail("-listen needs an rtmp:// or rtsp:// input");
  job.listen = o.listen;
  if (o.video.empty() && o.audio.empty())
    return job.Fail("choose -c copy, -c:v h264 or copy, -c:a aac or copy, -c:a pcm_s16le or -c:v rawvideo");
  if (o.Decoded()) {
    // A decoded output is one stream, of one kind.
    if ((o.video == "rawvideo" && !o.audio.empty()) || (o.audio == "pcm_s16le" && !o.video.empty()))
      return job.Fail("choose one output mode/codec");
    if (IsStream(o.target)) return job.Fail("decoded output must be a local file");
  } else {
    // The kind not named is copied.
    if (o.video.empty()) o.video = "copy";
    if (o.audio.empty()) o.audio = "copy";
  }
  const bool pcm = o.audio == "pcm_s16le", aac = o.audio == "aac", raw = o.video == "rawvideo", h264 = o.video == "h264";
  if (!pcm && !aac && (o.rate || o.channels)) return job.Fail("-ar/-ac require pcm_s16le or aac");
  if (!raw && !h264 && o.width) return job.Fail("-s requires rawvideo or h264");
  if (!raw && (o.frames || o.pixel_format)) return job.Fail("-frames:v/-pix_fmt require rawvideo");
  if (!h264 && (o.video_bitrate || o.gop)) return job.Fail("-b:v/-g require -c:v h264");
  if (!aac && o.audio_bitrate) return job.Fail("-b:a requires -c:a aac");
  if (h264 && o.width && (o.width % 2 || o.height % 2)) return job.Fail("H.264 needs an even width and height");
  if ((pcm && o.no_audio) || (raw && o.no_video) || (h264 && o.no_video) || (aac && o.no_audio))
    return job.Fail("selected output stream is disabled");
  return true;
}
bool Selected(const Options& o, AVStream* s, int index) {
  if (o.no_audio && s->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) return false;
  if (o.no_video && s->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) return false;
  return o.maps.empty() ? (s->codecpar->codec_type == AVMEDIA_TYPE_AUDIO || s->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) :
      std::find(o.maps.begin(), o.maps.end(), index) != o.maps.end();
}
// The muxer for an output: -f, or the address's scheme, or the extension.
std::string Format(const Options& o) {
  if (!o.format.empty()) return o.format;
  if (IsRtsp(o.target)) return "rtsp";
  if (IsStream(o.target)) return "flv";
  auto dot = o.target.find_last_of('.');
  std::string ext = dot == std::string::npos ? "" : o.target.substr(dot + 1);
  if (ext == "mkv") return "matroska";
  if (ext == "m4a" || ext == "m4v") return "ipod";
  if (ext == "aac") return "adts";
  return ext;
}
// -re: waits until the wall clock reaches a packet's time from the first one,
// asking the progress function on the way, so a file is published at the pace
// a live stream has.
struct Pace {
  int64_t wall = AV_NOPTS_VALUE, first = AV_NOPTS_VALUE;
  bool Wait(Job& job, int64_t time) {
    if (time == INT64_MIN) return true;
    if (first == AV_NOPTS_VALUE) { first = time; wall = av_gettime_relative(); return true; }
    for (;;) {
      const int64_t ahead = (time - first) - (av_gettime_relative() - wall);
      if (ahead <= 0) return true;
      av_usleep(static_cast<unsigned>(std::min<int64_t>(ahead, 100000)));
      if (!job.Progress(time)) return false;
    }
  }
};
// An encoder for input stream @index, written to output stream @out.
bool OpenEncoder(Job& job, const Options& o, unsigned index, AVStream* out) {
  AVStream* in = job.input->streams[index];
  auto coder = std::make_unique<Job::Coder>();
  const bool video = in->codecpar->codec_type == AVMEDIA_TYPE_VIDEO;
  const AVCodec* decoder = avcodec_find_decoder(in->codecpar->codec_id);
  if (!decoder) return job.Fail(std::string("decoder is not built in: ") + avcodec_get_name(in->codecpar->codec_id));
  coder->decoder = avcodec_alloc_context3(decoder);
  if (!coder->decoder) return job.Fail("allocate decoder");
  if (!job.Check(avcodec_parameters_to_context(coder->decoder, in->codecpar), "set decoder parameters")) return false;
  coder->decoder->pkt_timebase = in->time_base;
  if (!job.Check(avcodec_open2(coder->decoder, decoder, nullptr), "open decoder")) return false;
  const AVCodec* encoder = video ? avcodec_find_encoder_by_name("libopenh264") : avcodec_find_encoder(AV_CODEC_ID_AAC);
  if (!encoder) return job.Fail(video ? "H.264 encoder is not built in" : "AAC encoder is not built in");
  AVCodecContext* e = coder->encoder = avcodec_alloc_context3(encoder);
  if (!e) return job.Fail("allocate encoder");
  if (video) {
    // OpenH264 takes even sizes: an odd source loses its last row or column.
    e->width = o.width ? o.width : coder->decoder->width & ~1;
    e->height = o.height ? o.height : coder->decoder->height & ~1;
    if (e->width < 2 || e->height < 2 || e->width > 8192 || e->height > 8192) return job.Fail("video dimensions must be 2..8192");
    e->pix_fmt = AV_PIX_FMT_YUV420P;
    e->time_base = in->time_base;
    AVRational rate = av_guess_frame_rate(job.input, in, nullptr);
    if (rate.num <= 0 || rate.den <= 0) rate = AVRational{30, 1};
    e->framerate = rate;
    e->sample_aspect_ratio = in->sample_aspect_ratio;
    e->bit_rate = o.video_bitrate ? o.video_bitrate : 2000000;
    // A key frame every two seconds by default, as live streaming services ask.
    e->gop_size = o.gop ? o.gop : std::max(1, static_cast<int>(std::lround(2 * av_q2d(rate))));
    e->max_b_frames = 0;
  } else {
    int rate = o.rate ? o.rate : coder->decoder->sample_rate;
    // Rates AAC has, the nearest at or above; 48 kHz past them.
    static const int kRates[] = {8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000, 64000, 88200, 96000};
    int chosen = 48000;
    for (int r : kRates) if (r >= rate) { chosen = r; break; }
    if (o.rate && chosen != o.rate) return job.Fail("AAC takes 8000..96000 Hz at its own rates (44100, 48000, ...)");
    e->sample_rate = chosen;
    const int channels = o.channels ? o.channels : std::min(coder->decoder->ch_layout.nb_channels, 8);
    if (channels < 1 || channels > 8) return job.Fail("AAC takes 1..8 channels");
    av_channel_layout_default(&e->ch_layout, channels);
    e->sample_fmt = AV_SAMPLE_FMT_FLTP;
    e->time_base = AVRational{1, chosen};
    e->bit_rate = o.audio_bitrate ? o.audio_bitrate : 64000 * std::min(channels, 2);
  }
  if (job.output->oformat->flags & AVFMT_GLOBALHEADER) e->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
  if (!job.Check(avcodec_open2(e, encoder, nullptr), video ? "open H.264 encoder" : "open AAC encoder")) return false;
  if (!job.Check(avcodec_parameters_from_context(out->codecpar, e), "set encoder parameters")) return false;
  out->time_base = e->time_base;
  if (video) out->avg_frame_rate = e->framerate;
  coder->converted = av_frame_alloc();
  if (!coder->converted) return job.Fail("allocate frame");
  if (!video) {
    coder->fifo = av_audio_fifo_alloc(e->sample_fmt, e->ch_layout.nb_channels, std::max(e->frame_size, 1024));
    if (!coder->fifo) return job.Fail("allocate audio queue");
  }
  coder->output = out->index;
  job.coders[index] = std::move(coder);
  return true;
}
// What an encoder has ready, written to its output stream.
bool Drain(Job& job, Job::Coder& c) {
  for (;;) {
    int r = avcodec_receive_packet(c.encoder, job.packet);
    if (r == AVERROR(EAGAIN) || r == AVERROR_EOF) return true;
    if (!job.Check(r, "encode")) return false;
    AVStream* out = job.output->streams[c.output];
    av_packet_rescale_ts(job.packet, c.encoder->time_base, out->time_base);
    job.packet->stream_index = c.output;
    if (!job.Check(av_interleaved_write_frame(job.output, job.packet), "mux packet")) return false;
  }
}
// Audio from the queue to the encoder, a frame of the encoder's size at a
// time; at the end (@flush) what is left, as a shorter last frame.
bool EncodeAudio(Job& job, Job::Coder& c, bool flush) {
  AVCodecContext* e = c.encoder;
  const int size = e->frame_size > 0 ? e->frame_size : 1024;
  while (av_audio_fifo_size(c.fifo) >= size || (flush && av_audio_fifo_size(c.fifo) > 0)) {
    const int n = std::min(size, av_audio_fifo_size(c.fifo));
    AVFrame* f = c.converted;
    av_frame_unref(f);
    f->nb_samples = n;
    f->format = e->sample_fmt;
    f->sample_rate = e->sample_rate;
    if (!job.Check(av_channel_layout_copy(&f->ch_layout, &e->ch_layout), "copy channel layout") ||
        !job.Check(av_frame_get_buffer(f, 0), "allocate audio frame")) return false;
    if (av_audio_fifo_read(c.fifo, reinterpret_cast<void**>(f->data), n) != n) return job.Fail("read audio queue");
    f->pts = c.next;
    c.next += n;
    if (!job.Check(avcodec_send_frame(e, f), "send AAC encoder frame") || !Drain(job, c)) return false;
  }
  return true;
}
// A decoded frame of input stream @index, after -ss and -t, to its encoder.
bool Encode(Job& job, const Options& o, unsigned index, Job::Coder& c, AVFrame* f) {
  AVStream* in = job.input->streams[index];
  const int64_t stamp = f->best_effort_timestamp;
  const int64_t when = stamp == AV_NOPTS_VALUE ? INT64_MIN : av_rescale_q(stamp, in->time_base, AVRational{1, 1000000});
  if (when != INT64_MIN && (when < o.start || (o.duration >= 0 && when >= o.start + o.duration))) return true;
  AVCodecContext* e = c.encoder;
  if (c.encoder->codec_type == AVMEDIA_TYPE_VIDEO) {
    // Times from the start (-ss), in the input's units, which the encoder has.
    int64_t pts = stamp == AV_NOPTS_VALUE ? (c.last == AV_NOPTS_VALUE ? 0 : c.last + 1)
                                          : stamp - av_rescale_q(o.start, AVRational{1, 1000000}, in->time_base);
    if (c.last != AV_NOPTS_VALUE && pts <= c.last) return true;  // out of order or repeated: left out
    c.scaler = sws_getCachedContext(c.scaler, f->width, f->height, static_cast<AVPixelFormat>(f->format),
                                    e->width, e->height, e->pix_fmt, SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!c.scaler) return job.Fail("initialize video scaler");
    AVFrame* out = c.converted;
    av_frame_unref(out);
    out->format = e->pix_fmt; out->width = e->width; out->height = e->height;
    if (!job.Check(av_frame_get_buffer(out, 0), "allocate video frame")) return false;
    if (!job.Check(sws_scale(c.scaler, f->data, f->linesize, 0, f->height, out->data, out->linesize), "scale video")) return false;
    out->pts = pts;
    c.last = pts;
    return job.Check(avcodec_send_frame(e, out), "send H.264 encoder frame") && Drain(job, c);
  }
  if (!c.resampler) {
    if (!job.Check(swr_alloc_set_opts2(&c.resampler, &e->ch_layout, e->sample_fmt, e->sample_rate, &f->ch_layout,
                                       static_cast<AVSampleFormat>(f->format), f->sample_rate, 0, nullptr), "configure resampler") ||
        !job.Check(swr_init(c.resampler), "initialize resampler")) return false;
  }
  if (c.next == AV_NOPTS_VALUE)
    c.next = when == INT64_MIN ? 0 : std::max<int64_t>(0, av_rescale_q(when - o.start, AVRational{1, 1000000}, e->time_base));
  const int capacity = swr_get_out_samples(c.resampler, f->nb_samples);
  if (capacity < 0 || capacity > 4 * 1024 * 1024) return job.Fail("invalid resampler output size");
  uint8_t** planes = nullptr;
  if (av_samples_alloc_array_and_samples(&planes, nullptr, e->ch_layout.nb_channels, std::max(capacity, 1), e->sample_fmt, 0) < 0)
    return job.Fail("allocate audio samples");
  const int n = swr_convert(c.resampler, planes, capacity, const_cast<const uint8_t**>(f->extended_data), f->nb_samples);
  const bool queued = n >= 0 && av_audio_fifo_write(c.fifo, reinterpret_cast<void**>(planes), n) == n;
  av_freep(&planes[0]);
  av_freep(&planes);
  if (n < 0) return job.Check(n, "resample audio");
  if (!queued) return job.Fail("queue audio");
  return EncodeAudio(job, c, false);
}
// Frames still in a decoder and an encoder, at the end.
bool Flush(Job& job, const Options& o, unsigned index, Job::Coder& c) {
  if (!job.Check(avcodec_send_packet(c.decoder, nullptr), "flush decoder")) return false;
  for (;;) {
    int r = avcodec_receive_frame(c.decoder, job.frame);
    if (r == AVERROR_EOF || r == AVERROR(EAGAIN)) break;
    if (!job.Check(r, "decode frame")) return false;
    const bool ok = Encode(job, o, index, c, job.frame);
    av_frame_unref(job.frame);
    if (!ok) return false;
  }
  if (c.encoder->codec_type == AVMEDIA_TYPE_AUDIO && c.resampler) {
    // What the resampler holds back, then the queue's rest.
    const int capacity = swr_get_out_samples(c.resampler, 0);
    if (capacity > 0) {
      uint8_t** planes = nullptr;
      if (av_samples_alloc_array_and_samples(&planes, nullptr, c.encoder->ch_layout.nb_channels, capacity, c.encoder->sample_fmt, 0) < 0)
        return job.Fail("allocate audio samples");
      const int n = swr_convert(c.resampler, planes, capacity, nullptr, 0);
      if (n > 0) av_audio_fifo_write(c.fifo, reinterpret_cast<void**>(planes), n);
      av_freep(&planes[0]);
      av_freep(&planes);
    }
    if (!EncodeAudio(job, c, true)) return false;
  }
  return job.Check(avcodec_send_frame(c.encoder, nullptr), "flush encoder") && Drain(job, c);
}
// Copies or encodes each selected stream into the output: a file (staged in
// job.temporary) or a live stream's address.
bool Mux(Job& job, Options& o) {
  const std::string format = Format(o);
  const bool live = IsStream(o.target);
  static const char* const kFormats[] = {"mp4", "mov", "matroska", "webm", "ipod", "flv", "adts", "rtsp"};
  if (std::find_if(std::begin(kFormats), std::end(kFormats), [&](const char* f) { return format == f; }) == std::end(kFormats))
    return job.Fail("output format must be mp4, mov, matroska, webm, ipod, flv, adts or rtsp");
  if (format == "rtsp" && !IsRtsp(o.target)) return job.Fail("rtsp output needs an rtsp:// or rtsps:// address");
  if (IsRtsp(o.target) && format != "rtsp") return job.Fail("an rtsp:// address takes the rtsp format");
  if (live && !IsRtsp(o.target) && format != "flv") return job.Fail("an rtmp address takes the flv format");
  if (!job.Check(avformat_alloc_output_context2(&job.output, nullptr, format.c_str(), live ? o.target.c_str() : nullptr), "allocate muxer")) return false;
  job.output->interrupt_callback = job.Interrupter();
  std::vector<int> mapping(job.input->nb_streams, -1);
  job.coders.resize(job.input->nb_streams);
  for (unsigned i = 0; i < job.input->nb_streams; ++i) {
    AVStream* in = job.input->streams[i];
    if (!Selected(o, in, i)) continue;
    AVStream* out = avformat_new_stream(job.output, nullptr);
    if (!out) return job.Fail("allocate output stream");
    mapping[i] = out->index;
    const bool video = in->codecpar->codec_type == AVMEDIA_TYPE_VIDEO;
    const std::string& codec = video ? o.video : o.audio;
    if (codec != "copy" && (in->codecpar->codec_type == AVMEDIA_TYPE_VIDEO || in->codecpar->codec_type == AVMEDIA_TYPE_AUDIO)) {
      if (!OpenEncoder(job, o, i, out)) return false;
    } else {
      if (!job.Check(avcodec_parameters_copy(out->codecpar, in->codecpar), "copy codec parameters")) return false;
      out->codecpar->codec_tag = 0; out->time_base = in->time_base;
      out->avg_frame_rate = in->avg_frame_rate;
      out->sample_aspect_ratio = in->sample_aspect_ratio;
    }
    out->disposition = in->disposition;
    av_dict_copy(&out->metadata, in->metadata, 0);
  }
  if (!job.output->nb_streams) return job.Fail("no selected streams");
  // Encoded streams are decoded first, into this frame.
  if (!job.frame && !(job.frame = av_frame_alloc())) return job.Fail("allocate frame");
  av_dict_copy(&job.output->metadata, job.input->metadata, 0);
  AVDictionary* options = job.NetworkFor(o.target);
  struct Free { AVDictionary** d; ~Free() { av_dict_free(d); } } free_options{&options};
  if (!(job.output->oformat->flags & AVFMT_NOFILE)) {
    const std::string where = live ? o.target : "file:" + job.temporary;
    const AVIOInterruptCB interrupt = job.Interrupter();
    if (!job.Check(avio_open2(&job.output->pb, where.c_str(), AVIO_FLAG_WRITE, &interrupt, live ? &options : nullptr), "open muxer output"))
      return false;
  }
  // RTSP's muxer connects in its header, with the stream's options.
  if (!job.Check(avformat_write_header(job.output, format == "rtsp" ? &options : nullptr), "write container header")) return false;
  int r;
  // -ss and -t: copied streams as `ffmpeg -ss -t -c copy`, from the key frame
  // at or before the start, each stream's times from there, packets from the
  // end on left out; encoded streams frame by frame, from the start itself.
  const bool cut = o.start > 0 || o.duration >= 0;
  const int64_t end = o.duration >= 0 ? o.start + o.duration : INT64_MAX;
  std::vector<int64_t> first(job.input->nb_streams, AV_NOPTS_VALUE);
  // Each copied stream's last time written, in the output's units.
  std::vector<int64_t> last(job.input->nb_streams, AV_NOPTS_VALUE);
  int64_t written = 0;
  bool reached = false;
  Pace pace;
  if (o.start > 0 && !job.Check(av_seek_frame(job.input, -1, av_rescale_q(o.start, AVRational{1, 1000000}, AV_TIME_BASE_Q), AVSEEK_FLAG_BACKWARD), "seek in the input")) return false;
  while ((r = av_read_frame(job.input, job.packet)) >= 0) {
    int i = job.packet->stream_index;
    ++job.packets;
    // A demuxer may add a stream while reading (Ogg); it has no mapping.
    if (i < 0 || static_cast<size_t>(i) >= mapping.size()) { av_packet_unref(job.packet); continue; }
    AVStream* in = job.input->streams[i];
    const int64_t stamp = job.packet->dts != AV_NOPTS_VALUE ? job.packet->dts : job.packet->pts;
    int64_t time = job.packet->pts == AV_NOPTS_VALUE ? INT64_MIN : av_rescale_q(job.packet->pts, in->time_base, AVRational{1, 1000000});
    if (!job.Progress(time)) return false;
    if (o.realtime && mapping[i] >= 0 &&
        !pace.Wait(job, stamp == AV_NOPTS_VALUE ? INT64_MIN : av_rescale_q(stamp, in->time_base, AVRational{1, 1000000}))) return false;
    if (mapping[i] >= 0 && job.coders[i]) {
      Job::Coder& c = *job.coders[i];
      // Past the end: this stream is done; another may not be.
      const int64_t when = job.packet->pts != AV_NOPTS_VALUE ? job.packet->pts : job.packet->dts;
      if (when != AV_NOPTS_VALUE && av_rescale_q(when, in->time_base, AVRational{1, 1000000}) >= end) { av_packet_unref(job.packet); continue; }
      int sent = avcodec_send_packet(c.decoder, job.packet);
      av_packet_unref(job.packet);
      // A damaged packet of a live stream is passed over, as ffmpeg does.
      if (sent == AVERROR_INVALIDDATA) continue;
      if (!job.Check(sent, "send decoder packet")) return false;
      for (;;) {
        int got = avcodec_receive_frame(c.decoder, job.frame);
        if (got == AVERROR(EAGAIN) || got == AVERROR_EOF) break;
        if (got == AVERROR_INVALIDDATA) continue;
        if (!job.Check(got, "decode frame")) return false;
        const int64_t at = job.frame->best_effort_timestamp;
        if (at != AV_NOPTS_VALUE && av_rescale_q(at + std::max<int64_t>(job.frame->duration, 0), in->time_base, AVRational{1, 1000000}) > o.start) reached = true;
        const bool ok = Encode(job, o, i, c, job.frame);
        av_frame_unref(job.frame);
        if (!ok) return false;
        ++written;
      }
      continue;
    }
    if (mapping[i] >= 0 && cut) {
      const int64_t when = job.packet->pts != AV_NOPTS_VALUE ? job.packet->pts : job.packet->dts;
      // Another stream may still have packets before the end.
      if (when != AV_NOPTS_VALUE && av_rescale_q(when, in->time_base, AVRational{1, 1000000}) >= end) { av_packet_unref(job.packet); continue; }
      if (first[i] == AV_NOPTS_VALUE) {
        if (!(job.packet->flags & AV_PKT_FLAG_KEY) && in->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) { av_packet_unref(job.packet); continue; }
        first[i] = job.packet->dts != AV_NOPTS_VALUE ? job.packet->dts : when;
      }
      // A start past the end still seeks back to the last key frame.
      if (when != AV_NOPTS_VALUE && av_rescale_q(when + job.packet->duration, in->time_base, AVRational{1, 1000000}) > o.start) reached = true;
      if (first[i] != AV_NOPTS_VALUE) {
        if (job.packet->pts != AV_NOPTS_VALUE) job.packet->pts -= first[i];
        if (job.packet->dts != AV_NOPTS_VALUE) job.packet->dts -= first[i];
      }
    }
    if (mapping[i] >= 0) {
      AVStream* out = job.output->streams[mapping[i]];
      av_packet_rescale_ts(job.packet, in->time_base, out->time_base);
      // A live stream's packets may come without times (RTP's before its
      // first sender report) or out of order, which muxers refuse: as the
      // ffmpeg command does, a missing one from the other, none at all left
      // out, and decode times kept rising.
      AVPacket* p = job.packet;
      if (p->pts == AV_NOPTS_VALUE) p->pts = p->dts;
      if (p->dts == AV_NOPTS_VALUE) p->dts = p->pts;
      if (p->dts == AV_NOPTS_VALUE) { av_packet_unref(p); continue; }
      if (last[i] != AV_NOPTS_VALUE && p->dts <= last[i]) {
        p->dts = last[i] + 1;
        if (p->pts < p->dts) p->pts = p->dts;
      }
      last[i] = p->dts;
      ++written;
      job.packet->stream_index = out->index; job.packet->pos = -1;
      if (!job.Check(av_interleaved_write_frame(job.output, job.packet), "mux packet")) return false;
    }
    av_packet_unref(job.packet);
  }
  // A live input that ends (its publisher stopped) ends the job as a file does.
  if (r != AVERROR_EOF && !(IsStream(o.source) && !job.cancelled && r == AVERROR(EIO))) return job.Check(r, "read packet");
  for (unsigned i = 0; i < job.coders.size(); ++i)
    if (job.coders[i] && !Flush(job, o, i, *job.coders[i])) return false;
  if (cut && (!written || !reached)) return job.Fail("nothing in the input lies between those times");
  if (!written) return job.Fail("nothing was written");
  if (!job.Check(av_write_trailer(job.output), "write container trailer")) return false;
  if (job.output->oformat->flags & AVFMT_NOFILE) return true;
  return job.Check(avio_closep(&job.output->pb), "close container");
}
// WAV is a small explicit PCM container; no encoder or WAV muxer is needed.
std::vector<uint8_t> Wave(uint32_t bytes, int rate, int channels) {
  std::vector<uint8_t> b;
  auto text = [&](const char* s) { b.insert(b.end(), s, s + 4); };
  auto put = [&](uint32_t n, int size) { for (int i = 0; i < size; ++i) b.push_back(n >> (8 * i)); };
  text("RIFF"); put(bytes + 36, 4); text("WAVE"); text("fmt "); put(16, 4); put(1, 2); put(channels, 2);
  put(rate, 4); put(rate * channels * 2, 4); put(channels * 2, 2); put(16, 2); text("data"); put(bytes, 4);
  return b;
}
bool Decode(Job& job, const Options& o) {
  bool audio = o.audio == "pcm_s16le";
  if ((!audio && !o.format.empty() && o.format != "rawvideo") ||
      (audio && !o.format.empty() && o.format != "wav" && o.format != "s16le")) return job.Fail("unsupported decoded output format");
  bool wav = audio && o.format != "s16le";
  int stream = -1;
  for (unsigned i = 0; i < job.input->nb_streams; ++i) {
    AVStream* s = job.input->streams[i];
    if (s->codecpar->codec_type == (audio ? AVMEDIA_TYPE_AUDIO : AVMEDIA_TYPE_VIDEO) && Selected(o, s, i)) { stream = i; break; }
  }
  if (stream < 0) return job.Fail("no selected stream of the requested type");
  if (o.maps.size() > 1 || (!o.maps.empty() && o.maps[0] != stream)) return job.Fail("decoded output requires exactly one stream");
  const AVCodec* codec = avcodec_find_decoder(job.input->streams[stream]->codecpar->codec_id);
  if (!codec) return job.Fail("decoder is not built in");
  job.decoder = avcodec_alloc_context3(codec); job.frame = av_frame_alloc();
  if (!job.decoder || !job.frame) return job.Fail("allocate decoder");
  if (!job.Check(avcodec_parameters_to_context(job.decoder, job.input->streams[stream]->codecpar), "set decoder parameters") ||
      !job.Check(avcodec_open2(job.decoder, codec, nullptr), "open decoder")) return false;
  int rate = o.rate ? o.rate : job.decoder->sample_rate;
  int channels = o.channels ? o.channels : job.decoder->ch_layout.nb_channels;
  if (audio && (rate < 1 || rate > 384000 || channels < 1 || channels > 32)) return job.Fail("invalid audio format");
  if (wav) { auto header = Wave(0, rate, channels); if (!job.Write(header.data(), header.size())) return false; }
  uint64_t bytes = 0;
  int frames = 0, width = o.width, height = o.height;
  AVChannelLayout input_layout{};
  struct LayoutGuard { AVChannelLayout* layout; ~LayoutGuard() { av_channel_layout_uninit(layout); } } guard{&input_layout};
  int input_rate = 0, input_format = -1;
  auto samples = [&](const uint8_t** source, int count) {
    int capacity = swr_get_out_samples(job.resampler, count);
    if (capacity < 0 || capacity > 4 * 1024 * 1024) return job.Fail("invalid resampler output size");
    std::vector<uint8_t> buffer(static_cast<size_t>(capacity) * channels * 2);
    uint8_t* dest = buffer.data();
    int n = swr_convert(job.resampler, &dest, capacity, source, count);
    if (!job.Check(n, "resample audio")) return false;
    size_t size = static_cast<size_t>(n) * channels * 2;
    if (wav && bytes + size > UINT32_MAX - 36) return job.Fail("WAV exceeds RIFF 4 GiB limit; use s16le");
    bytes += size;
    return job.Write(buffer.data(), size);
  };
  auto receive = [&]() {
    for (;;) {
      int r = avcodec_receive_frame(job.decoder, job.frame);
      if (r == AVERROR(EAGAIN) || r == AVERROR_EOF) return true;
      if (!job.Check(r, "decode frame")) return false;
      AVFrame* f = job.frame;
      // What decoding needed from the key frame on, before the start or after
      // the end, is not output.
      if (o.start > 0 || o.duration >= 0) {
        const int64_t when = f->best_effort_timestamp == AV_NOPTS_VALUE ? INT64_MIN : av_rescale_q(f->best_effort_timestamp, job.input->streams[stream]->time_base, AVRational{1, 1000000});
        if (when != INT64_MIN && (when < o.start || (o.duration >= 0 && when >= o.start + o.duration))) { av_frame_unref(f); continue; }
      }
      if (audio) {
        if (!job.resampler) {
          AVChannelLayout layout{}; av_channel_layout_default(&layout, channels);
          int result = swr_alloc_set_opts2(&job.resampler, &layout, AV_SAMPLE_FMT_S16, rate, &f->ch_layout,
              static_cast<AVSampleFormat>(f->format), f->sample_rate, 0, nullptr);
          av_channel_layout_uninit(&layout);
          if (!job.Check(result, "configure resampler") || !job.Check(swr_init(job.resampler), "initialize resampler")) return false;
          if (!job.Check(av_channel_layout_copy(&input_layout, &f->ch_layout), "copy channel layout")) return false;
          input_rate = f->sample_rate; input_format = f->format;
        } else if (input_rate != f->sample_rate || input_format != f->format || av_channel_layout_compare(&input_layout, &f->ch_layout))
          return job.Fail("audio format changed; use the low-level API for reconfiguration");
        if (!samples(const_cast<const uint8_t**>(f->extended_data), f->nb_samples)) return false;
      } else {
        if (!width) { width = f->width; height = f->height; }
        if (width < 1 || height < 1 || width > 8192 || height > 8192) return job.Fail("video dimensions exceed 8192");
        job.scaler = sws_getCachedContext(job.scaler, f->width, f->height, static_cast<AVPixelFormat>(f->format),
            width, height, AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!job.scaler) return job.Fail("initialize video scaler");
        std::vector<uint8_t> buffer(static_cast<size_t>(width) * height * 4);
        uint8_t* dest[] = {buffer.data(), nullptr, nullptr, nullptr}; int strides[] = {width * 4, 0, 0, 0};
        const int scaled = sws_scale(job.scaler, f->data, f->linesize, 0, f->height, dest, strides);
        if (!job.Check(scaled, "scale video")) return false;
        if (scaled != height) return job.Fail("scale video: a partial frame");
        if (!job.Write(buffer.data(), buffer.size())) return false;
      }
      ++frames; av_frame_unref(f);
      if (!audio && o.frames && frames >= o.frames) return true;
    }
  };
  int r = AVERROR_EOF;
  if (o.start > 0 && !job.Check(av_seek_frame(job.input, -1, av_rescale_q(o.start, AVRational{1, 1000000}, AV_TIME_BASE_Q), AVSEEK_FLAG_BACKWARD), "seek in the input")) return false;
  while ((r = av_read_frame(job.input, job.packet)) >= 0) {
    ++job.packets;
    AVStream* current = job.input->streams[job.packet->stream_index];
    int64_t timestamp = job.packet->pts == AV_NOPTS_VALUE ? INT64_MIN : av_rescale_q(job.packet->pts, current->time_base, AVRational{1, 1000000});
    if (!job.Progress(timestamp)) return false;
    if (job.packet->stream_index == stream) {
      if (!job.Check(avcodec_send_packet(job.decoder, job.packet), "send decoder packet") || !receive()) return false;
    }
    av_packet_unref(job.packet);
    if (!audio && o.frames && frames >= o.frames) break;
  }
  if (r < 0 && r != AVERROR_EOF) return job.Check(r, "read packet");
  if (audio || !o.frames || frames < o.frames) {
    if (!job.Check(avcodec_send_packet(job.decoder, nullptr), "flush decoder") || !receive()) return false;
  }
  if (!frames) return job.Fail("no decoded frames");
  if (audio && job.resampler && !samples(nullptr, 0)) return false;
  if (wav) {
    if (fseek(job.file, 0, SEEK_SET)) return job.Fail("seek WAV header");
    auto header = Wave(static_cast<uint32_t>(bytes), rate, channels);
    if (!job.Write(header.data(), header.size())) return false;
  }
  return true;
}
}  // namespace

extern "C" int zebkit_ffprobe(int argc, const char* const* argv, char** json, char** error) {
  if (json) *json = nullptr;
  if (error) *error = nullptr;
  Job job;
  if (!json) return Result(job, job.Fail("json output is required"), error);
  if (!Arguments(argc, argv, job)) return Result(job, false, error);
  std::string source;
  bool format = false, streams = false, end = false;
  for (int i = 0; i < argc; ++i) {
    std::string a = argv[i];
    if (!end && a == "--") { end = true; continue; }
    if (!end && a == "-show_format") { format = true; continue; }
    if (!end && a == "-show_streams") { streams = true; continue; }
    if (!end && (a == "-of" || a == "-print_format" || a == "-i")) {
      if (++i == argc) return Result(job, job.Fail("missing value for " + a), error);
      if (a == "-i") { if (!source.empty()) return Result(job, job.Fail("only one input is supported"), error); source = argv[i]; }
      else if (argv[i] != std::string("json")) return Result(job, job.Fail("only JSON output is supported"), error);
    } else if (!end && a[0] == '-') return Result(job, job.Fail("unsupported option: " + a), error);
    else { if (!source.empty()) return Result(job, job.Fail("only one input is supported"), error); source = a; }
  }
  if (source.empty()) return Result(job, job.Fail("input is required"), error);
  if (!job.Open(source)) return Result(job, false, error);
  if (!format && !streams) format = streams = true;
  std::string out = "{";
  if (streams) {
    out += "\"streams\":[";
    for (unsigned i = 0; i < job.input->nb_streams; ++i) {
      AVStream* s = job.input->streams[i]; AVCodecParameters* p = s->codecpar;
      if (i) out += ',';
      out += "{\"index\":" + std::to_string(i) + ",\"codec_name\":" + Quote(avcodec_get_name(p->codec_id)) +
          ",\"codec_type\":" + Quote(av_get_media_type_string(p->codec_type)) + ",\"time_base\":" + Quote(Rational(s->time_base).c_str()) +
          ",\"start_time\":" + Time(s->start_time, s->time_base) + ",\"duration\":" + Time(s->duration, s->time_base) +
          ",\"bit_rate\":" + Quote(std::to_string(p->bit_rate).c_str()) + ",\"tags\":" + Tags(s->metadata);
      if (p->codec_type == AVMEDIA_TYPE_VIDEO) out += ",\"width\":" + std::to_string(p->width) + ",\"height\":" + std::to_string(p->height) +
          ",\"pix_fmt\":" + Quote(av_get_pix_fmt_name(static_cast<AVPixelFormat>(p->format))) + ",\"avg_frame_rate\":" + Quote(Rational(s->avg_frame_rate).c_str());
      if (p->codec_type == AVMEDIA_TYPE_AUDIO) out += ",\"sample_rate\":" + Quote(std::to_string(p->sample_rate).c_str()) + ",\"channels\":" + std::to_string(p->ch_layout.nb_channels);
      out += '}';
    }
    out += ']';
  }
  if (format) {
    if (streams) out += ',';
    out += "\"format\":{\"filename\":" + Quote(source.c_str()) + ",\"format_name\":" + Quote(job.input->iformat->name) +
        ",\"nb_streams\":" + std::to_string(job.input->nb_streams) + ",\"duration\":" + Time(job.input->duration, AVRational{1, 1000000}) +
        ",\"start_time\":" + Time(job.input->start_time, AVRational{1, 1000000}) + ",\"size\":" + Quote(std::to_string((job.input->pb ? avio_size(job.input->pb) : -1)).c_str()) +
        ",\"bit_rate\":" + Quote(std::to_string(job.input->bit_rate).c_str()) + ",\"tags\":" + Tags(job.input->metadata) + '}';
  }
  out += '}'; *json = strdup(out.c_str());
  return Result(job, *json || job.Fail("allocate JSON"), error);
}
extern "C" int zebkit_media_probe(const char* path, char** json, char** error) {
  const char* const arguments[] = {"--", path};  // A path, even "-x.mp4".
  return zebkit_ffprobe(2, arguments, json, error);
}
extern "C" int zebkit_ffmpeg(int argc, const char* const* argv, zebkit_media_progress_func progress, void* data, char** error) {
  if (error) *error = nullptr;
  Job job; job.callback = progress; job.data = data;
  Options o;
  bool ok = Parse(argc, argv, o, job) && job.Progress() && job.Open(o.source);
  if (ok) {
    for (size_t i = 0; i < o.maps.size(); ++i) {
      if (o.maps[i] >= static_cast<int>(job.input->nb_streams) || std::find(o.maps.begin(), o.maps.begin() + i, o.maps[i]) != o.maps.begin() + i) {
        ok = job.Fail("invalid or duplicate stream mapping"); break;
      }
    }
  }
  // A live stream's address is written to as it is; a file is staged.
  const bool live = IsStream(o.target);
  if (ok && !live) ok = job.Stage(o.target, o.source, o.overwrite);
  if (ok) ok = o.Decoded() ? Decode(job, o) : Mux(job, o);
  if (ok) ok = job.Progress() && (live || job.Publish(o.target, o.overwrite));
  return Result(job, ok, error);
}
