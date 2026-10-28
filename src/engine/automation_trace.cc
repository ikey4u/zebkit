// zebkit's automation, what a page's run leaves (automation_trace.h).

#ifdef UNSAFE_BUFFERS_BUILD
// libvpx's and FFmpeg's buffers are C's.
#pragma allow_unsafe_buffers
#endif

#include "zebkit/engine/automation_trace.h"

#include <algorithm>
#include <utility>

#include "base/base64.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/functional/bind.h"
#include "base/hash/sha1.h"
#include "base/json/json_writer.h"
#include "base/rand_util.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/task/sequenced_task_runner.h"
#include "base/task/thread_pool.h"
#include "components/viz/common/frame_sinks/copy_output_result.h"
#include "content/public/browser/render_widget_host_view.h"
#include "content/public/browser/web_contents.h"
#include "media/ffmpeg/ffmpeg_common.h"
#include "third_party/libvpx/source/libvpx/vpx/vp8cx.h"
#include "third_party/libvpx/source/libvpx/vpx/vpx_encoder.h"
#include "third_party/libyuv/include/libyuv/convert_from_argb.h"
#include "third_party/libyuv/include/libyuv/scale_argb.h"
#include "third_party/skia/include/core/SkBitmap.h"
#include "third_party/zlib/google/zip.h"
#include "zebkit/engine/include/zebkit/zebkit.h"

namespace zebkit {

namespace {

std::string Json(const base::Value& value) {
  return base::WriteJson(value).value_or("null");
}

// Where an export goes: @path, or a new file in the temporary directory.
base::FilePath OutputPath(base::FilePath path, const char* extension) {
  if (!path.empty())
    return path;
  base::FilePath directory;
  base::GetTempDir(&directory);
  return directory.AppendASCII("zebkit-" + base::NumberToString(base::RandUint64()) +
                               extension);
}

void Reply(WrittenCallback done, base::FilePath path, bool ok) {
  std::move(done).Run(path, ok ? std::string() : "cannot write " + path.AsUTF8Unsafe());
}

std::string Iso8601(base::Time time) {
  base::Time::Exploded parts;
  time.UTCExplode(&parts);
  char text[32];
  snprintf(text, sizeof(text), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", parts.year,
           parts.month, parts.day_of_month, parts.hour, parts.minute,
           parts.second, parts.millisecond);
  return text;
}

base::ListValue Headers(const base::DictValue& from) {
  const base::ListValue* headers = from.FindList("headers");
  return headers ? headers->Clone() : base::ListValue();
}

}  // namespace

// ---- HAR ----------------------------------------------------------------

HarRecorder::HarRecorder(const base::DictValue& options) {
  const std::string* content = options.FindString("content");
  embed_ = !content || *content == "embed";
}

HarRecorder::~HarRecorder() = default;
HarRecorder::HarRecorder(HarRecorder&&) = default;
HarRecorder& HarRecorder::operator=(HarRecorder&&) = default;

void HarRecorder::Record(const std::string& event,
                         const base::DictValue& params) {
  const std::string* id = params.FindString("request");
  if (!id)
    return;
  if (event == "Page.request") {
    Entry& entry = entries_[*id];
    entry.started = base::Time::Now();
    entry.start = base::TimeTicks::Now();
    entry.request = params.Clone();
    order_.push_back(*id);
    return;
  }
  auto found = entries_.find(*id);
  if (found == entries_.end())
    return;
  Entry& entry = found->second;
  if (event == "Page.response") {
    entry.responded = base::TimeTicks::Now();
    entry.response = params.Clone();
  } else if (event == "Page.requestFinished" || event == "Page.requestFailed") {
    entry.ended = base::TimeTicks::Now();
    entry.finished = true;
    if (const std::string* failure = params.FindString("failureText"))
      entry.failure = *failure;
  }
}

base::ListValue HarRecorder::Entries(const BodyFunc& body) const {
  base::ListValue entries;
  for (const std::string& id : order_) {
    auto found = entries_.find(id);
    if (found == entries_.end() || !found->second.finished)
      continue;
    const Entry& entry = found->second;
    const std::string* url = entry.request.FindString("url");
    const std::string* method = entry.request.FindString("method");
    base::DictValue request;
    request.Set("method", method ? *method : "GET");
    request.Set("url", url ? *url : "");
    request.Set("httpVersion", "HTTP/1.1");
    request.Set("cookies", base::ListValue());
    request.Set("headers", Headers(entry.request));
    request.Set("queryString", base::ListValue());
    request.Set("headersSize", -1);
    request.Set("bodySize", -1);
    base::DictValue response;
    response.Set("status", entry.response.FindInt("status").value_or(0));
    const std::string* status_text = entry.response.FindString("statusText");
    response.Set("statusText", status_text ? *status_text : "");
    response.Set("httpVersion", "HTTP/1.1");
    response.Set("cookies", base::ListValue());
    response.Set("headers", Headers(entry.response));
    base::DictValue content;
    const std::string* mime = entry.response.FindString("mimeType");
    content.Set("mimeType", mime ? *mime : "x-unknown");
    std::optional<std::string> bytes = body ? body.Run(id) : std::nullopt;
    content.Set("size", bytes ? static_cast<int>(bytes->size()) : -1);
    if (bytes && embed_) {
      content.Set("text", base::Base64Encode(*bytes));
      content.Set("encoding", "base64");
    }
    response.Set("content", std::move(content));
    response.Set("redirectURL", "");
    response.Set("headersSize", -1);
    response.Set("bodySize", bytes ? static_cast<int>(bytes->size()) : -1);
    if (!entry.failure.empty())
      response.Set("_failureText", entry.failure);
    const double wait = entry.responded.is_null()
                            ? -1
                            : (entry.responded - entry.start).InMillisecondsF();
    const double receive =
        entry.responded.is_null() ? -1
                                  : (entry.ended - entry.responded).InMillisecondsF();
    base::DictValue timings;
    timings.Set("send", 0);
    timings.Set("wait", wait);
    timings.Set("receive", receive);
    base::DictValue har;
    har.Set("startedDateTime", Iso8601(entry.started));
    har.Set("time", (entry.ended - entry.start).InMillisecondsF());
    har.Set("request", std::move(request));
    har.Set("response", std::move(response));
    har.Set("cache", base::DictValue());
    har.Set("timings", std::move(timings));
    if (const std::string* address = entry.response.FindString("serverAddr"))
      har.Set("serverIPAddress", *address);
    if (const std::string* frame = entry.request.FindString("frame"))
      har.Set("_frameref", *frame);
    entries.Append(std::move(har));
  }
  return entries;
}

void HarRecorder::Export(base::FilePath path,
                         const BodyFunc& body,
                         const std::string& browser_version,
                         WrittenCallback done) const {
  base::DictValue creator;
  creator.Set("name", "zebkit");
  creator.Set("version", zebkit_version());
  base::DictValue browser;
  browser.Set("name", "chromium");
  browser.Set("version", browser_version);
  base::DictValue log;
  log.Set("version", "1.2");
  log.Set("creator", std::move(creator));
  log.Set("browser", std::move(browser));
  log.Set("pages", base::ListValue());
  log.Set("entries", Entries(body));
  base::DictValue har;
  har.Set("log", std::move(log));
  path = OutputPath(path, ".har");
  std::string text = Json(base::Value(std::move(har)));
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock()},
      base::BindOnce(
          [](base::FilePath path, std::string text) {
            return base::WriteFile(path, text);
          },
          path, std::move(text)),
      base::BindOnce(&Reply, std::move(done), path));
}

// ---- Traces -------------------------------------------------------------

TraceRecorder::TraceRecorder(std::string page_id, const base::DictValue& options)
    : page_id_(std::move(page_id)),
      screenshots_(options.FindBool("screenshots").value_or(false)),
      origin_(base::TimeTicks::Now()),
      network_(base::DictValue()) {
  if (const std::string* title = options.FindString("title"))
    title_ = *title;
  StartChunk(title_);
}

TraceRecorder::~TraceRecorder() = default;

double TraceRecorder::Now() const {
  return (base::TimeTicks::Now() - origin_).InMillisecondsF();
}

void TraceRecorder::Line(base::DictValue event) {
  lines_.push_back(Json(base::Value(std::move(event))));
}

void TraceRecorder::StartChunk(const std::string& title) {
  lines_.clear();
  resources_.clear();
  network_ = HarRecorder(base::DictValue());
  base::DictValue event;
  event.Set("version", 10);
  event.Set("type", "context-options");
  event.Set("origin", "library");
  event.Set("browserName", "chromium");
  event.Set("platform",
#if BUILDFLAG(IS_MAC)
            "darwin"
#elif BUILDFLAG(IS_WIN)
            "win32"
#else
            "linux"
#endif
  );
  event.Set("wallTime", base::Time::Now().InMillisecondsFSinceUnixEpoch());
  event.Set("monotonicTime", Now());
  if (!title.empty())
    event.Set("title", title);
  event.Set("options", base::DictValue());
  event.Set("sdkLanguage", "javascript");
  Line(std::move(event));
}

std::string TraceRecorder::Before(const std::string& method,
                                  const base::DictValue& params) {
  const std::string call_id = "call@" + base::NumberToString(++next_call_);
  const size_t dot = method.find('.');
  base::DictValue event;
  event.Set("type", "before");
  event.Set("callId", call_id);
  event.Set("startTime", Now());
  event.Set("class", method.substr(0, dot));
  event.Set("method", dot == std::string::npos ? method : method.substr(dot + 1));
  event.Set("params", params.Clone());
  event.Set("pageId", page_id_);
  Line(std::move(event));
  return call_id;
}

void TraceRecorder::After(const std::string& call_id, const std::string& error) {
  base::DictValue event;
  event.Set("type", "after");
  event.Set("callId", call_id);
  event.Set("endTime", Now());
  if (!error.empty()) {
    base::DictValue failure;
    failure.Set("message", error);
    failure.Set("name", error.rfind("Timeout ", 0) == 0 ? "TimeoutError" : "Error");
    event.Set("error", std::move(failure));
  }
  Line(std::move(event));
}

void TraceRecorder::Log(const std::string& call_id, const std::string& message) {
  base::DictValue event;
  event.Set("type", "log");
  event.Set("callId", call_id);
  event.Set("time", Now());
  event.Set("message", message);
  Line(std::move(event));
}

void TraceRecorder::Screencast(std::vector<uint8_t> jpeg, int width, int height) {
  const std::string name =
      page_id_ + "-" + base::NumberToString(base::TimeTicks::Now().since_origin().InMicroseconds()) +
      ".jpeg";
  base::DictValue event;
  event.Set("type", "screencast-frame");
  event.Set("pageId", page_id_);
  event.Set("file", name);
  event.Set("width", width);
  event.Set("height", height);
  event.Set("timestamp", Now());
  resources_["resources/" + name] = std::move(jpeg);
  Line(std::move(event));
}

void TraceRecorder::Event(const std::string& event, const base::DictValue& params) {
  network_.Record(event, params);
  if (event != "BrowserContext.console")
    return;
  base::DictValue console;
  console.Set("type", "console");
  console.Set("time", Now());
  console.Set("pageId", page_id_);
  const std::string* type = params.FindString("type");
  console.Set("messageType", type ? *type : "log");
  const std::string* text = params.FindString("text");
  console.Set("text", text ? *text : "");
  base::DictValue location;
  if (const base::DictValue* given = params.FindDict("location"))
    location = given->Clone();
  location.Set("columnNumber", 0);
  console.Set("location", std::move(location));
  Line(std::move(console));
}

void TraceRecorder::Export(base::FilePath path,
                           const HarRecorder::BodyFunc& body,
                           WrittenCallback done) {
  std::string trace;
  for (const std::string& line : lines_)
    trace += line + "\n";
  std::string network;
  base::ListValue entries = network_.Entries(body);
  for (base::Value& entry : entries) {
    base::DictValue event;
    event.Set("type", "resource-snapshot");
    event.Set("snapshot", std::move(entry));
    network += Json(base::Value(std::move(event))) + "\n";
  }
  path = OutputPath(path, ".zip");
  std::map<std::string, std::vector<uint8_t>> files = std::move(resources_);
  files["trace.trace"] = std::vector<uint8_t>(trace.begin(), trace.end());
  files["trace.network"] = std::vector<uint8_t>(network.begin(), network.end());
  StartChunk(title_);
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock()},
      base::BindOnce(
          [](base::FilePath path,
             std::map<std::string, std::vector<uint8_t>> files) {
            base::ScopedTempDir directory;
            if (!directory.CreateUniqueTempDir())
              return false;
            for (const auto& [name, bytes] : files) {
              const base::FilePath file = directory.GetPath().AppendASCII(name);
              if (!base::CreateDirectory(file.DirName()) ||
                  !base::WriteFile(file, bytes)) {
                return false;
              }
            }
            return zip::Zip(directory.GetPath(), path, /*include_hidden_files=*/true);
          },
          path, std::move(files)),
      base::BindOnce(&Reply, std::move(done), path));
}

// ---- Video ----------------------------------------------------------------

// The encoder and the file, on a sequence of the thread pool's.
struct VideoRecorder::Encoder {
  Encoder(base::FilePath path, int width, int height)
      : path(std::move(path)), width(width & ~1), height(height & ~1) {}

  ~Encoder() {
    if (ready)
      vpx_codec_destroy(&codec);
    if (image)
      vpx_img_free(image);
    if (format) {
      if (format->pb)
        avio_closep(&format->pb);
      avformat_free_context(format);
    }
  }

  bool Open() {
    vpx_codec_enc_cfg_t config;
    if (vpx_codec_enc_config_default(vpx_codec_vp8_cx(), &config, 0) != VPX_CODEC_OK)
      return false;
    config.g_w = width;
    config.g_h = height;
    config.g_timebase.num = 1;
    config.g_timebase.den = 1000;
    config.rc_target_bitrate = 1000;
    config.g_lag_in_frames = 0;
    config.g_threads = 2;
    config.kf_max_dist = 120;
    if (vpx_codec_enc_init(&codec, vpx_codec_vp8_cx(), &config, 0) != VPX_CODEC_OK)
      return false;
    ready = true;
    image = vpx_img_alloc(nullptr, VPX_IMG_FMT_I420, width, height, 1);
    if (!image)
      return false;
    if (avformat_alloc_output_context2(&format, nullptr, "webm",
                                       path.AsUTF8Unsafe().c_str()) < 0 ||
        !format) {
      return false;
    }
    stream = avformat_new_stream(format, nullptr);
    if (!stream)
      return false;
    stream->time_base = AVRational{1, 1000};
    stream->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    stream->codecpar->codec_id = AV_CODEC_ID_VP8;
    stream->codecpar->width = width;
    stream->codecpar->height = height;
    if (avio_open(&format->pb, path.AsUTF8Unsafe().c_str(), AVIO_FLAG_WRITE) < 0)
      return false;
    return avformat_write_header(format, nullptr) >= 0;
  }

  // A frame at @ms, the view's pixels (N32, which is BGRA here: libyuv's
  // ARGB).
  void Frame(int64_t ms, SkBitmap bitmap) {
    if (!ready || !image || bitmap.drawsNothing())
      return;
    std::vector<uint8_t> scaled;
    const uint8_t* pixels = static_cast<const uint8_t*>(bitmap.getPixels());
    int stride = static_cast<int>(bitmap.rowBytes());
    if (bitmap.width() != width || bitmap.height() != height) {
      scaled.resize(static_cast<size_t>(width) * height * 4);
      libyuv::ARGBScale(pixels, stride, bitmap.width(), bitmap.height(),
                        scaled.data(), width * 4, width, height,
                        libyuv::kFilterBilinear);
      pixels = scaled.data();
      stride = width * 4;
    }
    libyuv::ARGBToI420(pixels, stride, image->planes[0], image->stride[0],
                       image->planes[1], image->stride[1], image->planes[2],
                       image->stride[2], width, height);
    if (ms <= last_ms)
      ms = last_ms + 1;
    const int64_t duration = last_ms < 0 ? 40 : ms - last_ms;
    last_ms = ms;
    if (vpx_codec_encode(&codec, image, ms, duration, 0, VPX_DL_REALTIME) != VPX_CODEC_OK)
      return;
    Drain();
  }

  void Drain() {
    vpx_codec_iter_t iterator = nullptr;
    while (const vpx_codec_cx_pkt_t* packet = vpx_codec_get_cx_data(&codec, &iterator)) {
      if (packet->kind != VPX_CODEC_CX_FRAME_PKT)
        continue;
      AVPacket* av = av_packet_alloc();
      if (av_new_packet(av, static_cast<int>(packet->data.frame.sz)) < 0) {
        av_packet_free(&av);
        continue;
      }
      memcpy(av->data, packet->data.frame.buf, packet->data.frame.sz);
      av->stream_index = stream->index;
      av->pts = av->dts = av_rescale_q(packet->data.frame.pts, AVRational{1, 1000},
                                       stream->time_base);
      if (packet->data.frame.flags & VPX_FRAME_IS_KEY)
        av->flags |= AV_PKT_FLAG_KEY;
      av_interleaved_write_frame(format, av);
      av_packet_free(&av);
    }
  }

  bool Finish() {
    if (!ready || !format)
      return false;
    vpx_codec_encode(&codec, nullptr, last_ms + 1, 1, 0, VPX_DL_REALTIME);
    Drain();
    const bool written = av_write_trailer(format) >= 0;
    avio_closep(&format->pb);
    return written;
  }

  base::FilePath path;
  int width;
  int height;
  vpx_codec_ctx_t codec;
  bool ready = false;
  vpx_image_t* image = nullptr;
  AVFormatContext* format = nullptr;
  AVStream* stream = nullptr;
  int64_t last_ms = -1;
};

namespace {

scoped_refptr<base::SequencedTaskRunner> VideoSequence() {
  static base::NoDestructor<scoped_refptr<base::SequencedTaskRunner>> runner(
      base::ThreadPool::CreateSequencedTaskRunner(
          {base::MayBlock(), base::TaskPriority::USER_VISIBLE}));
  return *runner;
}

}  // namespace

VideoRecorder::VideoRecorder(content::WebContents* contents,
                             base::FilePath path,
                             int width,
                             int height)
    : contents_(contents->GetWeakPtr()),
      path_(OutputPath(path, ".webm")),
      start_(base::TimeTicks::Now()) {
  auto encoder = std::make_unique<Encoder>(path_, std::max(width, 2), std::max(height, 2));
  // Opened on its sequence before any frame.
  Encoder* raw = encoder.get();
  VideoSequence()->PostTask(FROM_HERE, base::BindOnce(
                                           [](Encoder* encoder) {
                                             if (!encoder->Open())
                                               encoder->ready = false;
                                           },
                                           raw));
  encoder_ = std::move(encoder);
  timer_.Start(FROM_HERE, base::Milliseconds(40),
               base::BindRepeating(&VideoRecorder::Capture, base::Unretained(this)));
}

VideoRecorder::~VideoRecorder() {
  timer_.Stop();
  if (encoder_)
    VideoSequence()->DeleteSoon(FROM_HERE, std::move(encoder_));
}

void VideoRecorder::Capture() {
  if (capturing_ || !contents_)
    return;
  content::RenderWidgetHostView* view = contents_->GetRenderWidgetHostView();
  if (!view)
    return;
  capturing_ = true;
  const base::TimeTicks when = base::TimeTicks::Now();
  view->CopyFromSurface(
      gfx::Rect(), gfx::Size(), base::Seconds(1),
      base::BindOnce(
          [](base::WeakPtr<VideoRecorder> self, base::TimeTicks when,
             const content::CopyFromSurfaceResult& result) {
            if (!self)
              return;
            self->capturing_ = false;
            if (result.has_value())
              self->Captured(when, result->bitmap);
          },
          weak_factory_.GetWeakPtr(), when));
}

void VideoRecorder::Captured(base::TimeTicks when, const SkBitmap& bitmap) {
  if (!encoder_)
    return;
  VideoSequence()->PostTask(
      FROM_HERE,
      base::BindOnce(&Encoder::Frame, base::Unretained(encoder_.get()),
                     (when - start_).InMilliseconds(), bitmap));
}

void VideoRecorder::Stop(WrittenCallback done) {
  timer_.Stop();
  if (!encoder_) {
    std::move(done).Run(path_, "the video could not be made");
    return;
  }
  std::unique_ptr<Encoder> encoder = std::move(encoder_);
  Encoder* raw = encoder.get();
  VideoSequence()->PostTaskAndReplyWithResult(
      FROM_HERE,
      base::BindOnce(
          [](std::unique_ptr<Encoder> encoder) { return encoder->Finish(); },
          std::move(encoder)),
      base::BindOnce(
          [](base::FilePath path, WrittenCallback done, bool ok) {
            std::move(done).Run(path, ok ? std::string() : "the video could not be written");
          },
          path_, std::move(done)));
  (void)raw;
}

}  // namespace zebkit
