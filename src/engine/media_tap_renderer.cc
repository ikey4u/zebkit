#include "zebkit/engine/media_tap_renderer.h"
#include <algorithm>
#include <atomic>
#include <cstring>
#include <map>
#include <optional>
#include "base/functional/bind.h"
#include "base/json/json_writer.h"
#include "base/synchronization/lock.h"
#include "base/task/sequenced_task_runner.h"
#include "base/timer/timer.h"
#include "cc/paint/paint_flags.h"
#include "cc/paint/skia_paint_canvas.h"
#include "media/base/audio_bus.h"
#include "media/base/audio_parameters.h"
#include "media/base/decoder_buffer.h"
#include "media/base/demuxer_tap.h"
#include "media/base/timestamp_constants.h"
#include "media/base/video_frame.h"
#include "mojo/public/cpp/bindings/shared_remote.h"
#include "third_party/blink/public/platform/web_audio_source_provider_impl.h"
#include "third_party/blink/public/platform/web_media_capture.h"
#include "third_party/blink/public/platform/web_media_player.h"
#include "third_party/blink/public/platform/modules/mediastream/web_media_stream_audio_sink.h"
#include "third_party/blink/public/web/web_media_element.h"
#include "third_party/skia/include/core/SkBitmap.h"

namespace zebkit {
namespace {
int64_t Timestamp(base::TimeDelta t) {
  return t == media::kNoTimestamp || t == media::kInfiniteDuration || t.is_min()
      ? mojom::kUnknown : t.InMicroseconds();
}
// All source threads share this budget. Reservation precedes allocation and
// posting, so a stalled host cannot grow the Mojo queue without bound.
class Sender : public media::DemuxerTap {
 public:
  Sender(mojo::PendingRemote<mojom::MediaTapSink> sink, uint32_t limit)
      : runner_(base::SequencedTaskRunner::GetCurrentDefault()),
        sink_(std::move(sink), runner_), limit_(limit) {}
  mojo::SharedRemote<mojom::MediaTapSink>& sink() { return sink_; }
  bool stopped() const { return stopped_.load(); }
  void End(const std::string& reason) {
    if (!stopped_.exchange(true)) sink_->OnEnded(reason);
  }
  bool Reserve(size_t size, bool encoded) {
    if (stopped()) return false;
    size_t old = pending_.load();
    do {
      if (size > limit_ || old > limit_ - size) {
        if (encoded) End("encoded media exceeded maxPendingBytes");
        else ++dropped_;
        return false;
      }
    } while (!pending_.compare_exchange_weak(old, old + size));
    return true;
  }
  void Sample(base::DictValue metadata, mojo_base::BigBuffer bytes) {
    metadata.Set("droppedSamples", static_cast<double>(dropped_.load()));
    // Audio device threads have no current SequencedTaskRunner. SharedRemote
    // replies require one, so issue the request on our owning sequence. The
    // bytes were reserved before this post and remain inside the same budget.
    runner_->PostTask(FROM_HERE, base::BindOnce(&Sender::SendSample,
        base::WrapRefCounted(this), base::WriteJson(metadata).value_or("{}"), std::move(bytes)));
  }
  void PCM(const media::AudioBus& bus, int rate, uint32_t delay,
           base::TimeTicks when, uint32_t track = 0) {
    const size_t size = size_t(bus.channels()) * bus.frames() * sizeof(float);
    if (!Reserve(size, false)) return;
    mojo_base::BigBuffer bytes(size);
    for (int c = 0; c < bus.channels(); ++c) {
      const auto samples = base::as_byte_span(base::allow_nonunique_obj, bus.channel(c));
      base::span(bytes).subspan(size_t(c) * samples.size(), samples.size()).copy_from(samples);
    }
    base::DictValue meta;
    meta.Set("type", "audio"); meta.Set("format", "f32-planar");
    meta.Set("track", static_cast<int>(track));
    meta.Set("sampleRate", rate); meta.Set("channels", bus.channels());
    meta.Set("frames", bus.frames()); meta.Set("framesDelayed", static_cast<double>(delay));
    meta.Set("captureTimeUs", static_cast<double>(when.since_origin().InMicroseconds()));
    Sample(std::move(meta), std::move(bytes));
  }
  void Audio(std::unique_ptr<media::AudioBus> bus, uint32_t delay, int rate) {
    PCM(*bus, rate, delay, base::TimeTicks::Now());
  }
  void OnTimelineOffset(base::Time origin, base::TimeDelta from) override {
    if (!stopped() && !origin.is_null())
      sink_->OnTimelineOffset((origin - base::Time::UnixEpoch()).InMicroseconds(), Timestamp(from));
  }
  void OnAudioConfig(const void* stream, const media::AudioDecoderConfig& config) override {
    if (stopped()) return;
    if (config.is_encrypted()) { End("encrypted media cannot be tapped"); return; }
    if (config.extra_data().size() > 65536) { End("media configuration exceeds 64 KiB"); return; }
    if (++configs_ > 256) { End("too many media configurations"); return; }
    sink_->OnAudioConfig(Track(stream), config);
  }
  void OnVideoConfig(const void* stream, const media::VideoDecoderConfig& config) override {
    if (stopped()) return;
    if (config.is_encrypted()) { End("encrypted media cannot be tapped"); return; }
    if (config.extra_data().size() > 65536) { End("media configuration exceeds 64 KiB"); return; }
    if (++configs_ > 256) { End("too many media configurations"); return; }
    sink_->OnVideoConfig(Track(stream), config);
  }
  void OnBuffer(const void* stream, scoped_refptr<media::DecoderBuffer> buffer,
                base::TimeDelta dts) override {
    if (!buffer || buffer->end_of_stream() || buffer->empty()) return;
    if (buffer->decrypt_config()) { End("encrypted media cannot be tapped"); return; }
    if (!Reserve(buffer->size(), true)) return;
    auto p = mojom::MediaPacket::New();
    p->track = Track(stream);
    p->data = mojo_base::BigBuffer(base::span<const uint8_t>(*buffer));
    p->pts_us = Timestamp(buffer->timestamp()); p->dts_us = Timestamp(dts);
    p->duration_us = Timestamp(buffer->duration()); p->keyframe = buffer->is_key_frame();
    size_t size = p->data.size();
    sink_->OnPacket(std::move(p), base::BindOnce(&Sender::Ack, base::WrapRefCounted(this), size));
  }
  void OnStopped() override { End("the player stopped"); }
 private:
  ~Sender() override = default;
  void Ack(size_t size) { pending_.fetch_sub(size); }
  void SendSample(std::string metadata, mojo_base::BigBuffer bytes) {
    const size_t size = bytes.size();
    if (stopped()) { Ack(size); return; }
    sink_->OnSample(metadata, std::move(bytes),
        base::BindOnce(&Sender::Ack, base::WrapRefCounted(this), size));
  }
  uint32_t Track(const void* stream) {
    base::AutoLock lock(lock_);
    return tracks_.emplace(stream, tracks_.size()).first->second;
  }
  scoped_refptr<base::SequencedTaskRunner> runner_;
  mojo::SharedRemote<mojom::MediaTapSink> sink_;
  const size_t limit_;
  std::atomic<size_t> pending_{0};
  std::atomic<uint64_t> dropped_{0};
  std::atomic<unsigned> configs_{0};
  std::atomic<bool> stopped_{false};
  base::Lock lock_;
  std::map<const void*, uint32_t> tracks_;
};

class StreamAudio : public blink::WebMediaStreamAudioSink {
 public:
  StreamAudio(blink::WebMediaStreamTrack track, scoped_refptr<Sender> sender, uint32_t id)
      : track_(track), sender_(std::move(sender)), id_(id) {
    AddToAudioTrack(this, track_);
  }
  ~StreamAudio() override { RemoveFromAudioTrack(this, track_); }
  void OnSetFormat(const media::AudioParameters& p) override { rate_ = p.sample_rate(); }
  void OnData(const media::AudioBus& bus, base::TimeTicks when) override {
    sender_->PCM(bus, rate_, 0, when, id_);
  }
 private:
  blink::WebMediaStreamTrack track_;
  scoped_refptr<Sender> sender_;
  uint32_t id_;
  int rate_ = 0;
};

class NativeTap final : public NativeMediaTap {
 public:
  NativeTap(blink::WebElement element, bool encoded, bool video,
            uint32_t width, uint32_t height, scoped_refptr<Sender> sender)
      : element_(element), player_(blink::GetWebMediaPlayer(element)),
        encoded_(encoded), video_(video), width_(width), height_(height), sender_(std::move(sender)) {}
  ~NativeTap() override {
    timer_.Stop();
    sender_->End("stopped");
    if (provider_) {
      provider_->SetEmbedderAudioCallback({});
      if (blink::GetWebMediaPlayer(element_) == player_ && player_->GetMediaCapture())
        player_->GetMediaCapture()->SetNativeAudioCapture(false);
    }
    streams_.clear();
    if (encoded_ && blink::GetWebMediaPlayer(element_) == player_)
      player_->GetMediaCapture()->SetCaptureTap(nullptr, media::kNoTimestamp);
  }
  bool encoded() const override { return encoded_; }
  void Start(bool audio, uint32_t fps) {
    if (audio) {
      provider_ = player_->GetAudioSourceProvider();
      if (provider_) {
        provider_->SetEmbedderAudioCallback(base::BindRepeating(&Sender::Audio, sender_));
        if (player_->GetMediaCapture()) player_->GetMediaCapture()->SetNativeAudioCapture(true);
      } else {
        auto tracks = blink::GetWebMediaAudioTracks(element_);
        for (size_t i = 0; i < tracks.size(); ++i)
          streams_.push_back(std::make_unique<StreamAudio>(tracks[i], sender_, i));
      }
    }
    timer_.Start(FROM_HERE, base::Milliseconds(1000.0 / fps),
                 base::BindRepeating(&NativeTap::Poll, base::Unretained(this)));
  }
 private:
  void Poll() {
    if (sender_->stopped()) { timer_.Stop(); return; }
    if (blink::GetWebMediaPlayer(element_) != player_) {
      sender_->End("the element loaded other media"); timer_.Stop(); return;
    }
    auto* capture = player_->GetMediaCapture();
    if (capture && capture->GetCaptureInfo().encrypted) {
      sender_->End("encrypted media cannot be tapped"); timer_.Stop(); return;
    }
    if (!video_ || !player_->HasVideo() || !player_->HasAvailableVideoFrame()) return;
    if (!player_->HasReadableVideoFrame()) {
      sender_->End("the video frame is protected or unreadable"); return;
    }
    auto frame = player_->GetCurrentFrameThenUpdate();
    if (!frame || last_frame_ == frame->unique_id()) return;
    gfx::Size natural = player_->NaturalSize();
    if (natural.IsEmpty()) return;
    double scale = std::min({1.0, double(width_) / natural.width(), double(height_) / natural.height()});
    int w = std::max(1, int(natural.width() * scale));
    int h = std::max(1, int(natural.height() * scale));
    size_t size = size_t(w) * h * 4;
    if (!sender_->Reserve(size, false)) return;
    mojo_base::BigBuffer bytes(size);
    SkBitmap bitmap;
    bitmap.installPixels(SkImageInfo::Make(w, h, kRGBA_8888_SkColorType, kPremul_SkAlphaType), bytes.data(), w * 4);
    bitmap.eraseColor(SK_ColorTRANSPARENT);
    cc::SkiaPaintCanvas canvas(bitmap);
    cc::PaintFlags flags;
    player_->Paint(&canvas, gfx::Rect(w, h), flags, true);
    last_frame_ = frame->unique_id();
    base::DictValue meta;
    meta.Set("type", "video"); meta.Set("format", "rgba8");
    meta.Set("width", w); meta.Set("height", h); meta.Set("stride", w * 4);
    meta.Set("ptsUs", static_cast<double>(Timestamp(frame->timestamp())));
    meta.Set("captureTimeUs", static_cast<double>(base::TimeTicks::Now().since_origin().InMicroseconds()));
    sender_->Sample(std::move(meta), std::move(bytes));
  }
  blink::WebElement element_;
  blink::WebMediaPlayer* player_;
  bool encoded_, video_;
  uint32_t width_, height_;
  scoped_refptr<Sender> sender_;
  scoped_refptr<blink::WebAudioSourceProviderImpl> provider_;
  std::vector<std::unique_ptr<StreamAudio>> streams_;
  std::optional<media::VideoFrame::ID> last_frame_;
  base::RepeatingTimer timer_;
};
}  // namespace

std::unique_ptr<NativeMediaTap> StartNativeMediaTap(
    blink::WebElement element, bool packets, bool video, bool audio,
    uint32_t fps, uint32_t width, uint32_t height, uint32_t pending,
    bool buffered, mojo::PendingRemote<mojom::MediaTapSink> remote, base::OnceClosure disconnected, std::string* error) {
  auto* player = blink::GetWebMediaPlayer(element);
  if (!player) { *error = "the element has no media loaded"; return nullptr; }
  auto* capture = player->GetMediaCapture();
  if (capture && capture->GetCaptureInfo().encrypted) {
    *error = "encrypted media cannot be tapped"; return nullptr;
  }
  if (packets && !capture) {
    *error = "MediaStream has no demuxed packets; select decoded video/audio only"; return nullptr;
  }
  if (audio && player->HasAudio() && !player->GetAudioSourceProvider() &&
      blink::GetWebMediaAudioTracks(element).empty()) {
    *error = "the player does not expose PCM"; return nullptr;
  }
  auto sender = base::MakeRefCounted<Sender>(std::move(remote), pending);
  sender->sink().set_disconnect_handler(std::move(disconnected), base::SequencedTaskRunner::GetCurrentDefault());
  auto tap = std::make_unique<NativeTap>(element, packets, video, width, height, sender);
  if (packets && !capture->SetCaptureTap(sender,
      buffered ? media::kNoTimestamp : base::Seconds(std::max(0.0, player->CurrentTime())))) {
    *error = "the player is not demuxing anything yet"; return nullptr;
  }
  tap->Start(audio, fps);
  return tap;
}
}  // namespace zebkit
