// Media capture, the renderer's side: a frame's <audio> and <video>
// elements, and a tap on a player's demuxer (media::DemuxerTap,
// patches/1031) that sends what its decoders get to the browser, where
// media_capture_writer.cc makes a file of it. Nothing is decoded: the
// buffers are the stream's own, with the configuration they need.

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "base/command_line.h"
#include "base/functional/bind.h"
#include "base/json/json_writer.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/no_destructor.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/synchronization/lock.h"
#include "base/task/sequenced_task_runner.h"
#include "base/task/thread_pool.h"
#include "base/timer/timer.h"
#include "base/values.h"
#include "content/public/renderer/render_frame.h"
#include "content/public/renderer/render_frame_observer.h"
#include "media/base/audio_decoder_config.h"
#include "media/base/decoder_buffer.h"
#include "media/base/demuxer_tap.h"
#include "media/base/timestamp_constants.h"
#include "media/base/video_decoder_config.h"
#include "mojo/public/cpp/base/big_buffer.h"
#include "mojo/public/cpp/bindings/associated_receiver_set.h"
#include "mojo/public/cpp/bindings/pending_associated_receiver.h"
#include "mojo/public/cpp/bindings/shared_remote.h"
#include "third_party/blink/public/common/associated_interfaces/associated_interface_registry.h"
#include "third_party/blink/public/platform/task_type.h"
#include "third_party/blink/public/platform/web_media_capture.h"
#include "third_party/blink/public/platform/web_media_player.h"
#include "third_party/blink/public/platform/web_string.h"
#include "third_party/blink/public/platform/web_time_range.h"
#include "third_party/blink/public/platform/web_url.h"
#include "third_party/blink/public/web/web_document.h"
#include "third_party/blink/public/web/web_element.h"
#include "third_party/blink/public/web/web_frame_widget.h"
#include "third_party/blink/public/web/web_hit_test_result.h"
#include "third_party/blink/public/web/web_local_frame.h"
#include "third_party/blink/public/web/web_media_element.h"
#include "third_party/blink/public/web/web_node.h"
#include "ui/gfx/geometry/rect_f.h"
#include "url/gurl.h"
#include "zebkit/engine/engine.h"
#include "zebkit/engine/media_capture.h"
#include "zebkit/engine/media_capture.mojom.h"

namespace zebkit {

namespace {

int64_t Micros(base::TimeDelta time) {
  if (time == media::kNoTimestamp || time == media::kInfiniteDuration ||
      time.is_min()) {
    return mojom::kUnknown;
  }
  return time.InMicroseconds();
}

// What the demuxer gives its decoders, to the browser. Called on the media
// thread, or on the main thread for what a page appends; the sink is bound
// on a sequence of its own.
class Tap : public media::DemuxerTap {
 public:
  explicit Tap(mojo::PendingRemote<mojom::MediaCaptureSink> sink)
      : sink_(std::move(sink),
              base::ThreadPool::CreateSequencedTaskRunner({})) {}

  mojo::SharedRemote<mojom::MediaCaptureSink>& sink() { return sink_; }

  void OnAudioConfig(const void* stream,
                     const media::AudioDecoderConfig& config) override {
    sink_->OnAudioConfig(Track(stream), config);
  }

  void OnVideoConfig(const void* stream,
                     const media::VideoDecoderConfig& config) override {
    sink_->OnVideoConfig(Track(stream), config);
  }

  void OnBuffer(const void* stream,
                scoped_refptr<media::DecoderBuffer> buffer,
                base::TimeDelta decode_timestamp) override {
    if (!buffer || buffer->end_of_stream() || buffer->empty()) {
      return;
    }
    uint32_t track;
    {
      base::AutoLock lock(lock_);
      auto it = tracks_.find(stream);
      if (it == tracks_.end()) {
        return;
      }
      track = it->second;
    }
    auto packet = mojom::MediaPacket::New();
    packet->track = track;
    packet->data = mojo_base::BigBuffer(base::span<const uint8_t>(*buffer));
    packet->pts_us = Micros(buffer->timestamp());
    packet->dts_us = Micros(decode_timestamp);
    packet->duration_us = Micros(buffer->duration());
    packet->keyframe = buffer->is_key_frame();
    sink_->OnPacket(std::move(packet));
  }

  void OnStopped() override { sink_->OnEnded("the player stopped"); }

 private:
  ~Tap() override = default;

  uint32_t Track(const void* stream) {
    base::AutoLock lock(lock_);
    auto [it, made] = tracks_.emplace(stream, tracks_.size());
    return it->second;
  }

  mojo::SharedRemote<mojom::MediaCaptureSink> sink_;
  base::Lock lock_;
  std::map<const void*, uint32_t> tracks_;
};

// The element as a CSS selector from the document: tag names with their
// place among siblings of the same tag, up to an element with an id.
std::string PathOf(const blink::WebElement& element) {
  std::vector<std::string> parts;
  blink::WebNode node = element;
  while (!node.IsNull() && node.IsElementNode()) {
    blink::WebElement current = node.To<blink::WebElement>();
    std::string tag = current.TagName().Utf8();
    for (char& c : tag) {
      c = base::ToLowerASCII(c);
    }
    const std::string id = current.GetIdAttribute().Utf8();
    if (!id.empty()) {
      parts.push_back(tag + "#" + id);
      break;
    }
    int index = 1;
    for (blink::WebNode sibling = current.PreviousSibling(); !sibling.IsNull();
         sibling = sibling.PreviousSibling()) {
      if (sibling.IsElementNode() &&
          sibling.To<blink::WebElement>().TagName() == current.TagName()) {
        ++index;
      }
    }
    blink::WebNode parent = current.ParentNode();
    if (tag == "html" || parent.IsNull() || !parent.IsElementNode()) {
      parts.push_back(tag);
      break;
    }
    parts.push_back(tag + ":nth-of-type(" + base::NumberToString(index) + ")");
    node = parent;
  }
  std::string path;
  for (auto it = parts.rbegin(); it != parts.rend(); ++it) {
    if (!path.empty()) {
      path += " > ";
    }
    path += *it;
  }
  return path;
}

// The URL the element plays: its src, or its first <source>'s.
std::string SourceOf(const blink::WebElement& element) {
  blink::WebString src = element.GetAttribute("src");
  if (src.IsEmpty()) {
    for (blink::WebNode child = element.FirstChild(); !child.IsNull();
         child = child.NextSibling()) {
      if (child.IsElementNode() &&
          child.To<blink::WebElement>().HasHTMLTagName("source")) {
        src = child.To<blink::WebElement>().GetAttribute("src");
        if (!src.IsEmpty()) {
          break;
        }
      }
    }
  }
  if (src.IsEmpty()) {
    return std::string();
  }
  return GURL(element.GetDocument().CompleteURL(src)).spec();
}

base::Value Seconds(double seconds) {
  if (std::isnan(seconds) || std::isinf(seconds)) {
    return base::Value();
  }
  return base::Value(seconds);
}

class Agent;

// The agents of this process, for the element a hit test finds in another
// frame of it.
std::map<const blink::WebLocalFrame*, Agent*>& Agents() {
  static base::NoDestructor<std::map<const blink::WebLocalFrame*, Agent*>>
      agents;
  return *agents;
}

int NextElementId() {
  static int next = 0;
  return ++next;
}

// One for each frame; deletes itself with it.
class Agent : public content::RenderFrameObserver,
              public mojom::MediaCaptureAgent {
 public:
  explicit Agent(content::RenderFrame* render_frame)
      : content::RenderFrameObserver(render_frame),
        frame_(render_frame->GetWebFrame()) {
    Agents()[frame_] = this;
    render_frame->GetAssociatedInterfaceRegistry()
        ->AddInterface<mojom::MediaCaptureAgent>(base::BindRepeating(
            &Agent::Bind, weak_factory_.GetWeakPtr()));
  }

  ~Agent() override {
    for (auto& [id, capture] : captures_) {
      capture.tap->sink()->OnEnded("the page went away");
    }
  }

  // content::RenderFrameObserver
  void OnDestruct() override {
    Agents().erase(frame_);
    delete this;
  }

  void DidCommitProvisionalLoad(ui::PageTransition transition) override {
    // A new document: the elements of the last one are gone.
    for (auto& [id, capture] : captures_) {
      capture.tap->sink()->OnEnded("the page went away");
    }
    captures_.clear();
    elements_.clear();
  }

  // mojom::MediaCaptureAgent
  void List(ListCallback callback) override {
    // Captures may follow: players keep what one starts from from now on.
    media::DemuxerTap::KeepForTapsFromNow();
    base::ListValue list;
    blink::WebDocument document = render_frame()->GetWebFrame()->GetDocument();
    for (const blink::WebElement& element :
         document.QuerySelectorAll("video, audio")) {
      list.Append(Describe(IdOf(element), element));
    }
    std::move(callback).Run(base::WriteJson(list).value_or("[]"));
  }

  void ElementAt(const gfx::PointF& point,
                 ElementAtCallback callback) override {
    media::DemuxerTap::KeepForTapsFromNow();
    blink::WebLocalFrame* frame = render_frame()->GetWebFrame();
    blink::WebFrameWidget* widget = frame->FrameWidget();
    if (!widget) {
      std::move(callback).Run(std::string(), "null");
      return;
    }
    const gfx::PointF blink_point = widget->DIPsToBlinkSpace(point);
    blink::WebElement found;
    // The element hit, or one it is inside of (a <source>, controls).
    blink::WebNode node = widget->HitTestResultAt(blink_point).GetNode();
    for (; !node.IsNull(); node = node.ParentNode()) {
      if (node.IsElementNode() &&
          blink::IsWebMediaElement(node.To<blink::WebElement>())) {
        found = node.To<blink::WebElement>();
        break;
      }
    }
    // Players draw their own controls over the video: the smallest media
    // element whose box holds the point.
    if (found.IsNull()) {
      int64_t area = 0;
      for (const blink::WebElement& element :
           frame->GetDocument().QuerySelectorAll("video, audio")) {
        const gfx::Rect bounds = element.BoundsInWidget();
        if (!gfx::RectF(bounds).Contains(blink_point)) {
          continue;
        }
        const int64_t size = int64_t{bounds.width()} * bounds.height();
        if (found.IsNull() || size < area) {
          found = element;
          area = size;
        }
      }
    }
    if (found.IsNull()) {
      std::move(callback).Run(std::string(), "null");
      return;
    }
    blink::WebLocalFrame* owner = found.GetDocument().GetFrame();
    auto agent = Agents().find(owner);
    if (!owner || agent == Agents().end()) {
      std::move(callback).Run(std::string(), "null");
      return;
    }
    base::DictValue entry =
        agent->second->Describe(agent->second->IdOf(found), found);
    std::move(callback).Run(owner->GetLocalFrameToken().ToString(),
                            base::WriteJson(entry).value_or("null"));
  }

  void Start(int32_t id,
             bool include_buffered,
             mojo::PendingRemote<mojom::MediaCaptureSink> sink,
             StartCallback callback) override {
    auto element = elements_.find(id);
    if (element == elements_.end()) {
      std::move(callback).Run("no such media element (list the media again)",
                              "null");
      return;
    }
    const std::string json =
        base::WriteJson(Describe(id, element->second)).value_or("null");
    blink::WebMediaPlayer* player = blink::GetWebMediaPlayer(element->second);
    if (!player) {
      std::move(callback).Run("the element has no media loaded", json);
      return;
    }
    blink::WebMediaCapture* capture = player->GetMediaCapture();
    if (!capture) {
      std::move(callback).Run(
          "the element plays a live stream (a camera, WebRTC) that is not "
          "demuxed, and cannot be captured",
          json);
      return;
    }
    if (capture->GetCaptureInfo().encrypted) {
      std::move(callback).Run(
          "the media is encrypted (Encrypted Media Extensions) and cannot be "
          "captured",
          json);
      return;
    }
    auto tap = base::MakeRefCounted<Tap>(std::move(sink));
    tap->sink().set_disconnect_handler(
        base::BindOnce(&Agent::OnSinkClosed, weak_factory_.GetWeakPtr(), id,
                       static_cast<const void*>(tap.get())),
        render_frame()->GetTaskRunner(blink::TaskType::kInternalMedia));
    const base::TimeDelta from =
        include_buffered ? media::kNoTimestamp
                         : base::Seconds(std::max(0.0, player->CurrentTime()));
    // A capture of the element before stops: one tap at a time.
    StopCapture(id);
    if (!capture->SetCaptureTap(tap, from)) {
      std::move(callback).Run("the player is not demuxing anything yet", json);
      return;
    }
    captures_[id] = {tap, player};
    if (!watch_.IsRunning()) {
      watch_.Start(FROM_HERE, base::Seconds(1),
                   base::BindRepeating(&Agent::Watch, base::Unretained(this)));
    }
    std::move(callback).Run(std::string(), json);
  }

 private:
  struct Capture {
    scoped_refptr<Tap> tap;
    // Which player was tapped: an element that loads other media has a
    // new one.
    raw_ptr<blink::WebMediaPlayer, DisableDanglingPtrDetection> player;
  };

  void Bind(mojo::PendingAssociatedReceiver<mojom::MediaCaptureAgent>
                receiver) {
    receivers_.Add(this, std::move(receiver));
  }

  int IdOf(const blink::WebElement& element) {
    for (const auto& [id, known] : elements_) {
      if (known == element) {
        return id;
      }
    }
    Prune();
    const int id = NextElementId();
    elements_[id] = element;
    return id;
  }

  // The elements are held, so that an id stays the element's: those taken
  // out of the document go, unless they are captured (a detached element
  // plays on), so that the page's garbage is not kept.
  void Prune() {
    std::erase_if(elements_, [&](const auto& entry) {
      return !entry.second.IsConnected() && !captures_.contains(entry.first);
    });
  }

  base::DictValue Describe(int id, const blink::WebElement& element) {
    base::DictValue entry;
    entry.Set("element", id);
    std::string tag = element.TagName().Utf8();
    for (char& c : tag) {
      c = base::ToLowerASCII(c);
    }
    entry.Set("tag", tag);
    entry.Set("path", PathOf(element));
    entry.Set("src", SourceOf(element));
    blink::WebMediaPlayer* player = blink::GetWebMediaPlayer(element);
    if (!player) {
      entry.Set("kind", "none");
      entry.Set("capturable", false);
      return entry;
    }
    blink::WebMediaCapture* capture = player->GetMediaCapture();
    blink::WebMediaCapture::Info info;
    if (capture) {
      info = capture->GetCaptureInfo();
    } else {
      info.kind = "stream";
    }
    entry.Set("kind", info.kind);
    const double duration = player->Duration();
    entry.Set("duration", Seconds(duration));
    entry.Set("live", std::isinf(duration));
    entry.Set("currentTime", Seconds(player->CurrentTime()));
    base::ListValue buffered;
    for (const blink::WebTimeRange& range : player->Buffered()) {
      buffered.Append(base::ListValue()
                          .Append(Seconds(range.start))
                          .Append(Seconds(range.end)));
    }
    entry.Set("buffered", std::move(buffered));
    base::DictValue codecs;
    if (!info.audio_codec.empty()) {
      codecs.Set("audio", info.audio_codec);
    }
    if (!info.video_codec.empty()) {
      codecs.Set("video", info.video_codec);
    }
    entry.Set("codecs", std::move(codecs));
    entry.Set("hasAudio", player->HasAudio());
    entry.Set("hasVideo", player->HasVideo());
    const gfx::Size size = player->NaturalSize();
    entry.Set("width", size.width());
    entry.Set("height", size.height());
    entry.Set("playing", !player->Paused());
    entry.Set("encrypted", info.encrypted);
    entry.Set("capturable", capture && !info.encrypted);
    entry.Set("capturing", captures_.contains(id));
    return entry;
  }

  void StopCapture(int id) {
    auto it = captures_.find(id);
    if (it == captures_.end()) {
      return;
    }
    auto element = elements_.find(id);
    if (element != elements_.end() &&
        blink::GetWebMediaPlayer(element->second) == it->second.player) {
      it->second.player->GetMediaCapture()->SetCaptureTap(nullptr,
                                                          media::kNoTimestamp);
    }
    captures_.erase(it);
  }

  // The browser closed the capture.
  void OnSinkClosed(int id, const void* tap) {
    auto it = captures_.find(id);
    if (it != captures_.end() && it->second.tap.get() == tap) {
      StopCapture(id);
    }
  }

  // An element that loads other media (a new src) or is removed from its
  // player has none of the capture's any more.
  void Watch() {
    Prune();
    std::vector<int> ended;
    for (auto& [id, capture] : captures_) {
      auto element = elements_.find(id);
      if (element == elements_.end() ||
          blink::GetWebMediaPlayer(element->second) != capture.player) {
        capture.tap->sink()->OnEnded("the element loaded other media");
        ended.push_back(id);
      }
    }
    for (int id : ended) {
      captures_.erase(id);
    }
    if (captures_.empty()) {
      watch_.Stop();
    }
  }

  const raw_ptr<const blink::WebLocalFrame> frame_;
  std::map<int, blink::WebElement> elements_;
  std::map<int, Capture> captures_;
  base::RepeatingTimer watch_;
  mojo::AssociatedReceiverSet<mojom::MediaCaptureAgent> receivers_;
  base::WeakPtrFactory<Agent> weak_factory_{this};
};

}  // namespace

void InstallMediaCaptureAgent(content::RenderFrame* render_frame) {
  // zebkit_media_keep_history(): from before the process's first player.
  static const bool keep = [] {
    if (base::CommandLine::ForCurrentProcess()->HasSwitch(
            kMediaHistorySwitch)) {
      media::DemuxerTap::KeepForTapsFromNow();
    }
    return true;
  }();
  (void)keep;
  new Agent(render_frame);
}

}  // namespace zebkit
