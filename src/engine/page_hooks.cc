// What a page asks of its window (<zebkit/hooks.h>): links to other programs,
// file choosers, camera and microphone, keys before the page, context menus and
// fullscreen, as content's delegates ask of an embedder.

#include "zebkit/engine/page_hooks.h"
#include "zebkit/engine/context_menu.h"
#include "zebkit/engine/share.h"
#include "base/memory/weak_ptr.h"
#include "base/strings/string_number_conversions.h"
#include "content/public/browser/eye_dropper_listener.h"
#include "content/public/browser/eye_dropper.h"

#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "base/files/file_enumerator.h"
#include "base/files/file_path.h"
#include "base/json/json_reader.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/thread_pool.h"
#include "components/input/native_web_keyboard_event.h"
#include "build/build_config.h"
#include "content/public/browser/context_menu_params.h"
#include "content/public/browser/desktop_media_id.h"
#include "content/public/browser/render_process_host.h"
#include "content/public/browser/web_contents_media_capture_id.h"
#include "media/mojo/mojom/display_media_information.mojom.h"
#include "content/public/browser/file_select_listener.h"
#include "content/public/browser/global_routing_id.h"
#include "content/public/browser/media_capture_devices.h"
#include "content/public/browser/media_stream_request.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/weak_document_ptr.h"
#include "zebkit/engine/window.h"
#include "services/network/public/cpp/web_sandbox_flags.h"
#include "services/network/public/mojom/permissions_policy/permissions_policy_feature.mojom.h"
#include "third_party/blink/public/common/input/web_input_event.h"
#include "third_party/blink/public/common/mediastream/media_stream_request.h"
#include "third_party/blink/public/mojom/choosers/file_chooser.mojom.h"
#include "third_party/blink/public/mojom/mediastream/media_stream.mojom.h"
#include "ui/events/keycodes/dom/keycode_converter.h"
#include "url/gurl.h"
#include "zebkit/engine/engine.h"
#include "zebkit/engine/hook_requests.h"
#include "zebkit/engine/network_hooks.h"

namespace zebkit {
namespace {

std::string FrameId(content::RenderFrameHost* frame) {
  return "f" + base::NumberToString(frame->GetFrameTreeNodeId().value());
}

// A report after the fact, which can close the view. The function is that of
// the view that decides (a popup's opener); the view is the one concerned.
template <auto kFunction, auto kData, auto kGeneration>
void Tell(zebkit_view* view, base::DictValue event) {
  zebkit_view* decider = view->Decider(kFunction);
  if (!decider || !(decider->*kFunction) || view->told_pending_ >= 256)
    return;
  ++view->told_pending_;
  base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE,
      base::BindOnce(
          [](base::WeakPtr<zebkit_view> view, base::WeakPtr<zebkit_view> decider,
             uint64_t generation, std::string json) {
            if (!view)
              return;
            --view->told_pending_;
            if (!decider || decider.get()->*kGeneration != generation || !(decider.get()->*kFunction))
              return;
            auto function = decider.get()->*kFunction;
            function(view.get(), json.c_str(), decider.get()->*kData);
          },
          view->GetWeakPtr(), decider->GetWeakPtr(), decider->*kGeneration,
          base::WriteJson(event).value_or("{}")));
}

// A request a document made goes with the document.
struct DocumentOf {
  content::WeakDocumentPtr document;
  content::GlobalRenderFrameHostId frame;
};

struct FileTraits {
  struct Decision {
    std::vector<base::FilePath> paths;
  };
  using Request = zebkit_file_chooser_request;
  static constexpr auto kHandler = &zebkit_view::on_file_chooser_;
  static constexpr auto kData = &zebkit_view::on_file_chooser_data_;
  static constexpr auto kResult = &zebkit_view::on_file_chooser_result_;
  static constexpr auto kResultData = &zebkit_view::on_file_chooser_result_data_;
  static constexpr auto kGeneration = &zebkit_view::file_chooser_generation_;
  static constexpr auto kTimeout = &zebkit_view::file_chooser_timeout_ms_;
  static Decision Refusal() { return {}; }
  static Decision Unanswered() { return {}; }
  static const char* State(const Decision& decision) {
    return decision.paths.empty() ? "cancelled" : "selected";
  }
};
using FileRequests = HookRequests<FileTraits>;
std::map<uintptr_t, DocumentOf>& FileDocuments() {
  static base::NoDestructor<std::map<uintptr_t, DocumentOf>> documents;
  return *documents;
}

struct MediaTraits {
  struct Decision {
    bool allow = false;
    std::optional<std::string> audio;
    std::optional<std::string> video;
  };
  using Request = zebkit_media_access_request;
  static constexpr auto kHandler = &zebkit_view::on_media_access_;
  static constexpr auto kData = &zebkit_view::on_media_access_data_;
  static constexpr auto kResult = &zebkit_view::on_media_access_result_;
  static constexpr auto kResultData = &zebkit_view::on_media_access_result_data_;
  static constexpr auto kGeneration = &zebkit_view::media_access_generation_;
  static constexpr auto kTimeout = &zebkit_view::media_access_timeout_ms_;
  static Decision Refusal() { return {}; }
  static Decision Unanswered() { return {}; }
  static const char* State(const Decision& decision) { return decision.allow ? "allowed" : "denied"; }
};
using MediaRequests = HookRequests<MediaTraits>;

struct EyeDropperTraits {
  struct Decision {
    std::optional<SkColor> color;
  };
  using Request = zebkit_eye_dropper_request;
  static constexpr auto kHandler = &zebkit_view::on_eye_dropper_;
  static constexpr auto kData = &zebkit_view::on_eye_dropper_data_;
  static constexpr auto kResult = &zebkit_view::on_eye_dropper_result_;
  static constexpr auto kResultData = &zebkit_view::on_eye_dropper_result_data_;
  static constexpr auto kGeneration = &zebkit_view::eye_dropper_generation_;
  static constexpr auto kTimeout = &zebkit_view::eye_dropper_timeout_ms_;
  static Decision Refusal() { return {}; }
  static Decision Unanswered() { return {}; }
  static const char* State(const Decision& decision) { return decision.color ? "selected" : "cancelled"; }
};
using EyeDropperRequests = HookRequests<EyeDropperTraits>;

struct DeviceTraits {
  struct Decision {
    std::optional<std::string> id;
  };
  using Request = zebkit_device_chooser_request;
  static constexpr auto kHandler = &zebkit_view::on_device_chooser_;
  static constexpr auto kData = &zebkit_view::on_device_chooser_data_;
  static constexpr auto kResult = &zebkit_view::on_device_chooser_result_;
  static constexpr auto kResultData = &zebkit_view::on_device_chooser_result_data_;
  static constexpr auto kGeneration = &zebkit_view::device_chooser_generation_;
  static constexpr auto kTimeout = &zebkit_view::device_chooser_timeout_ms_;
  static Decision Refusal() { return {}; }
  static Decision Unanswered() { return {}; }
  static const char* State(const Decision& decision) { return decision.id ? "selected" : "cancelled"; }
};
using DeviceRequests = HookRequests<DeviceTraits>;
std::map<uintptr_t, DocumentOf>& DeviceDocuments() {
  static base::NoDestructor<std::map<uintptr_t, DocumentOf>> documents;
  return *documents;
}
std::map<uintptr_t, DocumentOf>& EyeDropperDocuments() {
  static base::NoDestructor<std::map<uintptr_t, DocumentOf>> documents;
  return *documents;
}

// What content holds while a page's eye dropper is open; its listener is
// good as long as this is.
class HookEyeDropper : public content::EyeDropper {
 public:
  explicit HookEyeDropper(content::EyeDropperListener* listener) : listener_(listener) {}
  // Content is destroying this: the request ends without its reply, which
  // would call the listener from here.
  ~HookEyeDropper() override {
    if (!id_)
      return;
    EyeDropperDocuments().erase(id_);
    EyeDropperRequests::Get().Abandon(id_);
  }
  void set_id(uintptr_t id) { id_ = id; }
  void Answer(EyeDropperTraits::Decision decision) {
    id_ = 0;
    if (decision.color)
      listener_->ColorSelected(*decision.color);
    else
      listener_->ColorSelectionCanceled();
  }
  base::WeakPtr<HookEyeDropper> GetWeakPtr() { return weak_factory_.GetWeakPtr(); }

 private:
  raw_ptr<content::EyeDropperListener> listener_;
  uintptr_t id_ = 0;
  base::WeakPtrFactory<HookEyeDropper> weak_factory_{this};
};
std::map<uintptr_t, DocumentOf>& MediaDocuments() {
  static base::NoDestructor<std::map<uintptr_t, DocumentOf>> documents;
  return *documents;
}

// A request's document, kept for what is pending: however it ended, its
// entry does not stay.
template <typename Requests>
void RememberDocument(Requests& requests, std::map<uintptr_t, DocumentOf>& documents, DocumentOf document) {
  std::erase_if(documents, [&](const auto& entry) { return !requests.Pending(entry.first); });
  documents[requests.last_id()] = std::move(document);
}

template <typename Requests>
void SweepDocuments(Requests& requests, std::map<uintptr_t, DocumentOf>& documents,
                    zebkit_view* view, content::RenderFrameHost* frame) {
  auto weak = view->GetWeakPtr();
  for (uintptr_t id : requests.IdsOf(view)) {
    auto found = documents.find(id);
    if (found == documents.end())
      continue;
    const bool gone = frame ? found->second.frame == frame->GetGlobalId()
                            : !found->second.document.AsRenderFrameHostIfValid();
    if (gone) {
      documents.erase(found);
      requests.Settle(id, typename Requests::Decision(), "cancelled");
      if (!weak)
        return;
    }
  }
}

void SelectFiles(scoped_refptr<content::FileSelectListener> listener,
                 blink::mojom::FileChooserParams::Mode mode,
                 FileTraits::Decision decision) {
  if (decision.paths.empty()) {
    listener->FileSelectionCanceled();
    return;
  }
  auto give = [](scoped_refptr<content::FileSelectListener> listener,
                 blink::mojom::FileChooserParams::Mode mode, base::FilePath base_dir,
                 std::vector<base::FilePath> paths) {
    std::vector<blink::mojom::FileChooserFileInfoPtr> files;
    for (const base::FilePath& path : paths) {
      files.push_back(blink::mojom::FileChooserFileInfo::NewNativeFile(
          blink::mojom::NativeFileInfo::New(path, std::u16string(), std::vector<std::u16string>())));
    }
    listener->FileSelected(std::move(files), base_dir, mode);
  };
  if (mode != blink::mojom::FileChooserParams::Mode::kUploadFolder) {
    give(std::move(listener), mode, base::FilePath(), std::move(decision.paths));
    return;
  }
  // The files of the directory, as Chrome's FileSelectHelper lists them.
  base::FilePath directory = decision.paths.front();
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
      base::BindOnce(
          [](base::FilePath directory) {
            std::vector<base::FilePath> found;
            base::FileEnumerator files(directory, true, base::FileEnumerator::FILES);
            for (base::FilePath path = files.Next(); !path.empty(); path = files.Next())
              found.push_back(path);
            return found;
          },
          directory),
      base::BindOnce(give, std::move(listener), mode, directory));
}

// Sources getDisplayMedia() may get (docs/SYSTEM_INTEGRATION.md, 10): the view
// itself, whose WebContents content captures without the system; and the whole
// screen where WebRTC's capturer reaches one (X11, macOS, Windows; not Wayland,
// whose capture is PipeWire's, off in this build).
bool ScreenCaptureAvailable() {
#if BUILDFLAG(IS_MAC) || BUILDFLAG(IS_WIN)
  return true;
#elif BUILDFLAG(IS_LINUX)
  return getenv("DISPLAY") && !getenv("WAYLAND_DISPLAY");
#else
  return false;
#endif
}

base::ListValue DisplaySources(content::WebContents* contents) {
  base::ListValue list;
  base::DictValue view;
  view.Set("id", "view");
  view.Set("kind", "view");
  view.Set("name", base::UTF16ToUTF8(contents->GetTitle()));
  list.Append(std::move(view));
  if (ScreenCaptureAvailable()) {
    base::DictValue screen;
    screen.Set("id", "screen");
    screen.Set("kind", "screen");
    screen.Set("name", "Entire screen");
    list.Append(std::move(screen));
  }
  return list;
}

void GrantDisplay(content::WeakDocumentPtr document, content::MediaStreamRequest request,
                  content::MediaResponseCallback callback, MediaTraits::Decision decision) {
  using Result = blink::mojom::MediaStreamRequestResult;
  using Stream = blink::mojom::MediaStreamType;
  auto* frame = document.AsRenderFrameHostIfValid();
  if (!decision.allow || !frame || !frame->IsActive()) {
    std::move(callback).Run(blink::mojom::StreamDevicesSet(), Result::PERMISSION_DENIED, nullptr);
    return;
  }
  content::WebContents* contents = content::WebContents::FromRenderFrameHost(frame);
  const std::string source = decision.video.value_or("");
  content::DesktopMediaID chosen;
  media::mojom::DisplayCaptureSurfaceType surface = media::mojom::DisplayCaptureSurfaceType::MONITOR;
  std::string name;
  if (source == "view" && contents) {
    content::RenderFrameHost* main = contents->GetPrimaryMainFrame();
    chosen = content::DesktopMediaID(
        content::DesktopMediaID::TYPE_WEB_CONTENTS, content::DesktopMediaID::kNullId,
        content::WebContentsMediaCaptureId(main->GetProcess()->GetDeprecatedID(), main->GetRoutingID()));
    surface = media::mojom::DisplayCaptureSurfaceType::BROWSER;
    name = base::UTF16ToUTF8(contents->GetTitle());
  } else if (source == "screen" && ScreenCaptureAvailable()) {
    // webrtc::kFullDesktopScreenId: every screen of the desktop.
    chosen = content::DesktopMediaID(content::DesktopMediaID::TYPE_SCREEN, -1);
    name = "Entire screen";
  } else {
    std::move(callback).Run(blink::mojom::StreamDevicesSet(), Result::INVALID_STATE, nullptr);
    return;
  }
  // The view's sound with its picture, when the page asked and the application
  // said so (an audio answer); the system's sound is not offered.
  const bool audio = request.audio_type == Stream::DISPLAY_AUDIO_CAPTURE && decision.audio &&
                     chosen.type == content::DesktopMediaID::TYPE_WEB_CONTENTS;
  chosen.audio_share = audio;
  auto information = [&] {
    return media::mojom::DisplayMediaInformation::New(
        surface, /*logical_surface=*/true,
        surface == media::mojom::DisplayCaptureSurfaceType::BROWSER ? media::mojom::CursorCaptureType::MOTION
                                                                     : media::mojom::CursorCaptureType::ALWAYS,
        /*capture_handle=*/nullptr, /*initial_zoom_level=*/100);
  };
  auto stream = blink::mojom::StreamDevices::New();
  blink::MediaStreamDevice video(Stream::DISPLAY_VIDEO_CAPTURE, chosen.ToString(), name);
  video.display_media_info = information();
  stream->video_device = std::move(video);
  if (audio) {
    blink::MediaStreamDevice sound(Stream::DISPLAY_AUDIO_CAPTURE, chosen.ToString(), name);
    sound.display_media_info = information();
    stream->audio_device = std::move(sound);
  }
  blink::mojom::StreamDevicesSet selected;
  selected.stream_devices.push_back(std::move(stream));
  std::move(callback).Run(selected, Result::OK, nullptr);
}

void GrantMedia(content::WeakDocumentPtr document, content::MediaStreamRequest request,
                content::MediaResponseCallback callback, MediaTraits::Decision decision) {
  using Result = blink::mojom::MediaStreamRequestResult;
  using Stream = blink::mojom::MediaStreamType;
  auto* frame = document.AsRenderFrameHostIfValid();
  if (!decision.allow || !frame || !frame->IsActive()) {
    std::move(callback).Run(blink::mojom::StreamDevicesSet(), Result::PERMISSION_DENIED, nullptr);
    return;
  }
  // The application's device, else what the page asked for, else the first.
  // Never another than was asked for, as the window's own path (window.cc).
  auto choose = [](const blink::MediaStreamDevices& devices,
                   const std::vector<std::string>& requested,
                   const std::optional<std::string>& chosen) -> std::optional<blink::MediaStreamDevice> {
    if (chosen) {
      for (const auto& device : devices)
        if (device.id == *chosen) return device;
      return std::nullopt;
    }
    for (const auto& id : requested)
      for (const auto& device : devices)
        if (device.id == id) return device;
    if (requested.empty() && !devices.empty()) return devices.front();
    return std::nullopt;
  };
  auto* devices = content::MediaCaptureDevices::GetInstance();
  auto stream = blink::mojom::StreamDevices::New();
  const bool audio = request.audio_type == Stream::DEVICE_AUDIO_CAPTURE;
  const bool video = request.video_type == Stream::DEVICE_VIDEO_CAPTURE;
  if (audio)
    stream->audio_device = choose(devices->GetAudioCaptureDevices(), request.requested_audio_device_ids, decision.audio);
  if (video)
    stream->video_device = choose(devices->GetVideoCaptureDevices(), request.requested_video_device_ids, decision.video);
  if ((audio && !stream->audio_device) || (video && !stream->video_device)) {
    std::move(callback).Run(blink::mojom::StreamDevicesSet(), Result::NO_HARDWARE, nullptr);
    return;
  }
  blink::mojom::StreamDevicesSet selected;
  selected.stream_devices.push_back(std::move(stream));
  std::move(callback).Run(selected, Result::OK, nullptr);
}

base::ListValue DeviceList(const blink::MediaStreamDevices& devices) {
  base::ListValue list;
  for (const auto& device : devices) {
    base::DictValue entry;
    entry.Set("id", device.id);
    entry.Set("name", device.name);
    list.Append(std::move(entry));
  }
  return list;
}

base::ListValue StringList(const std::vector<std::string>& strings) {
  base::ListValue list;
  for (const std::string& value : strings)
    list.Append(value);
  return list;
}

// "Ctrl+Alt+Shift+Meta+Code", in that order, whichever order it was given in.
std::optional<std::string> CanonicalKey(const std::string& combo) {
  std::vector<std::string> parts =
      base::SplitString(combo, "+", base::TRIM_WHITESPACE, base::SPLIT_WANT_ALL);
  if (parts.empty() || ui::KeycodeConverter::CodeStringToDomCode(parts.back()) == ui::DomCode::NONE)
    return std::nullopt;
  std::string out;
  for (const char* modifier : {"Ctrl", "Alt", "Shift", "Meta"}) {
    if (std::erase(parts, std::string(modifier)))
      out += std::string(modifier) + "+";
  }
  if (parts.size() != 1)
    return std::nullopt;
  return out + parts.back();
}

class Hooks : public Window::EmbedderHooks {
 public:
  bool KeyBeforePage(content::WebContents* contents,
                     const input::NativeWebKeyboardEvent& event) override {
    zebkit_view* view = ViewOf(contents);
    if (!view || event.GetType() != blink::WebInputEvent::Type::kRawKeyDown)
      return false;
    zebkit_view* decider = view->Decider(&zebkit_view::reserved_keys_);
    if (!decider || !decider->reserved_keys_)
      return false;
    const std::string code = ui::KeycodeConverter::DomCodeToCodeString(
        static_cast<ui::DomCode>(event.dom_code));
    const int modifiers = event.GetModifiers();
    std::string combo;
    if (modifiers & blink::WebInputEvent::kControlKey) combo += "Ctrl+";
    if (modifiers & blink::WebInputEvent::kAltKey) combo += "Alt+";
    if (modifiers & blink::WebInputEvent::kShiftKey) combo += "Shift+";
    if (modifiers & blink::WebInputEvent::kMetaKey) combo += "Meta+";
    combo += code;
    if (!decider->reserved_keys_->contains(combo))
      return false;
    base::DictValue told;
    told.Set("combo", combo);
    told.Set("key", ui::KeycodeConverter::DomKeyToKeyString(static_cast<ui::DomKey>(event.dom_key)));
    told.Set("code", code);
    told.Set("autoRepeat", !!(modifiers & blink::WebInputEvent::kIsAutoRepeat));
    Tell<&zebkit_view::on_reserved_key_, &zebkit_view::on_reserved_key_data_,
         &zebkit_view::reserved_key_generation_>(view, std::move(told));
    return true;
  }

  bool ContextMenu(content::RenderFrameHost& frame, const content::ContextMenuParams& params) override {
    zebkit_view* view = ViewOf(content::WebContents::FromRenderFrameHost(&frame));
    if (!view)
      return false;
    zebkit_view* decider = view->Decider(&zebkit_view::on_context_menu_);
    if (!decider)
      return true;  // Its opener has closed: nothing shown, nobody to tell.
    if (!decider->on_context_menu_)
      return false;
    base::DictValue told;
    told.Set("x", params.x);
    told.Set("y", params.y);
    told.Set("linkUrl", params.link_url.spec());
    told.Set("linkText", base::UTF16ToUTF8(params.link_text.substr(0, 16384)));
    told.Set("srcUrl", params.src_url.spec());
    told.Set("mediaType", static_cast<int>(params.media_type));
    told.Set("selectionText", base::UTF16ToUTF8(params.selection_text.substr(0, 65536)));
    told.Set("editable", params.is_editable);
    told.Set("pageUrl", params.page_url.spec());
    told.Set("frameUrl", params.frame_url.spec());
    told.Set("frameId", FrameId(&frame));
    // What a default item would do, kept for zebkit_view_context_menu_run/show.
    told.Set("menuId", static_cast<double>(CaptureContextMenu(view, frame, params)));
    told.Set("items", ContextMenuValue(DefaultContextMenu(view->web_contents(), params)));
    Tell<&zebkit_view::on_context_menu_, &zebkit_view::on_context_menu_data_,
         &zebkit_view::context_menu_generation_>(view, std::move(told));
    return true;
  }

  bool CanEnterFullscreen(content::RenderFrameHost* frame) override {
    zebkit_view* view = frame ? ViewOf(content::WebContents::FromRenderFrameHost(frame)) : nullptr;
    if (!view)
      return true;
    zebkit_view* decider = view->Decider(&zebkit_view::fullscreen_policy_);
    if (!decider)
      return false;
    return decider->fullscreen_policy_.value_or(ZEBKIT_FULLSCREEN_ALLOW) == ZEBKIT_FULLSCREEN_ALLOW;
  }

  void FullscreenChanged(content::WebContents* contents, bool entered) override {
    zebkit_view* view = ViewOf(contents);
    if (!view)
      return;
    base::DictValue told;
    told.Set("entered", entered);
    Tell<&zebkit_view::on_fullscreen_, &zebkit_view::on_fullscreen_data_,
         &zebkit_view::fullscreen_generation_>(view, std::move(told));
  }

  bool MediaAccess(content::WebContents* contents, const content::MediaStreamRequest& request,
                   content::MediaResponseCallback& callback) override {
    using Stream = blink::mojom::MediaStreamType;
    using Result = blink::mojom::MediaStreamRequestResult;
    zebkit_view* view = ViewOf(contents);
    if (!view || (!MediaRequests::HasDecider(view) && !MediaRequests::OrphanedPopup(view)))
      return false;
    auto refuse = [&callback] {
      std::move(callback).Run(blink::mojom::StreamDevicesSet(), Result::PERMISSION_DENIED, nullptr);
      return true;
    };
    auto* frame = content::RenderFrameHost::FromID(request.render_process_id, request.render_frame_id);
    const bool audio = request.audio_type == Stream::DEVICE_AUDIO_CAPTURE;
    const bool video = request.video_type == Stream::DEVICE_VIDEO_CAPTURE;
    // getDisplayMedia(): the application chooses the source.
    if (request.video_type == Stream::DISPLAY_VIDEO_CAPTURE &&
        (request.audio_type == Stream::NO_SERVICE || request.audio_type == Stream::DISPLAY_AUDIO_CAPTURE)) {
      if (MediaRequests::OrphanedPopup(view) || !frame || !frame->IsActive() ||
          content::WebContents::FromRenderFrameHost(frame) != contents ||
          frame->GetLastCommittedOrigin() != request.url_origin ||
          !frame->IsFeatureEnabled(network::mojom::PermissionsPolicyFeature::kDisplayCapture)) {
        return refuse();
      }
      base::DictValue metadata;
      metadata.Set("type", "display");
      metadata.Set("audio", request.audio_type == Stream::DISPLAY_AUDIO_CAPTURE);
      metadata.Set("video", true);
      metadata.Set("origin", request.url_origin.Serialize());
      metadata.Set("frameId", FrameId(frame));
      metadata.Set("mainFrame", frame->IsInPrimaryMainFrame());
      metadata.Set("userGesture", request.user_gesture);
      metadata.Set("sources", DisplaySources(contents));
      auto& requests = MediaRequests::Get();
      const DocumentOf document{frame->GetWeakDocumentPtr(), frame->GetGlobalId()};
      MediaRequests::Reply reply = base::BindOnce(&GrantDisplay, document.document, request, std::move(callback));
      if (!requests.Ask(view, std::move(metadata), reply)) {
        std::move(reply).Run(MediaTraits::Decision());
        return true;
      }
      RememberDocument(requests, MediaDocuments(), document);
      return true;
    }
    // Other screen and tab capture (an extension's) need a chooser: the window's path refuses them.
    if ((!audio && request.audio_type != Stream::NO_SERVICE) ||
        (!video && request.video_type != Stream::NO_SERVICE) || (!audio && !video)) {
      return false;
    }
    if (MediaRequests::OrphanedPopup(view) || !frame || !frame->IsActive() ||
        content::WebContents::FromRenderFrameHost(frame) != contents ||
        frame->GetLastCommittedOrigin() != request.url_origin) {
      return refuse();
    }
    // What the permission controller enforces on the window's path.
    using Feature = network::mojom::PermissionsPolicyFeature;
    if ((audio && !frame->IsFeatureEnabled(Feature::kMicrophone)) ||
        (video && !frame->IsFeatureEnabled(Feature::kCamera))) {
      return refuse();
    }
    auto* devices = content::MediaCaptureDevices::GetInstance();
    base::DictValue metadata;
    metadata.Set("audio", audio);
    metadata.Set("video", video);
    metadata.Set("origin", request.url_origin.Serialize());
    metadata.Set("frameId", FrameId(frame));
    metadata.Set("mainFrame", frame->IsInPrimaryMainFrame());
    metadata.Set("userGesture", request.user_gesture);
    metadata.Set("requestedAudioDevices", StringList(request.requested_audio_device_ids));
    metadata.Set("requestedVideoDevices", StringList(request.requested_video_device_ids));
    metadata.Set("audioDevices", DeviceList(devices->GetAudioCaptureDevices()));
    metadata.Set("videoDevices", DeviceList(devices->GetVideoCaptureDevices()));
    auto& requests = MediaRequests::Get();
    const DocumentOf document{frame->GetWeakDocumentPtr(), frame->GetGlobalId()};
    MediaRequests::Reply reply = base::BindOnce(&GrantMedia, document.document, request, std::move(callback));
    if (!requests.Ask(view, std::move(metadata), reply)) {
      std::move(reply).Run(MediaTraits::Decision());
      return true;
    }
    RememberDocument(requests, MediaDocuments(), document);
    return true;
  }
};

}  // namespace

void InstallPageHooks() {
  static base::NoDestructor<Hooks> hooks;
  Window::SetEmbedderHooks(hooks.get());
  FileRequests::Get().Start();
  MediaRequests::Get().Start();
  EyeDropperRequests::Get().Start();
  DeviceRequests::Get().Start();
}
void StopPageHooks() {
  Window::SetEmbedderHooks(nullptr);
  FileRequests::Get().Stop();
  MediaRequests::Get().Stop();
  EyeDropperRequests::Get().Stop();
  DeviceRequests::Get().Stop();
  DeviceDocuments().clear();
  FileDocuments().clear();
  MediaDocuments().clear();
  EyeDropperDocuments().clear();
}
void CancelPageHookRequests(zebkit_view* view) {
  auto weak = view->GetWeakPtr();
  FileRequests::Get().CancelAll(view, "cancelled");
  if (weak) MediaRequests::Get().CancelAll(view, "cancelled");
  if (weak) EyeDropperRequests::Get().CancelAll(view, "cancelled");
  if (weak) DeviceRequests::Get().CancelAll(view, "cancelled");
  if (weak) CancelShareRequests(view);
  if (weak) CancelDownloadRequests(view);
  if (weak) CancelClientCertificateRequests(view);
  if (weak) CancelNetworkHookRequests(view);
}
void PageHooksFrameGone(zebkit_view* view, content::RenderFrameHost* frame) {
  auto weak = view->GetWeakPtr();
  SweepDocuments(FileRequests::Get(), FileDocuments(), view, frame);
  if (weak)
    SweepDocuments(MediaRequests::Get(), MediaDocuments(), view, frame);
  if (weak)
    SweepDocuments(EyeDropperRequests::Get(), EyeDropperDocuments(), view, frame);
  if (weak)
    SweepDocuments(DeviceRequests::Get(), DeviceDocuments(), view, frame);
}

std::optional<uintptr_t> AskDeviceChooser(content::RenderFrameHost* frame, const char* kind, base::Value filters,
                                          base::ListValue devices, DeviceChoice& choice) {
  zebkit_view* view = ViewOf(content::WebContents::FromRenderFrameHost(frame));
  if (!view || DeviceRequests::OrphanedPopup(view) || !DeviceRequests::HasDecider(view))
    return std::nullopt;
  base::DictValue metadata;
  metadata.Set("kind", kind);
  metadata.Set("frameId", FrameId(frame));
  metadata.Set("mainFrame", frame->IsInPrimaryMainFrame());
  metadata.Set("origin", frame->GetLastCommittedOrigin().Serialize());
  metadata.Set("filters", std::move(filters));
  metadata.Set("devices", std::move(devices));
  auto& requests = DeviceRequests::Get();
  DeviceRequests::Reply reply = base::BindOnce(
      [](DeviceChoice choice, DeviceTraits::Decision decision) { std::move(choice).Run(decision.id); },
      std::move(choice));
  if (!requests.Ask(view, std::move(metadata), reply)) {
    // Too many waiting: none, after the chooser is made.
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE, base::BindOnce(std::move(reply), DeviceTraits::Decision()));
    return 0;
  }
  RememberDocument(requests, DeviceDocuments(), {frame->GetWeakDocumentPtr(), frame->GetGlobalId()});
  return requests.last_id();
}

void UpdateDeviceChooser(uintptr_t id, base::ListValue devices) {
  auto& requests = DeviceRequests::Get();
  if (!requests.Ready())
    return;
  if (auto* entry = requests.Find(id))
    entry->metadata.Set("devices", std::move(devices));
}

void AbandonDeviceChooser(uintptr_t id) {
  auto& requests = DeviceRequests::Get();
  if (requests.Ready())
    requests.Abandon(id);
  DeviceDocuments().erase(id);
}

std::unique_ptr<content::EyeDropper> OpenEyeDropperHook(content::RenderFrameHost* frame,
                                                       content::EyeDropperListener* listener) {
  zebkit_view* view = ViewOf(content::WebContents::FromRenderFrameHost(frame));
  // Without a function, nothing to pick a colour with: cancelled, as before.
  if (!view || EyeDropperRequests::OrphanedPopup(view) || !EyeDropperRequests::HasDecider(view))
    return nullptr;
  auto dropper = std::make_unique<HookEyeDropper>(listener);
  base::DictValue metadata;
  metadata.Set("frameId", FrameId(frame));
  metadata.Set("mainFrame", frame->IsInPrimaryMainFrame());
  metadata.Set("origin", frame->GetLastCommittedOrigin().Serialize());
  auto& requests = EyeDropperRequests::Get();
  EyeDropperRequests::Reply reply = base::BindOnce(
      [](base::WeakPtr<HookEyeDropper> dropper, EyeDropperTraits::Decision decision) {
        if (dropper)
          dropper->Answer(std::move(decision));
      },
      dropper->GetWeakPtr());
  if (!requests.Ask(view, std::move(metadata), reply))
    return nullptr;
  dropper->set_id(requests.last_id());
  RememberDocument(requests, EyeDropperDocuments(), {frame->GetWeakDocumentPtr(), frame->GetGlobalId()});
  return dropper;
}

void TellExternalProtocol(const GURL& url, content::WebContents* contents,
                          content::FrameTreeNodeId frame, bool main_frame, bool fenced,
                          network::mojom::WebSandboxFlags sandbox, bool user_gesture,
                          const std::optional<url::Origin>& initiator,
                          content::RenderFrameHost* initiator_document) {
  zebkit_view* view = contents ? ViewOf(contents) : nullptr;
  using Sandbox = network::mojom::WebSandboxFlags;
  if (!view || fenced || (sandbox & Sandbox::kTopNavigationToCustomProtocols) != Sandbox::kNone ||
      url.spec().size() > 65536) {
    return;
  }
  base::DictValue told;
  told.Set("url", url.spec());
  told.Set("scheme", url.GetScheme());
  told.Set("mainFrame", main_frame);
  told.Set("userGesture", user_gesture);
  // Without a redirect, Chromium's initiator can be the link's own opaque origin.
  if (initiator_document && (!initiator || initiator->opaque()))
    told.Set("initiatorOrigin", initiator_document->GetLastCommittedOrigin().Serialize());
  else if (initiator)
    told.Set("initiatorOrigin", initiator->Serialize());
  else
    told.Set("initiatorOrigin", base::Value());
  told.Set("frameId", "f" + base::NumberToString(frame.value()));
  Tell<&zebkit_view::on_external_protocol_, &zebkit_view::on_external_protocol_data_,
       &zebkit_view::external_protocol_generation_>(view, std::move(told));
}

bool HandleFileChooserHook(content::RenderFrameHost* frame,
                           scoped_refptr<content::FileSelectListener> listener,
                           const blink::mojom::FileChooserParams& params) {
  using Mode = blink::mojom::FileChooserParams::Mode;
  zebkit_view* view = ViewOf(content::WebContents::FromRenderFrameHost(frame));
  if (FileRequests::OrphanedPopup(view)) {
    listener->FileSelectionCanceled();
    return true;
  }
  if (!FileRequests::HasDecider(view))
    return false;
  base::DictValue metadata;
  metadata.Set("mode", params.mode == Mode::kOpen            ? "open"
                       : params.mode == Mode::kOpenMultiple  ? "open-multiple"
                       : params.mode == Mode::kUploadFolder  ? "upload-folder"
                       : params.mode == Mode::kOpenDirectory ? "directory"
                                                             : "save");
  // <input type=file>, or File System Access's pickers (showOpenFilePicker,
  // showSaveFilePicker, showDirectoryPicker), which ask for files they can
  // write: what they may write is the "file-system-write" permission.
  metadata.Set("api", params.open_writable ? "file-system-access" : "input");
  metadata.Set("title", base::UTF16ToUTF8(params.title));
  metadata.Set("defaultFileName", params.default_file_name.AsUTF8Unsafe());
  base::ListValue accept;
  for (const std::u16string& type : params.accept_types)
    accept.Append(base::UTF16ToUTF8(type));
  metadata.Set("acceptTypes", std::move(accept));
  metadata.Set("capture", params.use_media_capture);
  metadata.Set("frameId", FrameId(frame));
  metadata.Set("mainFrame", frame->IsInPrimaryMainFrame());
  metadata.Set("origin", frame->GetLastCommittedOrigin().Serialize());
  auto& requests = FileRequests::Get();
  FileRequests::Reply reply = base::BindOnce(&SelectFiles, listener, params.mode);
  if (!requests.Ask(view, std::move(metadata), reply)) {
    listener->FileSelectionCanceled();
    return true;
  }
  RememberDocument(requests, FileDocuments(), {frame->GetWeakDocumentPtr(), frame->GetGlobalId()});
  return true;
}

}  // namespace zebkit

extern "C" {
void zebkit_view_on_external_protocol(zebkit_view* view, zebkit_external_protocol_func function, void* data) {
  if (!zebkit::LiveView(view) || !view->web_contents()) return;
  ++view->external_protocol_generation_;
  view->on_external_protocol_ = function; view->on_external_protocol_data_ = data;
}
void zebkit_view_on_file_chooser(zebkit_view* view, zebkit_file_chooser_func function, void* data) {
  ZEBKIT_LIVE_VIEW_OR_RETURN(view);
  zebkit::FileRequests::Get().Install(view, function, data);
}
void zebkit_view_on_file_chooser_result(zebkit_view* view, zebkit_file_chooser_result_func function, void* data) {
  ZEBKIT_LIVE_VIEW_OR_RETURN(view);
  zebkit::FileRequests::Get().Observe(view, function, data);
}
bool zebkit_view_set_file_chooser_timeout(zebkit_view* view, uint32_t milliseconds) {
  ZEBKIT_LIVE_VIEW_OR_RETURN(view, false);
  return zebkit::FileRequests::Get().SetTimeout(view, milliseconds);
}
char* zebkit_file_chooser_request_info(zebkit_file_chooser_request* request) {
  return zebkit::FileRequests::Get().Info(reinterpret_cast<uintptr_t>(request));
}
bool zebkit_file_chooser_respond(zebkit_file_chooser_request* request, const char* const* paths, size_t count,
                                 char** error) {
  auto fail = [&](const char* message) { if (error) *error = strdup(message); return false; };
  auto& requests = zebkit::FileRequests::Get();
  if (!requests.Ready()) return fail("file chooser replies need the engine thread");
  const auto id = reinterpret_cast<uintptr_t>(request);
  auto* entry = requests.Find(id);
  if (!entry) return fail("the request is no longer pending");
  const std::string* mode = entry->metadata.FindString("mode");
  if (count > 1 && (!mode || *mode != "open-multiple")) return fail("this chooser takes one path");
  if (count > 65536 || (count && !paths)) return fail("invalid paths");
  zebkit::FileTraits::Decision decision;
  for (size_t i = 0; i < count; ++i) {
    // SAFETY: the C API's array of @count strings.
    const char* path = UNSAFE_BUFFERS(paths[i]);
    if (!path || !base::IsStringUTF8(path)) return fail("invalid path");
    base::FilePath file = base::FilePath::FromUTF8Unsafe(path);
    if (!file.IsAbsolute() || file.ReferencesParent()) return fail("paths must be absolute");
    decision.paths.push_back(std::move(file));
  }
  zebkit::FileDocuments().erase(id);
  requests.Settle(id, std::move(decision), "answered");
  return true;
}
void zebkit_view_on_device_chooser(zebkit_view* view, zebkit_device_chooser_func function, void* data) {
  ZEBKIT_LIVE_VIEW_OR_RETURN(view);
  zebkit::DeviceRequests::Get().Install(view, function, data);
}
void zebkit_view_on_device_chooser_result(zebkit_view* view, zebkit_device_chooser_result_func function, void* data) {
  ZEBKIT_LIVE_VIEW_OR_RETURN(view);
  zebkit::DeviceRequests::Get().Observe(view, function, data);
}
bool zebkit_view_set_device_chooser_timeout(zebkit_view* view, uint32_t milliseconds) {
  ZEBKIT_LIVE_VIEW_OR_RETURN(view, false);
  return zebkit::DeviceRequests::Get().SetTimeout(view, milliseconds);
}
char* zebkit_device_chooser_request_info(zebkit_device_chooser_request* request) {
  return zebkit::DeviceRequests::Get().Info(reinterpret_cast<uintptr_t>(request));
}
bool zebkit_device_chooser_respond(zebkit_device_chooser_request* request, const char* device_id) {
  auto& requests = zebkit::DeviceRequests::Get();
  if (!requests.Ready()) return false;
  const auto id = reinterpret_cast<uintptr_t>(request);
  auto* entry = requests.Find(id);
  if (!entry) return false;
  zebkit::DeviceTraits::Decision decision;
  if (device_id) {
    // Only a device the request lists now.
    const base::ListValue* devices = entry->metadata.FindList("devices");
    bool listed = false;
    if (devices) {
      for (const base::Value& device : *devices) {
        const std::string* known = device.is_dict() ? device.GetDict().FindString("id") : nullptr;
        listed |= known && *known == device_id;
      }
    }
    if (!listed) return false;
    decision.id = device_id;
  }
  zebkit::DeviceDocuments().erase(id);
  requests.Settle(id, std::move(decision), "answered");
  return true;
}
void zebkit_view_on_eye_dropper(zebkit_view* view, zebkit_eye_dropper_func function, void* data) {
  ZEBKIT_LIVE_VIEW_OR_RETURN(view);
  zebkit::EyeDropperRequests::Get().Install(view, function, data);
}
void zebkit_view_on_eye_dropper_result(zebkit_view* view, zebkit_eye_dropper_result_func function, void* data) {
  ZEBKIT_LIVE_VIEW_OR_RETURN(view);
  zebkit::EyeDropperRequests::Get().Observe(view, function, data);
}
bool zebkit_view_set_eye_dropper_timeout(zebkit_view* view, uint32_t milliseconds) {
  ZEBKIT_LIVE_VIEW_OR_RETURN(view, false);
  return zebkit::EyeDropperRequests::Get().SetTimeout(view, milliseconds);
}
char* zebkit_eye_dropper_request_info(zebkit_eye_dropper_request* request) {
  return zebkit::EyeDropperRequests::Get().Info(reinterpret_cast<uintptr_t>(request));
}
bool zebkit_eye_dropper_respond(zebkit_eye_dropper_request* request, const char* color) {
  auto& requests = zebkit::EyeDropperRequests::Get();
  if (!requests.Ready()) return false;
  const auto id = reinterpret_cast<uintptr_t>(request);
  if (!requests.Find(id)) return false;
  zebkit::EyeDropperTraits::Decision decision;
  if (color) {
    uint32_t value = 0;
    const std::string_view text(color);
    if (text.size() != 7 || text[0] != '#' || !base::HexStringToUInt(text.substr(1), &value)) return false;
    decision.color = SkColorSetRGB(value >> 16, (value >> 8) & 0xff, value & 0xff);
  }
  zebkit::EyeDropperDocuments().erase(id);
  requests.Settle(id, std::move(decision), "answered");
  return true;
}
void zebkit_view_on_media_access(zebkit_view* view, zebkit_media_access_func function, void* data) {
  ZEBKIT_LIVE_VIEW_OR_RETURN(view);
  zebkit::MediaRequests::Get().Install(view, function, data);
}
void zebkit_view_on_media_access_result(zebkit_view* view, zebkit_media_access_result_func function, void* data) {
  ZEBKIT_LIVE_VIEW_OR_RETURN(view);
  zebkit::MediaRequests::Get().Observe(view, function, data);
}
bool zebkit_view_set_media_access_timeout(zebkit_view* view, uint32_t milliseconds) {
  ZEBKIT_LIVE_VIEW_OR_RETURN(view, false);
  return zebkit::MediaRequests::Get().SetTimeout(view, milliseconds);
}
char* zebkit_media_access_request_info(zebkit_media_access_request* request) {
  return zebkit::MediaRequests::Get().Info(reinterpret_cast<uintptr_t>(request));
}
bool zebkit_media_access_respond(zebkit_media_access_request* request, bool allow, const char* audio_device_id,
                                 const char* video_device_id) {
  auto& requests = zebkit::MediaRequests::Get();
  if (!requests.Ready()) return false;
  const auto id = reinterpret_cast<uintptr_t>(request);
  if (!requests.Find(id)) return false;
  if (!allow && (audio_device_id || video_device_id)) return false;
  zebkit::MediaTraits::Decision decision;
  decision.allow = allow;
  if (audio_device_id) decision.audio = audio_device_id;
  if (video_device_id) decision.video = video_device_id;
  zebkit::MediaDocuments().erase(id);
  requests.Settle(id, std::move(decision), "answered");
  return true;
}
bool zebkit_view_set_reserved_keys(zebkit_view* view, const char* keys_json, char** error) {
  auto fail = [&](const char* message) { if (error) *error = strdup(message); return false; };
  if (!zebkit::LiveView(view) || !view->web_contents()) return fail("the view is closed");
  std::optional<base::Value> parsed = base::JSONReader::Read(keys_json ? keys_json : "", base::JSON_PARSE_RFC);
  if (!parsed || !parsed->is_list() || parsed->GetList().size() > 256)
    return fail("keys are a JSON array of at most 256 strings");
  std::set<std::string> keys;
  for (const base::Value& key : parsed->GetList()) {
    std::optional<std::string> canonical = key.is_string() ? zebkit::CanonicalKey(key.GetString()) : std::nullopt;
    if (!canonical) return fail("a key is modifiers and a KeyboardEvent.code, as Ctrl+Shift+KeyI");
    keys.insert(*canonical);
  }
  view->reserved_keys_ = std::move(keys);
  return true;
}
void zebkit_view_on_reserved_key(zebkit_view* view, zebkit_reserved_key_func function, void* data) {
  if (!zebkit::LiveView(view) || !view->web_contents()) return;
  ++view->reserved_key_generation_;
  view->on_reserved_key_ = function; view->on_reserved_key_data_ = data;
}
void zebkit_view_on_context_menu(zebkit_view* view, zebkit_context_menu_func function, void* data) {
  if (!zebkit::LiveView(view) || !view->web_contents()) return;
  ++view->context_menu_generation_;
  view->on_context_menu_ = function; view->on_context_menu_data_ = data;
}
bool zebkit_view_set_fullscreen_policy(zebkit_view* view, zebkit_fullscreen_policy policy) {
  if (!zebkit::LiveView(view) || !view->web_contents() || (policy != ZEBKIT_FULLSCREEN_ALLOW && policy != ZEBKIT_FULLSCREEN_DENY)) return false;
  view->fullscreen_policy_ = policy;
  if (policy == ZEBKIT_FULLSCREEN_DENY && view->web_contents()->IsFullscreen())
    view->web_contents()->ExitFullscreen(true);
  return true;
}
void zebkit_view_on_fullscreen(zebkit_view* view, zebkit_fullscreen_func function, void* data) {
  if (!zebkit::LiveView(view) || !view->web_contents()) return;
  ++view->fullscreen_generation_;
  view->on_fullscreen_ = function; view->on_fullscreen_data_ = data;
}
}  // extern "C"
