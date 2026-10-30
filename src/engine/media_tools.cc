#include "include/zebkit/media_tools.h"
#include <algorithm>
#include <cerrno>
#include <fcntl.h>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>
#include <sys/stat.h>
#include <unistd.h>

// The vendor variant lets the same implementation be integration-tested
// against a packaged engine, without rebuilding Chromium.
#ifdef ZEBKIT_MEDIA_TOOLS_VENDOR
#include <zebkit/vendor/ffmpeg.h>
using namespace zebkit::vendor::ffmpeg;
#else
extern "C" {
#include "third_party/ffmpeg/libavcodec/avcodec.h"
#include "third_party/ffmpeg/libavformat/avformat.h"
#include "third_party/ffmpeg/libavutil/channel_layout.h"
#include "third_party/ffmpeg/libavutil/imgutils.h"
#include "third_party/ffmpeg/libavutil/opt.h"
#include "third_party/ffmpeg/libavutil/pixdesc.h"
#include "third_party/ffmpeg/libswresample/swresample.h"
#include "third_party/ffmpeg/libswscale/swscale.h"
}
#endif

namespace {
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
    if (output) { if (output->pb) avio_closep(&output->pb); avformat_free_context(output); }
    avformat_close_input(&input);
    avcodec_free_context(&decoder);
    av_frame_free(&frame);
    av_packet_free(&packet);
    swr_free(&resampler);
    sws_freeContext(scaler);
    if (!temporary.empty()) unlink(temporary.c_str());
  }
  bool Fail(const std::string& s) { failure = s; return false; }
  bool Check(int code, const char* what) {
    if (code >= 0) return true;
    char s[AV_ERROR_MAX_STRING_SIZE]; av_strerror(code, s, sizeof(s));
    return Fail(std::string(what) + ": " + s);
  }
  bool Progress(int64_t time = INT64_MIN) {
    if (callback && !callback(packets, time, data)) { cancelled = true; return Fail("cancelled"); }
    return true;
  }
  bool Open(const std::string& path) {
    struct stat st;
    if (stat(path.c_str(), &st) || !S_ISREG(st.st_mode)) return Fail("input must be a local regular file: " + path);
    // Explicit file: avoids interpreting a colon in a local filename as a protocol.
    if (!Check(avformat_open_input(&input, ("file:" + path).c_str(), nullptr, nullptr), "open input") ||
        !Check(avformat_find_stream_info(input, nullptr), "read stream information")) return false;
    packet = av_packet_alloc();
    return packet || Fail("allocate packet");
  }
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
  std::string source, target, codec, format;
  std::vector<int> maps;
  bool overwrite = false, no_audio = false, no_video = false;
  int rate = 0, channels = 0, width = 0, height = 0, frames = 0;
  bool pixel_format = false;
};
bool Parse(int argc, const char* const* argv, Options& o, Job& job) {
  if (!Arguments(argc, argv, job)) return false;
  bool end = false;
  for (int i = 0; i < argc; ++i) {
    std::string a = argv[i];
    if (!end && a == "--") { end = true; continue; }
    if (!end && a == "-y") { o.overwrite = true; continue; }
    if (!end && a == "-n") { o.overwrite = false; continue; }
    if (!end && a == "-an") { o.no_audio = true; continue; }
    if (!end && a == "-vn") { o.no_video = true; continue; }
    if (!end && a[0] == '-') {
      if (a != "-i" && a != "-map" && a != "-c" && a != "-codec" && a != "-c:a" && a != "-c:v" &&
          a != "-f" && a != "-ar" && a != "-ac" && a != "-s" && a != "-frames:v" && a != "-pix_fmt")
        return job.Fail("unsupported option: " + a);
      if (++i == argc) return job.Fail("missing value for " + a);
      std::string v = argv[i];
      if (a == "-i") { if (!o.source.empty()) return job.Fail("only one input is supported"); o.source = v; }
      else if (a == "-c" || a == "-codec" || a == "-c:a" || a == "-c:v") {
        if (!o.codec.empty()) return job.Fail("choose one output mode/codec");
        if ((a == "-c:a" && v != "pcm_s16le") || (a == "-c:v" && v != "rawvideo"))
          return job.Fail("unsupported codec for stream type");
        // Per-stream mixed codecs are not supported.
        o.codec = v;
      } else if (a == "-f") o.format = v;
      else if (a == "-map") { int n; if (v.rfind("0:", 0) || !Number(v.substr(2), 0, 4095, n)) return job.Fail("map must be 0:STREAM_INDEX"); o.maps.push_back(n); }
      else if (a == "-pix_fmt") { if (v != "rgba") return job.Fail("only rgba is supported"); o.pixel_format = true; }
      else if (a == "-ar") { if (!Number(v, 1, 384000, o.rate)) return job.Fail("invalid sample rate"); }
      else if (a == "-ac") { if (!Number(v, 1, 32, o.channels)) return job.Fail("invalid channel count"); }
      else if (a == "-frames:v") { if (!Number(v, 1, INT_MAX, o.frames)) return job.Fail("invalid frame count"); }
      else if (a == "-s") { auto x = v.find('x'); if (x == std::string::npos || !Number(v.substr(0, x), 1, 8192, o.width) || !Number(v.substr(x + 1), 1, 8192, o.height)) return job.Fail("size must be WIDTHxHEIGHT (1..8192)"); }
    } else { if (!o.target.empty()) return job.Fail("only one output is supported"); o.target = a; }
  }
  if (o.source.empty() || o.target.empty()) return job.Fail("-i INPUT and OUTPUT are required");
  if (o.codec != "copy" && o.codec != "pcm_s16le" && o.codec != "rawvideo") return job.Fail("choose -c copy, -c:a pcm_s16le or -c:v rawvideo; encoders are not built in");
  if (o.codec != "pcm_s16le" && (o.rate || o.channels)) return job.Fail("-ar/-ac require pcm_s16le");
  if (o.codec != "rawvideo" && (o.width || o.frames || o.pixel_format)) return job.Fail("-s/-frames:v/-pix_fmt require rawvideo");
  if ((o.codec == "pcm_s16le" && o.no_audio) || (o.codec == "rawvideo" && o.no_video)) return job.Fail("selected output stream is disabled");
  return true;
}
bool Selected(const Options& o, AVStream* s, int index) {
  if (o.no_audio && s->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) return false;
  if (o.no_video && s->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) return false;
  return o.maps.empty() ? (s->codecpar->codec_type == AVMEDIA_TYPE_AUDIO || s->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) :
      std::find(o.maps.begin(), o.maps.end(), index) != o.maps.end();
}
bool Remux(Job& job, Options& o) {
  std::string format = o.format;
  if (format.empty()) {
    auto dot = o.target.find_last_of('.'); std::string ext = dot == std::string::npos ? "" : o.target.substr(dot + 1);
    if (ext == "mkv") format = "matroska";
    else if (ext == "m4a" || ext == "m4v") format = "ipod";
    else format = ext;
  }
  if (format != "mp4" && format != "mov" && format != "matroska" && format != "webm" && format != "ipod") return job.Fail("copy format must be mp4, mov, matroska, webm or ipod");
  if (!job.Check(avformat_alloc_output_context2(&job.output, nullptr, format.c_str(), nullptr), "allocate muxer")) return false;
  std::vector<int> mapping(job.input->nb_streams, -1);
  for (unsigned i = 0; i < job.input->nb_streams; ++i) {
    AVStream* in = job.input->streams[i];
    if (!Selected(o, in, i)) continue;
    AVStream* out = avformat_new_stream(job.output, nullptr);
    if (!out) return job.Fail("allocate output stream");
    mapping[i] = out->index;
    if (!job.Check(avcodec_parameters_copy(out->codecpar, in->codecpar), "copy codec parameters")) return false;
    out->codecpar->codec_tag = 0; out->time_base = in->time_base;
    out->disposition = in->disposition;
    out->avg_frame_rate = in->avg_frame_rate;
    out->sample_aspect_ratio = in->sample_aspect_ratio;
    av_dict_copy(&out->metadata, in->metadata, 0);
  }
  if (!job.output->nb_streams) return job.Fail("no selected streams");
  av_dict_copy(&job.output->metadata, job.input->metadata, 0);
  if (!job.Check(avio_open(&job.output->pb, ("file:" + job.temporary).c_str(), AVIO_FLAG_WRITE), "open muxer output") ||
      !job.Check(avformat_write_header(job.output, nullptr), "write container header")) return false;
  int r;
  while ((r = av_read_frame(job.input, job.packet)) >= 0) {
    int i = job.packet->stream_index;
    ++job.packets;
    // A demuxer may add a stream while reading (Ogg); it has no mapping.
    if (i < 0 || static_cast<size_t>(i) >= mapping.size()) { av_packet_unref(job.packet); continue; }
    AVStream* in = job.input->streams[i];
    int64_t time = job.packet->pts == AV_NOPTS_VALUE ? INT64_MIN : av_rescale_q(job.packet->pts, in->time_base, AVRational{1, 1000000});
    if (!job.Progress(time)) return false;
    if (mapping[i] >= 0) {
      AVStream* out = job.output->streams[mapping[i]];
      av_packet_rescale_ts(job.packet, in->time_base, out->time_base);
      job.packet->stream_index = out->index; job.packet->pos = -1;
      if (!job.Check(av_interleaved_write_frame(job.output, job.packet), "mux packet")) return false;
    }
    av_packet_unref(job.packet);
  }
  if (r != AVERROR_EOF) return job.Check(r, "read packet");
  if (!job.Check(av_write_trailer(job.output), "write container trailer")) return false;
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
  bool audio = o.codec == "pcm_s16le";
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
  if (ok) ok = job.Stage(o.target, o.source, o.overwrite);
  if (ok) ok = o.codec == "copy" ? Remux(job, o) : Decode(job, o);
  if (ok) ok = job.Progress() && job.Publish(o.target, o.overwrite);
  return Result(job, ok, error);
}
