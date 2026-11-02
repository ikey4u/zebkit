// Copyright 2013 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "zebkit/engine/window.h"

#include <stdio.h>

#include <algorithm>
#include <optional>

#include <stddef.h>

#include <map>
#include <memory>
#include <string>
#include <utility>

#include "base/command_line.h"
#include "base/compiler_specific.h"
#include "base/memory/weak_ptr.h"
#include "base/functional/callback_helpers.h"
#include "base/location.h"
#include "base/no_destructor.h"
#include "base/run_loop.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "build/build_config.h"
#include "components/custom_handlers/protocol_handler.h"
#include "components/custom_handlers/protocol_handler_registry.h"
#include "zebkit/engine/protocol_handlers.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/color_chooser.h"
#include "content/public/browser/devtools_agent_host.h"
#include "content/public/browser/document_picture_in_picture_window_controller.h"
#include "content/public/browser/file_select_listener.h"
#include "content/public/browser/media_capture_devices.h"
#include "content/public/browser/media_stream_request.h"
#include "content/public/browser/permission_controller.h"
#include "content/public/browser/permission_descriptor_util.h"
#include "content/public/browser/permission_request_description.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/weak_document_ptr.h"
#include "third_party/blink/public/mojom/mediastream/media_stream.mojom.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/navigation_entry.h"
#include "content/public/browser/page.h"
#include "content/public/browser/picture_in_picture_window_controller.h"
#include "content/public/browser/render_process_host.h"
#include "content/public/browser/render_view_host.h"
#include "content/public/browser/render_widget_host.h"
#include "content/public/browser/renderer_preferences_util.h"
#include "content/public/browser/keyboard_event_processing_result.h"
#include "content/public/browser/web_contents.h"
#include "content/public/common/content_switches.h"
#include "zebkit/engine/file_select.h"
#include "zebkit/engine/engine.h"
#include "zebkit/engine/js_dialog_manager.h"
#include "zebkit/engine/page_internal.h"
#include "zebkit/engine/view.h"
#include "zebkit/engine/window_platform.h"
#include "media/media_buildflags.h"
#include "third_party/blink/public/common/peerconnection/webrtc_ip_handling_policy.h"
#include "third_party/blink/public/common/renderer_preferences/renderer_preferences.h"
#include "third_party/blink/public/mojom/choosers/file_chooser.mojom-forward.h"
#include "third_party/blink/public/mojom/input/pointer_lock_result.mojom.h"
#include "third_party/blink/public/mojom/window_features/window_features.mojom.h"

namespace zebkit {

using namespace content;
using content::Page;

namespace {
bool g_shut_down = false;

// Null until/unless the default main message loop is running.
base::OnceClosure& GetMainMessageLoopQuitClosure() {
  static base::NoDestructor<base::OnceClosure> closure;
  return *closure;
}

constexpr int kDefaultWidth = 800;
constexpr int kDefaultHeight = 600;
}  // namespace

std::vector<Window*> Window::windows_;

Window::Window(std::unique_ptr<WebContents> web_contents, Kind kind)
    : WebContentsObserver(web_contents.get()),
      web_contents_(std::move(web_contents)),
      kind_(kind),
      platform_(PlatformFor(kind)) {
  web_contents_->SetDelegate(this);
  UpdateFontRendererPreferencesFromSystemSettings(
      web_contents_->GetMutableRendererPrefs());
  windows_.push_back(this);
}

// Device capture follows the same controller as navigator.permissions and
// embedder prompts. Display/tab capture needs a chooser and is not granted here.
namespace {
Window::EmbedderHooks* g_embedder_hooks = nullptr;
}  // namespace

void Window::SetEmbedderHooks(EmbedderHooks* hooks) {
  g_embedder_hooks = hooks;
}

bool Window::CanEnterFullscreenModeForTab(RenderFrameHost* requesting_frame) {
  return !g_embedder_hooks ||
         g_embedder_hooks->CanEnterFullscreen(requesting_frame);
}

KeyboardEventProcessingResult Window::PreHandleKeyboardEvent(
    WebContents* source,
    const input::NativeWebKeyboardEvent& event) {
  return g_embedder_hooks && g_embedder_hooks->KeyBeforePage(source, event)
             ? KeyboardEventProcessingResult::HANDLED
             : KeyboardEventProcessingResult::NOT_HANDLED;
}

bool Window::HandleContextMenu(RenderFrameHost& render_frame_host,
                              const ContextMenuParams& params) {
  return g_embedder_hooks &&
         g_embedder_hooks->ContextMenu(render_frame_host, params);
}

bool Window::CheckMediaAccessPermission(RenderFrameHost* frame,
                                        const url::Origin& origin,
                                        blink::mojom::MediaStreamType type) {
  using Stream = blink::mojom::MediaStreamType;
  if (!frame || !frame->IsActive() || frame->GetLastCommittedOrigin() != origin)
    return false;
  blink::PermissionType permission;
  if (type == Stream::DEVICE_AUDIO_CAPTURE) permission = blink::PermissionType::AUDIO_CAPTURE;
  else if (type == Stream::DEVICE_VIDEO_CAPTURE) permission = blink::PermissionType::VIDEO_CAPTURE;
  else return false;
  return frame->GetBrowserContext()->GetPermissionController()
      ->GetPermissionStatusForCurrentDocument(
          PermissionDescriptorUtil::CreatePermissionDescriptorForPermissionType(permission), frame)
      == blink::mojom::PermissionStatus::GRANTED;
}

void Window::RequestMediaAccessPermission(WebContents* contents,
                                         const MediaStreamRequest& request,
                                         MediaResponseCallback callback) {
  using Stream = blink::mojom::MediaStreamType;
  using Result = blink::mojom::MediaStreamRequestResult;
  if (g_embedder_hooks && g_embedder_hooks->MediaAccess(contents, request, callback))
    return;
  auto* frame = RenderFrameHost::FromID(request.render_process_id, request.render_frame_id);
  const bool audio = request.audio_type == Stream::DEVICE_AUDIO_CAPTURE;
  const bool video = request.video_type == Stream::DEVICE_VIDEO_CAPTURE;
  if (!frame || !frame->IsActive() || WebContents::FromRenderFrameHost(frame) != contents ||
      frame->GetLastCommittedOrigin() != request.url_origin ||
      (!audio && request.audio_type != Stream::NO_SERVICE) ||
      (!video && request.video_type != Stream::NO_SERVICE) || (!audio && !video)) {
    std::move(callback).Run(blink::mojom::StreamDevicesSet(), Result::PERMISSION_DENIED, nullptr);
    return;
  }
  std::vector<blink::mojom::PermissionDescriptorPtr> permissions;
  if (audio) permissions.push_back(PermissionDescriptorUtil::CreatePermissionDescriptorForPermissionType(blink::PermissionType::AUDIO_CAPTURE));
  if (video) permissions.push_back(PermissionDescriptorUtil::CreatePermissionDescriptorForPermissionType(blink::PermissionType::VIDEO_CAPTURE));
  if (video && request.request_pan_tilt_zoom_permission)
    permissions.push_back(PermissionDescriptorUtil::CreatePermissionDescriptorForPermissionType(blink::PermissionType::CAMERA_PAN_TILT_ZOOM));
  PermissionRequestDescription description(std::move(permissions), request.user_gesture, request.security_origin);
  description.requested_audio_capture_device_ids = request.requested_audio_device_ids;
  description.requested_video_capture_device_ids = request.requested_video_device_ids;
  const auto count = description.permissions.size();
  frame->GetBrowserContext()->GetPermissionController()->RequestPermissionsFromCurrentDocument(
      frame, std::move(description), base::BindOnce(
          [](WeakDocumentPtr document, MediaStreamRequest request, size_t expected,
             MediaResponseCallback callback, const std::vector<PermissionResult>& results) {
            auto* frame = document.AsRenderFrameHostIfValid();
            if (!frame || !frame->IsActive() || results.size() != expected ||
                std::ranges::any_of(results, [](const auto& result) {
                  return result.status != blink::mojom::PermissionStatus::GRANTED;
                })) {
              std::move(callback).Run(blink::mojom::StreamDevicesSet(), Result::PERMISSION_DENIED, nullptr);
              return;
            }
            // IDs have already been translated from page hashes to raw IDs by
            // MediaStreamManager. Never silently substitute a different device.
            auto choose = [](const blink::MediaStreamDevices& devices,
                             const std::vector<std::string>& requested)
                -> std::optional<blink::MediaStreamDevice> {
              for (const auto& id : requested)
                for (const auto& device : devices) if (device.id == id) return device;
              if (requested.empty() && !devices.empty()) return devices.front();
              return std::nullopt;
            };
            auto* devices = MediaCaptureDevices::GetInstance();
            auto stream = blink::mojom::StreamDevices::New();
            if (request.audio_type == Stream::DEVICE_AUDIO_CAPTURE)
              stream->audio_device = choose(devices->GetAudioCaptureDevices(), request.requested_audio_device_ids);
            if (request.video_type == Stream::DEVICE_VIDEO_CAPTURE)
              stream->video_device = choose(devices->GetVideoCaptureDevices(), request.requested_video_device_ids);
            if ((request.audio_type == Stream::DEVICE_AUDIO_CAPTURE && !stream->audio_device) ||
                (request.video_type == Stream::DEVICE_VIDEO_CAPTURE && !stream->video_device)) {
              std::move(callback).Run(blink::mojom::StreamDevicesSet(), Result::NO_HARDWARE, nullptr);
              return;
            }
            blink::mojom::StreamDevicesSet selected;
            selected.stream_devices.push_back(std::move(stream));
            std::move(callback).Run(selected, Result::OK, nullptr);
          }, frame->GetWeakDocumentPtr(), request, count, std::move(callback)));
}

Window::~Window() {
  platform_->CleanUp(this);
  std::erase(windows_, this);
  web_contents_->SetDelegate(nullptr);
  web_contents_.reset();
}

Window* Window::CreateForContents(std::unique_ptr<WebContents> web_contents,
                                  const gfx::Size& initial_size,
                                  Kind kind) {
  WebContents* contents = web_contents.get();
  Window* window = new Window(std::move(web_contents), kind);
  window->platform_->Create(window, initial_size);
  // Note: Do not make RenderFrameHost or RenderViewHost specific state changes
  // here, because they will be forgotten after a cross-process navigation. Use
  // RenderFrameCreated or RenderViewCreated instead.
  base::CommandLine* command_line = base::CommandLine::ForCurrentProcess();
  if (command_line->HasSwitch(switches::kForceWebRtcIPHandlingPolicy)) {
    contents->GetMutableRendererPrefs()->webrtc_ip_handling_policy =
        blink::ToWebRTCIPHandlingPolicy(command_line->GetSwitchValueASCII(
            switches::kForceWebRtcIPHandlingPolicy));
  }
  window->platform_->SetContents(window);
  ApplyLanguages(contents);
  // If the RenderFrame was created during WebContents construction (as happens
  // for windows opened from the renderer) then the window won't hear about the
  // main frame being created as a WebContentsObserver.
  if (contents->GetPrimaryMainFrame()->IsRenderFrameLive())
    window->platform_->MainFrameCreated(window);
  return window;
}

void Window::SetMainMessageLoopQuitClosure(base::OnceClosure quit_closure) {
  GetMainMessageLoopQuitClosure() = std::move(quit_closure);
}

// static
namespace {
Window::NewWindowHandler& NewWindowHandlerForEmbedder() {
  static base::NoDestructor<Window::NewWindowHandler> handler;
  return *handler;
}
}  // namespace

void Window::SetNewWindowHandler(NewWindowHandler handler) {
  NewWindowHandlerForEmbedder() = std::move(handler);
}

namespace {
Window::PopupHandler& PopupHandlerForEmbedder() {
  static base::NoDestructor<Window::PopupHandler> handler;
  return *handler;
}
}  // namespace

void Window::SetPopupHandler(PopupHandler handler) {
  PopupHandlerForEmbedder() = std::move(handler);
}

namespace {
Window::KeyboardEventHandler& KeyboardEventHandlerForEmbedder() {
  static base::NoDestructor<Window::KeyboardEventHandler> handler;
  return *handler;
}
}  // namespace

void Window::SetKeyboardEventHandler(KeyboardEventHandler handler) {
  KeyboardEventHandlerForEmbedder() = std::move(handler);
}

namespace {
Window::FileChooserHandler& FileChooserHandlerForEmbedder() {
  static base::NoDestructor<Window::FileChooserHandler> handler;
  return *handler;
}
}  // namespace

void Window::SetFileChooserHandler(FileChooserHandler handler) {
  FileChooserHandlerForEmbedder() = std::move(handler);
}

bool Window::IsWebContentsCreationOverridden(
    RenderFrameHost* opener,
    SiteInstance* source_site_instance,
    mojom::WindowContainerType window_container_type,
    const GURL& opener_url,
    const std::string& frame_name,
    const GURL& target_url) {
  const NewWindowHandler& handler = NewWindowHandlerForEmbedder();
  return handler && handler.Run(web_contents(), target_url);
}

// static
Window* Window::FromWebContents(WebContents* web_contents) {
  for (Window* window : windows_) {
    if (window->web_contents() && window->web_contents() == web_contents) {
      return window;
    }
  }
  return nullptr;
}

// static
void Window::Shutdown() {
  if (g_shut_down)  // Idempotent.
    return;
  g_shut_down = true;

  DevToolsAgentHost::DetachAllClients();

  while (!Window::windows().empty())
    Window::windows().back()->Close();

  ShutdownPlatforms();

  for (auto it = RenderProcessHost::AllHostsIterator(); !it.IsAtEnd();
       it.Advance()) {
    it.GetCurrentValue()->DisableRefCounts();
  }
  auto& quit_loop = GetMainMessageLoopQuitClosure();
  if (quit_loop)
    std::move(quit_loop).Run();

  // Pump the message loop to allow window teardown tasks to run.
  base::RunLoop().RunUntilIdle();
}

gfx::Size Window::AdjustWindowSize(const gfx::Size& initial_size) {
  if (!initial_size.IsEmpty())
    return initial_size;
  return DefaultSize();
}

// static
Window* Window::Create(BrowserContext* browser_context,
                       const GURL& url,
                       const scoped_refptr<SiteInstance>& site_instance,
                       const gfx::Size& initial_size,
                       Kind kind) {
  WebContents::CreateParams create_params(browser_context, site_instance);
  Window* window = CreateForContents(WebContents::Create(create_params),
                                     AdjustWindowSize(initial_size), kind);
  if (!url.is_empty())
    window->LoadURL(url);
  return window;
}

void Window::RenderFrameCreated(RenderFrameHost* frame_host) {
  if (frame_host == frame_host->GetOutermostMainFrame()) {
    platform_->MainFrameCreated(this);
  }
}

void Window::LoadURL(const GURL& url) {
  NavigationController::LoadURLParams params(url);
  params.transition_type = ui::PageTransitionFromInt(
      ui::PAGE_TRANSITION_TYPED | ui::PAGE_TRANSITION_FROM_ADDRESS_BAR);
  web_contents_->GetController().LoadURLWithParams(params);
}

WebContents* Window::AddNewContents(
    WebContents* source,
    std::unique_ptr<WebContents> new_contents,
    const GURL& target_url,
    WindowOpenDisposition disposition,
    const blink::mojom::WindowFeatures& window_features,
    bool user_gesture,
    bool* was_blocked) {
  // If the window is opening a document picture-in-picture window, it needs to
  // inform the DocumentPictureInPictureWindowController.
  if (disposition == WindowOpenDisposition::NEW_PICTURE_IN_PICTURE) {
    DocumentPictureInPictureWindowController* controller =
        PictureInPictureWindowController::
            GetOrCreateDocumentPictureInPictureController(source);
    controller->Close(/*should_pause_video=*/false);
    controller->SetChildWebContents(new_contents.get());
    controller->Show();
  }

  base::WeakPtr<WebContents> result = new_contents->GetWeakPtr();
  // What an off-screen view opens is off-screen too.
  Window* popup = CreateForContents(
      std::move(new_contents), AdjustWindowSize(window_features.bounds.size()),
      kind_);
  // To the embedder, with its opener.
  if (const PopupHandler& handler = PopupHandlerForEmbedder(); handler)
    handler.Run(source, popup, target_url, disposition, window_features, user_gesture);
  // The embedder may close the new view synchronously from its callback.
  return result.get();
}

void Window::ResizeContents(const gfx::Size& content_size) {
  platform_->ResizeContents(this, content_size);
}

gfx::NativeWindow Window::native_window() {
  return platform_->GetNativeWindow(this);
}

// static
Window* Window::OpenFrom(WebContents* source,
                         scoped_refptr<SiteInstance> site_instance,
                         const GURL& url,
                         WindowOpenDisposition disposition,
                         bool user_gesture) {
  Window* opener = FromWebContents(source);
  // Nothing loaded yet: the view's policy is its own before the first request.
  Window* window = Create(source->GetBrowserContext(), GURL(), site_instance,
                          gfx::Size(), opener ? opener->kind() : Kind::kNative);
  base::WeakPtr<WebContents> contents = window->web_contents()->GetWeakPtr();
  if (const PopupHandler& handler = PopupHandlerForEmbedder(); handler) {
    handler.Run(source, window, url, disposition, blink::mojom::WindowFeatures(),
                user_gesture);
  }
  return contents ? window : nullptr;
}

WebContents* Window::OpenURLFromTab(
    WebContents* source,
    const OpenURLParams& params,
    base::OnceCallback<void(content::NavigationHandle&)>
        navigation_handle_callback) {
  WebContents* target = nullptr;
  switch (params.disposition) {
    case WindowOpenDisposition::CURRENT_TAB:
      target = source;
      break;

    // No window has a toolbar, and none of the kinds below differs, tabs being
    // windows.
    case WindowOpenDisposition::NEW_POPUP:
    case WindowOpenDisposition::NEW_WINDOW:
    case WindowOpenDisposition::NEW_BACKGROUND_TAB:
    case WindowOpenDisposition::NEW_FOREGROUND_TAB: {
      // The embedder's, when it takes it.
      if (const NewWindowHandler& handler = NewWindowHandlerForEmbedder();
          handler && handler.Run(source, params.url)) {
        return nullptr;
      }
      Window* new_window = OpenFrom(source, params.source_site_instance, params.url,
                                    params.disposition, params.user_gesture);
      // The application may close it from its callback.
      target = new_window ? new_window->web_contents() : nullptr;
      if (!target)
        return nullptr;
      break;
    }

    default:
      return nullptr;
  }

  base::WeakPtr<NavigationHandle> navigation_handle =
      target->GetController().LoadURLWithParams(
          NavigationController::LoadURLParams(params));

  if (navigation_handle_callback && navigation_handle) {
    std::move(navigation_handle_callback).Run(*navigation_handle);
  }

  return target;
}

void Window::EnterFullscreenModeForTab(
    RenderFrameHost* requesting_frame,
    const blink::mojom::FullscreenOptions& options) {
  ToggleFullscreenModeForTab(WebContents::FromRenderFrameHost(requesting_frame),
                             true);
}

void Window::ExitFullscreenModeForTab(WebContents* web_contents) {
  ToggleFullscreenModeForTab(web_contents, false);
}

void Window::ToggleFullscreenModeForTab(WebContents* web_contents,
                                       bool enter_fullscreen) {
  if (g_embedder_hooks && is_fullscreen_ != enter_fullscreen)
    g_embedder_hooks->FullscreenChanged(web_contents, enter_fullscreen);
  if (is_fullscreen_ != enter_fullscreen) {
    is_fullscreen_ = enter_fullscreen;
    web_contents->GetPrimaryMainFrame()
        ->GetRenderViewHost()
        ->GetWidget()
        ->SynchronizeVisualProperties();
  }
}

bool Window::IsFullscreenForTabOrPending(const WebContents* web_contents) {
  return is_fullscreen_;
}

blink::mojom::DisplayMode Window::GetDisplayMode(
    const WebContents* web_contents) {
  // TODO: should return blink::mojom::DisplayModeFullscreen wherever user puts
  // a browser window into fullscreen (not only in case of renderer-initiated
  // fullscreen mode): crbug.com/476874.
  return IsFullscreenForTabOrPending(web_contents)
             ? blink::mojom::DisplayMode::kFullscreen
             : blink::mojom::DisplayMode::kBrowser;
}

void Window::RegisterProtocolHandler(RenderFrameHost* requesting_frame,
                                    const std::string& protocol,
                                    const GURL& url,
                                    bool user_gesture) {
  BrowserContext* context = requesting_frame->GetBrowserContext();
  if (context->IsOffTheRecord())
    return;

  custom_handlers::ProtocolHandler handler =
      custom_handlers::ProtocolHandler::CreateProtocolHandler(
          protocol, url, GetProtocolHandlerSecurityLevel(requesting_frame));

  // The parameters's normalization process defined in the spec has been already
  // applied in the WebContentImpl class, so at this point it shouldn't be
  // possible to create an invalid handler.
  // https://html.spec.whatwg.org/multipage/system-state.html#normalize-protocol-handler-parameters
  DCHECK(handler.IsValid());

  custom_handlers::ProtocolHandlerRegistry* registry = ProtocolHandlers::GetForBrowserContext(context, true);
  DCHECK(registry);
  if (registry->SilentlyHandleRegisterHandlerRequest(handler))
    return;

  if (!user_gesture && !windows_.empty()) {
    // TODO(jfernandez): This is not strictly needed, but we need a way to
    // inform the observers in browser tests that the request has been
    // cancelled, to avoid timeouts. Chrome just holds the handler as pending in
    // the PageContentSettingsDelegate, but we don't have such thing in the
    // Content Window.
    registry->OnDenyRegisterProtocolHandler(handler);
    return;
  }

  // FencedFrames can not register to handle any protocols.
  if (requesting_frame->IsNestedWithinFencedFrame()) {
    registry->OnIgnoreRegisterProtocolHandler(handler);
    return;
  }

  // TODO(jfernandez): Are we interested at all on using the
  // PermissionRequestManager in the ContentShell ?
  if (registry->registration_mode() ==
      custom_handlers::RphRegistrationMode::kAutoAccept) {
    registry->OnAcceptRegisterProtocolHandler(handler);
  }
}

void Window::UnregisterProtocolHandler(RenderFrameHost* requesting_frame,
                                      const std::string& protocol,
                                      const GURL& url,
                                      bool user_gesture) {
  BrowserContext* context = requesting_frame->GetBrowserContext();
  if (context->IsOffTheRecord()) {
    return;
  }

  custom_handlers::ProtocolHandler handler =
      custom_handlers::ProtocolHandler::CreateProtocolHandler(
          protocol, url, GetProtocolHandlerSecurityLevel(requesting_frame));
  custom_handlers::ProtocolHandlerRegistry* registry = ProtocolHandlers::GetForBrowserContext(context, true);
  CHECK(registry);

  registry->RemoveHandler(handler);
}

void Window::RequestPointerLock(WebContents* web_contents,
                               bool user_gesture,
                               bool last_unlocked_by_target) {
  web_contents->GotResponseToPointerLockRequest(
      blink::mojom::PointerLockResult::kSuccess);
}

void Window::Close() {
  // A window owns itself, and goes with its platform's window when it has one.
  if (!platform_->Destroy(this))
    delete this;
}

void Window::CloseContents(WebContents* source) {
  Close();
}

void Window::BeforeUnloadFired(WebContents* web_contents,
                              bool proceed,
                              bool* proceed_to_fire_unload) {
  *proceed_to_fire_unload = proceed;
}

bool Window::CanOverscrollContent() {
#if defined(USE_AURA)
  return true;
#else
  return false;
#endif
}

JavaScriptDialogManager* Window::GetJavaScriptDialogManager(
    WebContents* source) {
  if (!dialog_manager_)
    dialog_manager_ = CreateDialogManager(this);
  return dialog_manager_.get();
}

bool Window::HandleKeyboardEvent(WebContents* source,
                                const input::NativeWebKeyboardEvent& event) {
  if (const KeyboardEventHandler& handler = KeyboardEventHandlerForEmbedder();
      handler && handler.Run(this, event)) {
    return true;
  }
  return false;
}

void Window::RendererUnresponsive(
    WebContents* source,
    RenderWidgetHost* render_widget_host,
    base::RepeatingClosure hang_monitor_restarter) {
  LOG(WARNING) << "renderer unresponsive";
}

void Window::ActivateContents(WebContents* contents) {
  platform_->Activate(this);
}

void Window::RunFileChooser(RenderFrameHost* render_frame_host,
                           scoped_refptr<FileSelectListener> listener,
                           const blink::mojom::FileChooserParams& params) {
  if (const FileChooserHandler& handler = FileChooserHandlerForEmbedder();
      handler && handler.Run(render_frame_host, listener, params)) {
    return;
  }
  FileSelectHelper::RunFileChooser(render_frame_host, std::move(listener),
                                   params);
}

void Window::EnumerateDirectory(WebContents* web_contents,
                               scoped_refptr<FileSelectListener> listener,
                               const base::FilePath& path) {
  FileSelectHelper::EnumerateDirectory(web_contents, std::move(listener), path);
}

bool Window::IsBackForwardCacheSupported(WebContents& web_contents) {
  return true;
}

PreloadingEligibility Window::IsPrerender2Supported(
    WebContents& web_contents,
    PreloadingTriggerType trigger_type) {
  return PreloadingEligibility::kEligible;
}

bool Window::ShouldAllowRunningInsecureContent(WebContents* web_contents,
                                               bool allowed_per_prefs,
                                               const url::Origin& origin,
                                               const GURL& resource_url) {
  return allowed_per_prefs;
}

PictureInPictureResult Window::EnterPictureInPicture(WebContents* web_contents) {
  return PictureInPictureResult::kNotSupported;
}

gfx::Size Window::DefaultSize() {
  static gfx::Size size;  // Only go through this method once.
  if (!size.IsEmpty())
    return size;
  // --window-size, as in Chrome.
  const std::string given =
      base::CommandLine::ForCurrentProcess()->GetSwitchValueASCII(
          kWindowSizeSwitch);
  int width, height;
  if (UNSAFE_TODO(sscanf(given.c_str(), "%d,%d", &width, &height)) == 2)
    size = gfx::Size(width, height);
  if (size.IsEmpty())
    size = gfx::Size(kDefaultWidth, kDefaultHeight);
  return size;
}

void Window::TitleWasSet(NavigationEntry* entry) {
  if (entry)
    UpdateTitle();
}

void Window::UpdateTitle() {
  if (!web_contents())
    return;
  // The application's, when it gave one.
  zebkit_view* view = ViewOf(web_contents());
  platform_->SetTitle(this, view && view->window_title_
                                ? base::UTF8ToUTF16(*view->window_title_)
                                : web_contents()->GetTitle());
}

void WindowPlatform::Activate(Window* window) {
  window->web_contents()->Focus();
}

}  // namespace zebkit
