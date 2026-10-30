#include "zebkit/engine/include/zebkit/media_tap.h"
#include <cstring>
#include <optional>
#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/memory/weak_ptr.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_split.h"
#include "base/task/bind_post_task.h"
#include "base/task/sequenced_task_runner.h"
#include "base/task/thread_pool.h"
#include "base/threading/sequence_bound.h"
#include "base/timer/timer.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/web_contents.h"
#include "mojo/public/cpp/bindings/associated_remote.h"
#include "mojo/public/cpp/bindings/callback_helpers.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "third_party/blink/public/common/associated_interfaces/associated_interface_provider.h"
#include "zebkit/engine/media_capture.mojom.h"
#include "zebkit/engine/media_capture_writer.h"
#include "zebkit/engine/view.h"

struct zebkit_media_tap : zebkit::mojom::MediaTapSink {
  zebkit_media_tap_func callback = nullptr;
  void* data = nullptr;
  bool packets = true, stopping = false, done = false, released = false;
  bool deletion_scheduled = false, started = false;
  size_t in_flight = 0, max_pending = 0;
  std::optional<std::string> early_end;
  std::string reason, failure;
  void DeleteLater() {
    if (deletion_scheduled) return;
    deletion_scheduled = true;
    base::SequencedTaskRunner::GetCurrentDefault()->DeleteSoon(FROM_HERE, this);
  }
  mojo::AssociatedRemote<zebkit::mojom::MediaCaptureAgent> agent;
  mojo::Receiver<zebkit::mojom::MediaTapSink> receiver{this};
  // Keeps the writer's unused direct-input pipe alive; data is forwarded on
  // its sequence so acknowledgments also bound queued disk writes.
  mojo::Remote<zebkit::mojom::MediaCaptureSink> writer_pipe;
  base::SequenceBound<zebkit::CaptureWriter> writer;
  base::OneShotTimer timeout;
  base::WeakPtrFactory<zebkit_media_tap> weak{this};

  void Emit(base::DictValue meta, base::span<const uint8_t> bytes = {}) {
    if (!callback || released) return;
    std::string json = base::WriteJson(meta).value_or("{}");
    callback(this, json.c_str(), bytes.data(), bytes.size(), data);
  }
  void Finish(const std::string& why, const std::string& error = {}) {
    if (stopping) return;
    stopping = true; reason = why; failure = error;
    timeout.Stop(); receiver.reset();
    if (writer) {
      writer.AsyncCall(&zebkit::CaptureWriter::Finish).WithArgs(why, error);
    } else {
      base::SequencedTaskRunner::GetCurrentDefault()->PostTask(FROM_HERE,
          base::BindOnce(&zebkit_media_tap::Complete, weak.GetWeakPtr(), std::string("{}")));
    }
  }
  void Complete(const std::string& json) {
    if (done) return;
    done = true; stopping = true; receiver.reset(); timeout.Stop();
    auto meta = base::JSONReader::ReadDict(json, 0).value_or(base::DictValue());
    meta.Set("type", "done");
    if (!reason.empty()) meta.Set("reason", reason);
    else if (!meta.contains("reason")) meta.Set("reason", "stopped");
    if (!failure.empty()) meta.Set("error", failure);
    Emit(std::move(meta));
    if (released) DeleteLater();
  }
  void Started(const std::string& error) {
    if (started) return;
    started = true;
    timeout.Stop();
    if (!error.empty()) Finish("error", error);
    else if (early_end) OnEnded(*early_end);
  }
  void Report(std::string json, bool end) {
    if (end) { Complete(json); return; }
    auto meta = base::JSONReader::ReadDict(json, 0).value_or(base::DictValue());
    meta.Set("type", "progress"); Emit(std::move(meta));
  }
  void OnAudioConfig(uint32_t track, const media::AudioDecoderConfig& config) override {
    if (stopping) return;
    if (config.is_encrypted()) { Finish("error", "encrypted media cannot be tapped"); return; }
    if (writer) writer.AsyncCall(&zebkit::CaptureWriter::OnAudioConfig).WithArgs(track, config);
    if (!packets) return;
    base::DictValue meta;
    meta.Set("type", "audioConfig"); meta.Set("track", static_cast<int>(track));
    meta.Set("codec", media::GetCodecName(config.codec()));
    meta.Set("sampleRate", config.samples_per_second()); meta.Set("channels", config.channels());
    meta.Set("sampleFormat", static_cast<int>(config.sample_format()));
    Emit(std::move(meta), config.extra_data());
  }
  void OnVideoConfig(uint32_t track, const media::VideoDecoderConfig& config) override {
    if (stopping) return;
    if (config.is_encrypted()) { Finish("error", "encrypted media cannot be tapped"); return; }
    if (writer) writer.AsyncCall(&zebkit::CaptureWriter::OnVideoConfig).WithArgs(track, config);
    if (!packets) return;
    base::DictValue meta;
    meta.Set("type", "videoConfig"); meta.Set("track", static_cast<int>(track));
    meta.Set("codec", media::GetCodecName(config.codec()));
    meta.Set("profile", static_cast<int>(config.profile()));
    meta.Set("width", config.coded_size().width()); meta.Set("height", config.coded_size().height());
    meta.Set("displayWidth", config.natural_size().width()); meta.Set("displayHeight", config.natural_size().height());
    Emit(std::move(meta), config.extra_data());
  }
  void OnTimelineOffset(int64_t origin, int64_t from) override {
    if (stopping) return;
    if (writer) writer.AsyncCall(&zebkit::CaptureWriter::OnTimelineOffset).WithArgs(origin, from);
    base::DictValue meta;
    meta.Set("type", "timeline"); meta.Set("unixZeroUs", static_cast<double>(origin));
    meta.Set("fromMediaUs", static_cast<double>(from)); Emit(std::move(meta));
  }
  static bool ValidSample(const base::DictValue& meta, size_t size) {
    const std::string* type = meta.FindString("type");
    if (!type) return false;
    if (*type == "video") {
      const auto width = meta.FindInt("width"), height = meta.FindInt("height"), stride = meta.FindInt("stride");
      if (!width || !height || !stride || *width <= 0 || *height <= 0 || *width > 4096 || *height > 4096 ||
          *stride != *width * 4) return false;
      return size == static_cast<size_t>(*stride) * static_cast<size_t>(*height);
    }
    if (*type == "audio") {
      const auto channels = meta.FindInt("channels"), frames = meta.FindInt("frames");
      if (!channels || !frames || *channels <= 0 || *channels > 256 || *frames <= 0 || *frames > 1 << 24) return false;
      return size == static_cast<size_t>(*channels) * static_cast<size_t>(*frames) * sizeof(float);
    }
    return false;
  }
  void OnPacket(zebkit::mojom::MediaPacketPtr p, OnPacketCallback ack) override {
    if (stopping) { std::move(ack).Run(); return; }
    // The renderer holds to the limit itself, but the browser's memory cannot
    // depend on that: past twice the limit it is not waiting for its acks.
    const size_t packet_size = p->data.size();
    if (max_pending && in_flight + packet_size > 2 * max_pending) {
      Finish("error", "the page exceeded the media tap's pending bytes");
      std::move(ack).Run();
      return;
    }
    if (packets) {
      base::DictValue meta;
      meta.Set("type", "packet"); meta.Set("track", static_cast<int>(p->track));
      // Decimal strings preserve unknown sentinel and full int64 precision.
      meta.Set("ptsUs", base::NumberToString(p->pts_us));
      meta.Set("dtsUs", base::NumberToString(p->dts_us));
      meta.Set("durationUs", base::NumberToString(p->duration_us));
      meta.Set("keyframe", p->keyframe);
      Emit(std::move(meta), base::span(p->data));
    }
    if (writer && !stopping) {
      in_flight += packet_size;
      writer.AsyncCall(&zebkit::CaptureWriter::OnPacket).WithArgs(std::move(p)).Then(base::BindOnce(
          [](base::WeakPtr<zebkit_media_tap> tap, size_t size, OnPacketCallback ack) {
            if (tap) tap->in_flight -= size;
            std::move(ack).Run();
          }, weak.GetWeakPtr(), packet_size, std::move(ack)));
    }
    else std::move(ack).Run();
  }
  void OnSample(const std::string& json, mojo_base::BigBuffer bytes, OnSampleCallback ack) override {
    if (!stopping) {
      // From a renderer. The host sizes its reads from these numbers, so they
      // are made to agree with the bytes, and only known samples go on.
      auto meta = base::JSONReader::ReadDict(json, 0);
      if (!meta || !ValidSample(*meta, bytes.size())) {
        Finish("error", "the page sent an invalid media sample");
      } else {
        Emit(std::move(*meta), base::span(bytes));
      }
    }
    std::move(ack).Run();
  }
  void OnEnded(const std::string& why) override {
    if (!started) { early_end = why; return; }
    bool normal = why == "stopped" || why == "the page went away" || why == "the player stopped" || why == "the element loaded other media";
    Finish(normal ? why : "error", normal ? "" : why);
  }
};

extern "C" zebkit_media_tap* zebkit_media_tap_start(zebkit_view* view,
    const char* id, const char* options_json, zebkit_media_tap_func func, void* data, char** error) {
  if (error) *error = nullptr;
  auto fail = [error](const std::string& message) -> zebkit_media_tap* {
    if (error) *error = strdup(message.c_str());
    return nullptr;
  };
  if (!view || !view->web_contents() || !id || !func) return fail("view, media id and callback are required");
  auto options = base::JSONReader::ReadDict(options_json ? options_json : "{}", 0);
  if (!options) return fail("media tap options must be a JSON object");
  for (auto [key, value] : *options) {
    if (key == "packets" || key == "video" || key == "audio" || key == "includeBuffered") {
      if (!value.is_bool()) return fail(key + " must be a boolean");
    } else if (key == "maxFps" || key == "maxWidth" || key == "maxHeight" || key == "maxPendingBytes") {
      if (!value.is_int()) return fail(key + " must be an integer");
    } else if (key == "path") {
      if (!value.is_string()) return fail("path must be a string");
    } else if (key == "recordingOptions") {
      if (!value.is_dict()) return fail("recordingOptions must be an object");
    } else return fail("unknown media tap option: " + key);
  }
  bool packets = options->FindBool("packets").value_or(true);
  bool video = options->FindBool("video").value_or(true);
  bool audio = options->FindBool("audio").value_or(true);
  int fps = options->FindInt("maxFps").value_or(10);
  int width = options->FindInt("maxWidth").value_or(1280);
  int height = options->FindInt("maxHeight").value_or(720);
  int pending = options->FindInt("maxPendingBytes").value_or(16777216);
  if (fps < 1 || fps > 60 || width < 1 || width > 4096 || height < 1 || height > 4096 || pending < 65536 || pending > 67108864)
    return fail("media tap limits are out of range");
  auto* path = options->FindString("path");
  if (path && (path->empty() || path->find('\0') != std::string::npos || !base::FilePath::FromUTF8Unsafe(*path).IsAbsolute() || base::FilePath::FromUTF8Unsafe(*path).EndsWithSeparator()))
    return fail("path must be an absolute file path");
  if (!packets && !video && !audio && !path) return fail("at least one output is required");
  if (!path && options->contains("recordingOptions")) return fail("recordingOptions requires path");
  std::vector<std::string> parts = base::SplitString(id, ".", base::KEEP_WHITESPACE, base::SPLIT_WANT_ALL);
  int process, routing, element;
  if (parts.size() != 3 || !base::StringToInt(parts[0], &process) || !base::StringToInt(parts[1], &routing) || !base::StringToInt(parts[2], &element))
    return fail("not a media id of the media list");
  auto* frame = content::RenderFrameHost::FromID(process, routing);
  if (!frame || !frame->IsRenderFrameLive() || content::WebContents::FromRenderFrameHost(frame) != view->web_contents())
    return fail("the media does not belong to this live view");
  zebkit::CaptureOptions recording;
  bool buffered = options->FindBool("includeBuffered").value_or(false);
  if (path) {
    auto* record = options->FindDict("recordingOptions");
    std::string message;
    bool record_buffered = false;
    if (!zebkit::ParseCaptureOptions(record ? base::WriteJson(*record).value_or("{}") : "{}", &recording, &record_buffered, &message))
      return fail(message);
    buffered = buffered || record_buffered;
  }
  auto* tap = new zebkit_media_tap;
  tap->callback = func; tap->data = data; tap->packets = packets;
  if (path) {
    auto receiver = tap->writer_pipe.BindNewPipeAndPassReceiver();
    tap->writer = base::SequenceBound<zebkit::CaptureWriter>(
        base::ThreadPool::CreateSequencedTaskRunner({base::MayBlock(), base::TaskPriority::USER_VISIBLE}),
        base::FilePath::FromUTF8Unsafe(*path), recording, std::move(receiver),
        base::BindPostTaskToCurrentDefault(base::BindRepeating(&zebkit_media_tap::Report, tap->weak.GetWeakPtr())));
  }
  frame->GetRemoteAssociatedInterfaces()->GetInterface(&tap->agent);
  auto remote = tap->receiver.BindNewPipeAndPassRemote();
  tap->receiver.set_disconnect_handler(base::BindOnce(&zebkit_media_tap::OnEnded,
      tap->weak.GetWeakPtr(), "the page went away"));
  tap->max_pending = static_cast<size_t>(pending);
  tap->agent->StartTap(element, packets || path, video, audio, fps, width, height, pending, buffered,
      std::move(remote), mojo::WrapCallbackWithDefaultInvokeIfNotRun(
          base::BindOnce(&zebkit_media_tap::Started, tap->weak.GetWeakPtr()), std::string("the frame went away")));
  tap->timeout.Start(FROM_HERE, base::Seconds(10), base::BindOnce(&zebkit_media_tap::Started,
      tap->weak.GetWeakPtr(), std::string("the page did not answer the tap start")));
  return tap;
}
extern "C" void zebkit_media_tap_stop(zebkit_media_tap* tap) {
  if (tap) tap->Finish("stopped");
}
extern "C" void zebkit_media_tap_free(zebkit_media_tap* tap) {
  if (!tap || tap->released) return;
  tap->released = true; tap->callback = nullptr;
  if (tap->done) tap->DeleteLater();
  else tap->Finish("stopped");
}
