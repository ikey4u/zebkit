// Copyright 2013 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#ifndef ZEBKIT_ENGINE_WINDOW_H_
#define ZEBKIT_ENGINE_WINDOW_H_

#include <stdint.h>

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "base/functional/callback_forward.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/memory/scoped_refptr.h"
#include "build/build_config.h"
#include "content/public/browser/session_storage_namespace_handle.h"
#include "content/public/browser/web_contents_delegate.h"
#include "content/public/browser/web_contents_observer.h"
#include "ui/gfx/geometry/size.h"
#include "ui/gfx/native_ui_types.h"
#include "url/gurl.h"
#include "zebkit/engine/window_options.h"

namespace content {
class BrowserContext;
class FileSelectListener;
class JavaScriptDialogManager;
class RenderFrameHost;
class SiteInstance;
class WebContents;
}  // namespace content

namespace blink::mojom {
class FileChooserParams;
}

namespace zebkit {

class WindowPlatform;

inline constexpr char kWindowSizeSwitch[] = "window-size";

// One page and what holds it: a window of the system's (views, or an NSWindow
// on macOS), or for an off-screen view a root with no window at all. What a
// zebkit_view has in the browser, for the page's requests of its window.
class Window : public content::WebContentsDelegate,
               public content::WebContentsObserver {
 public:
  enum class Kind {
    kNative,     // A window of the system's, or as under --headless.
    kOffscreen,  // None: its frames and input are the application's.
  };

  ~Window() override;

  void LoadURL(const GURL& url);
  void Close();
  void ResizeContents(const gfx::Size& content_size);
  // The title again, from the application's or the page's.
  void UpdateTitle();
  Kind kind() const { return kind_; }

  // Closes all windows, pumps teardown tasks and signals the main message loop
  // to quit.
  static void Shutdown();  // Idempotent, can be called twice.
  // From Shutdown() on: no window is made, the platforms being gone.
  static bool IsShutDown();

  static Window* Create(content::BrowserContext* browser_context,
                        const GURL& url,
                        const scoped_refptr<content::SiteInstance>& site_instance,
                        const gfx::Size& initial_size,
                        Kind kind = Kind::kNative,
                        const WindowOptions* options = nullptr);

  // <zebkit/window.h>: the options the window was made with and has now, its
  // state as the system has it, what it is asked to do, and where it is
  // dragged from (docs/SYSTEM_INTEGRATION.md, 7).
  const WindowOptions& options() const { return options_; }
  // The frame asked for, when the system's was taken instead (fallback).
  WindowOptions::Frame requested_frame() const { return requested_frame_; }
  // Runtime options, already validated: to the platform, the page's display
  // mode and its title bar area.
  void SetOptions(WindowOptions options);
  // zebkit_view_set_title(): the "title" option; none for the page's.
  void SetTitle(std::optional<std::u16string> title);
  // For the windows pages of this one open; none: the system's frame.
  void SetPopupOptions(std::optional<WindowOptions> options) { popup_options_ = std::move(options); }
  WindowGeometry Geometry();
  WindowCapabilities Capabilities();
  bool PerformAction(WindowAction action, std::string* error);
  // The regions in force: the page's app-region (CSS) while its main frame is
  // of a granted origin, or the application's.
  const std::vector<DragRegion>& drag_regions() const { return drag_regions_; }
  bool InDragRegion(const gfx::Point& point_in_content) const;
  bool SetHostDragRegions(std::vector<DragRegion> regions, std::string* error);
  // The platform: the window's state, place, size, focus or buttons changed.
  void PlatformChanged();
  uint64_t revision() const { return revision_; }
  // Told after each change, on the engine's thread: a state change at once, a
  // size or place one with the others of the same task.
  void SetStateObserver(base::RepeatingClosure observer) { state_observer_ = std::move(observer); }
  bool page_fullscreen() const { return is_fullscreen_; }

  // The window of a page.
  static Window* FromWebContents(content::WebContents* web_contents);

  // Returns the currently open windows.
  static std::vector<Window*>& windows() { return windows_; }

  // Stores the supplied |quit_closure|, to be run when the last Window instance
  // is destroyed.
  static void SetMainMessageLoopQuitClosure(base::OnceClosure quit_closure);

  // what a page asks to open in a new window goes to the embedder
  // first, which returns true when it takes it: then no window is made.
  using NewWindowHandler =
      base::RepeatingCallback<bool(content::WebContents* opener, const GURL& url)>;
  static void SetNewWindowHandler(NewWindowHandler handler);

  // the window made for what a page opened (window.open, a link to a
  // new window), told to the embedder with the page that opened it, which
  // stays its opener: window.opener, postMessage() and window.closed work
  // between them, as a site's sign-in through a popup needs.
  using PopupHandler = base::RepeatingCallback<void(content::WebContents* opener,
                                                    Window* popup,
                                                    const GURL& url,
                                                    WindowOpenDisposition disposition,
                                                    const blink::mojom::WindowFeatures& features,
                                                    bool user_gesture)>;
  static void SetPopupHandler(PopupHandler handler);

  // a key the page did not take goes to the embedder first, which
  // returns true when it takes it (the application menu's keys).
  using KeyboardEventHandler = base::RepeatingCallback<bool(
      Window* window,
      const input::NativeWebKeyboardEvent& event)>;
  static void SetKeyboardEventHandler(KeyboardEventHandler handler);

  // a file chooser a page opens goes to the embedder first, which
  // returns true when it takes it, and then answers @listener itself.
  using FileChooserHandler = base::RepeatingCallback<bool(
      content::RenderFrameHost* render_frame_host,
      scoped_refptr<content::FileSelectListener> listener,
      const blink::mojom::FileChooserParams& params)>;
  static void SetFileChooserHandler(FileChooserHandler handler);

  // decisions of the embedder's, before the window's own.
  class EmbedderHooks {
   public:
    virtual ~EmbedderHooks() = default;
    // True: the page does not get this key.
    virtual bool KeyBeforePage(content::WebContents* contents,
                               const input::NativeWebKeyboardEvent& event) = 0;
    // True: no menu of the window's.
    virtual bool ContextMenu(content::RenderFrameHost& frame,
                             const content::ContextMenuParams& params) = 0;
    virtual bool CanEnterFullscreen(content::RenderFrameHost* frame) = 0;
    virtual void FullscreenChanged(content::WebContents* contents, bool entered) = 0;
    // True: the embedder has taken @callback and answers it.
    virtual bool MediaAccess(content::WebContents* contents,
                             const content::MediaStreamRequest& request,
                             content::MediaResponseCallback& callback) = 0;
  };
  static void SetEmbedderHooks(EmbedderHooks* hooks);

    content::WebContents* web_contents() const { return web_contents_.get(); }
  gfx::NativeWindow native_window();

  // WebContentsDelegate
  void RequestMediaAccessPermission(content::WebContents* web_contents,
                                    const content::MediaStreamRequest& request,
                                    content::MediaResponseCallback callback) override;
  bool CheckMediaAccessPermission(content::RenderFrameHost* frame,
                                  const url::Origin& security_origin,
                                  blink::mojom::MediaStreamType type) override;
  content::WebContents* OpenURLFromTab(
      content::WebContents* source,
      const content::OpenURLParams& params,
      base::OnceCallback<void(content::NavigationHandle&)>
          navigation_handle_callback) override;
  bool IsWebContentsCreationOverridden(
      content::RenderFrameHost* opener,
      content::SiteInstance* source_site_instance,
      content::mojom::WindowContainerType window_container_type,
      const GURL& opener_url,
      const std::string& frame_name,
      const GURL& target_url) override;
  content::WebContents* AddNewContents(
      content::WebContents* source,
      std::unique_ptr<content::WebContents> new_contents,
      const GURL& target_url,
      WindowOpenDisposition disposition,
      const blink::mojom::WindowFeatures& window_features,
      bool user_gesture,
      bool* was_blocked) override;
  void EnterFullscreenModeForTab(
      content::RenderFrameHost* requesting_frame,
      const blink::mojom::FullscreenOptions& options) override;
  void ExitFullscreenModeForTab(content::WebContents* web_contents) override;
  bool CanEnterFullscreenModeForTab(content::RenderFrameHost* requesting_frame) override;
  content::KeyboardEventProcessingResult PreHandleKeyboardEvent(
      content::WebContents* source,
      const input::NativeWebKeyboardEvent& event) override;
  bool HandleContextMenu(content::RenderFrameHost& render_frame_host,
                         const content::ContextMenuParams& params) override;
  bool IsFullscreenForTabOrPending(const content::WebContents* web_contents) override;
  blink::mojom::DisplayMode GetDisplayMode(
      const content::WebContents* web_contents) override;
  void RegisterProtocolHandler(content::RenderFrameHost* requesting_frame,
                               const std::string& protocol,
                               const GURL& url,
                               bool user_gesture) override;
  void UnregisterProtocolHandler(content::RenderFrameHost* requesting_frame,
                                 const std::string& protocol,
                                 const GURL& url,
                                 bool user_gesture) override;
  void RequestPointerLock(content::WebContents* web_contents,
                          bool user_gesture,
                          bool last_unlocked_by_target) override;
  void RequestKeyboardLock(content::WebContents* web_contents,
                           bool esc_key_locked) override;
  void CloseContents(content::WebContents* source) override;
  // a page kept by its beforeunload stays, rather than unloading
  // all the same (the delegate's default).
  void BeforeUnloadFired(content::WebContents* web_contents,
                         bool proceed,
                         bool* proceed_to_fire_unload) override;
  bool CanOverscrollContent() override;
  // window.focus (surfaces.json): Tab past the page's last element.
  bool TakeFocus(content::WebContents* source, bool reverse) override;
  content::JavaScriptDialogManager* GetJavaScriptDialogManager(
      content::WebContents* source) override;
  bool HandleKeyboardEvent(content::WebContents* source,
                           const input::NativeWebKeyboardEvent& event) override;
  void RendererUnresponsive(
      content::WebContents* source,
      content::RenderWidgetHost* render_widget_host,
      base::RepeatingClosure hang_monitor_restarter) override;
  void ActivateContents(content::WebContents* contents) override;
  void RunFileChooser(content::RenderFrameHost* render_frame_host,
                      scoped_refptr<content::FileSelectListener> listener,
                      const blink::mojom::FileChooserParams& params) override;
  bool UseFileChooserForFileSystemAccess() const override;
  void EnumerateDirectory(content::WebContents* web_contents,
                          scoped_refptr<content::FileSelectListener> listener,
                          const base::FilePath& path) override;
  bool IsBackForwardCacheSupported(content::WebContents& contents) override;
  content::PreloadingEligibility IsPrerender2Supported(
      content::WebContents& web_contents,
      content::PreloadingTriggerType trigger_type) override;
  bool ShouldAllowRunningInsecureContent(content::WebContents* web_contents,
                                         bool allowed_per_prefs,
                                         const url::Origin& origin,
                                         const GURL& resource_url) override;
  content::PictureInPictureResult EnterPictureInPicture(
      content::WebContents* web_contents) override;
  std::unique_ptr<content::EyeDropper> OpenEyeDropper(
      content::RenderFrameHost* frame,
      content::EyeDropperListener* listener) override;
  void DraggableRegionsChanged(
      const std::vector<blink::mojom::DraggableRegionPtr>& regions,
      content::WebContents* contents) override;

  static gfx::Size DefaultSize();

 private:
  Window(std::unique_ptr<content::WebContents> web_contents, Kind kind);

  // A window for a page made already, as one opened from a page is.
  static Window* CreateForContents(std::unique_ptr<content::WebContents> web_contents,
                                   const gfx::Size& initial_size,
                                   Kind kind,
                                   const WindowOptions* options = nullptr);

  // A window for what a page of @source opens with no opener of its own: a view
  // as every window a page opens is (Window::SetPopupHandler()).
  static Window* OpenFrom(content::WebContents* source,
                          scoped_refptr<content::SiteInstance> site_instance,
                          const GURL& url,
                          WindowOpenDisposition disposition,
                          bool user_gesture);

  // Adjust the size when Blink sends 0 for width and/or height.
  // This happens when Blink requests a default-sized window.
  static gfx::Size AdjustWindowSize(const gfx::Size& initial_size);

  void ToggleFullscreenModeForTab(content::WebContents* web_contents,
                                  bool enter_fullscreen);
  friend class BrowserClient;

  // WebContentsObserver
  void TitleWasSet(content::NavigationEntry* entry) override;
  void RenderFrameCreated(content::RenderFrameHost* frame_host) override;
  void PrimaryPageChanged(content::Page& page) override;
  void PrimaryMainFrameRenderProcessGone(base::TerminationStatus status) override;

  // Whether the page's own regions count now: CSS regions, and its main
  // frame's document of a granted origin.
  bool CssRegionsAllowed();
  // What the page is told of its title bar (Window Controls Overlay).
  void UpdateTitlebarArea(const WindowGeometry& geometry);
  void Notify();

  std::unique_ptr<content::JavaScriptDialogManager> dialog_manager_;
  std::unique_ptr<content::WebContents> web_contents_;
  const Kind kind_;
  const raw_ptr<WindowPlatform> platform_;
  bool is_fullscreen_ = false;

  WindowOptions options_;
  WindowOptions::Frame requested_frame_ = WindowOptions::Frame::kSystem;
  std::optional<WindowOptions> popup_options_;
  std::vector<DragRegion> drag_regions_;
  uint64_t revision_ = 0;
  gfx::Rect titlebar_area_;
  base::RepeatingClosure state_observer_;
  bool notify_pending_ = false;
  std::optional<WindowGeometry::State> told_state_;
  std::optional<bool> told_visible_;
  base::WeakPtrFactory<Window> weak_factory_{this};

  // A container of all the open windows. We use a vector so we can keep track
  // of ordering.
  static std::vector<Window*> windows_;
};

}  // namespace zebkit

#endif  // ZEBKIT_ENGINE_WINDOW_H_
