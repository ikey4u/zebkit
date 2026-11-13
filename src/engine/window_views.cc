// Copyright 2020 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Windows of the system's as views widgets, on Linux and Windows.

#include <memory>

#include "base/check.h"
#include "base/command_line.h"
#include "base/containers/flat_map.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/ptr_util.h"
#include "base/task/sequenced_task_runner.h"
#include "build/build_config.h"
#include "content/public/browser/web_contents.h"
#include "ui/aura/client/window_parenting_client.h"
#include "ui/aura/window.h"
#include "ui/aura/window_observer.h"
#include "ui/aura/window_tree_host.h"
#include "ui/aura/window_tree_host_platform.h"
#include "ui/base/metadata/metadata_header_macros.h"
#include "ui/base/metadata/metadata_impl_macros.h"
#include "ui/color/color_id.h"
#include "ui/display/screen.h"
#include "ui/platform_window/stub/stub_window.h"
#include "ui/views/background.h"
#include "ui/views/controls/webview/web_contents_set_background_color.h"
#include "ui/views/controls/webview/webview.h"
#include "ui/views/layout/box_layout_view.h"
#include "ui/views/layout/fill_layout.h"
#include "ui/views/layout/layout_provider.h"
#include "ui/views/views_delegate.h"
#include "ui/views/widget/desktop_aura/desktop_native_widget_aura.h"
#include "ui/views/widget/desktop_aura/desktop_screen.h"
#include "ui/views/widget/native_widget_aura.h"
#include "ui/views/widget/widget.h"
#include "ui/views/widget/widget_delegate.h"
#include "ui/wm/core/wm_state.h"
#include "zebkit/engine/dock.h"
#include "ui/base/hit_test.h"
#include "ui/compositor/compositor.h"
#include "ui/gfx/geometry/insets.h"
#include "ui/views/widget/widget_observer.h"
#include "ui/views/window/caption_button_types.h"
#include "ui/views/window/frame_caption_button.h"
#include "ui/views/window/frame_view.h"
#include "ui/views/window/non_client_view.h"
#include "ui/views/window/vector_icons/vector_icons.h"
#if BUILDFLAG(IS_OZONE)
#include "ui/ozone/public/ozone_platform.h"
#endif
#include "zebkit/engine/include/zebkit/zebkit.h"
#include "zebkit/engine/window.h"
#include "zebkit/engine/window_platform.h"

namespace zebkit {

namespace {

// Top-level windows and their menus on the desktop; a widget in another, in it.
class ViewsDelegate : public views::ViewsDelegate {
 public:
#if BUILDFLAG(IS_WIN)
  HICON GetSmallWindowIcon() const override { return nullptr; }
#endif
  void OnBeforeWidgetInit(views::Widget::InitParams* params,
                          views::internal::NativeWidgetDelegate* delegate) override {
    if (params->opacity == views::Widget::InitParams::WindowOpacity::kInferred)
      params->opacity = views::Widget::InitParams::WindowOpacity::kOpaque;
    if (params->native_widget)
      return;
    if (params->parent && params->type != views::Widget::InitParams::TYPE_MENU &&
        params->type != views::Widget::InitParams::TYPE_TOOLTIP) {
      params->native_widget = new views::NativeWidgetAura(delegate);
    } else {
      params->native_widget = new views::DesktopNativeWidgetAura(delegate);
    }
  }

 private:
  views::LayoutProvider layout_provider_;
};

// The contents of a window: the page, and on a window's own window nothing else.
class WindowView : public views::View {
  METADATA_HEADER(WindowView, views::View)

 public:
  explicit WindowView(std::unique_ptr<Window> window)
      : window_(std::move(window)) {
    SetBackground(views::CreateSolidBackground(ui::kColorWindowBackground));
    SetUseDefaultFillLayout(true);
  }

  void set_minimum_size(const gfx::Size& size) { minimum_size_ = size; }

  void SetWebContents(content::WebContents* contents, const gfx::Size& size) {
    auto web_view =
        std::make_unique<views::WebView>(contents->GetBrowserContext());
    web_view->SetWebContents(contents);
    web_view->SetPreferredSize(size);
    views::WebView* added = AddChildView(std::move(web_view));
    contents->Focus();
    added->SizeToPreferredSize();
    // Keeps its origin.
    gfx::Rect bounds = GetWidget()->GetWindowBoundsInScreen();
    bounds.set_size(GetWidget()->GetRootView()->GetPreferredSize({}));
    GetWidget()->SetBounds(bounds);
  }

  // views::View: smaller than its first size, unless the application says.
  gfx::Size GetMinimumSize() const override { return minimum_size_; }

 private:
  // Destroyed with the widget, which closes the window.
  std::unique_ptr<Window> window_;
  gfx::Size minimum_size_;
};

BEGIN_METADATA(WindowView)
END_METADATA

// While it lives, the widgets made have no window of the system's: an off-screen
// view's, in a process with a display. What Chromium's headless platform gives
// every window, as a root that viz composites into nothing (patches/1072, 1073) with
// no connection to X11 or Wayland. Aura's way to choose a root's platform window
// has no other caller here, and its name is for that.
class ScopedNoSystemWindow
    : public aura::WindowTreeHostPlatform::PlatformWindowFactoryDelegateForTesting {
 public:
  explicit ScopedNoSystemWindow(const gfx::Rect& bounds) : bounds_(bounds) {
    aura::WindowTreeHostPlatform::SetPlatformWindowFactoryDelegateForTesting(this);
  }
  ~ScopedNoSystemWindow() override {
    aura::WindowTreeHostPlatform::SetPlatformWindowFactoryDelegateForTesting(nullptr);
  }

  std::unique_ptr<ui::PlatformWindow> Create(
      aura::WindowTreeHostPlatform* host) override {
    return std::make_unique<ui::StubWindow>(host, true, bounds_);
  }

 private:
  const gfx::Rect bounds_;
};

// What opens from an off-screen view's root, a <select>'s list in its own root
// as menus have, has no system window either.
class NoSystemWindowParenting : public aura::client::WindowParentingClient,
                                public aura::WindowObserver {
 public:
  explicit NoSystemWindowParenting(aura::Window* root)
      : root_(root), inner_(aura::client::GetWindowParentingClient(root)) {
    aura::client::SetWindowParentingClient(root_, this);
    root_->AddObserver(this);
  }
  ~NoSystemWindowParenting() override {
    if (root_) {
      root_->RemoveObserver(this);
      aura::client::SetWindowParentingClient(root_, inner_);
    }
  }

  // aura::WindowObserver: the root goes with the widget, and the client it had
  // before it.
  void OnWindowDestroying(aura::Window* window) override {
    root_->RemoveObserver(this);
    root_ = nullptr;
    inner_ = nullptr;
  }

  aura::Window* GetDefaultParent(aura::Window* window,
                                 const gfx::Rect& bounds,
                                 int64_t display_id) override {
    ScopedNoSystemWindow no_window(bounds);
    return inner_->GetDefaultParent(window, bounds, display_id);
  }

 private:
  raw_ptr<aura::Window> root_;
  raw_ptr<aura::client::WindowParentingClient> inner_;
};

// The frame of an "overlay" or "none" window (<zebkit/window.h>): the page from
// the window's top, under the window's buttons for an overlay, as Chrome draws
// them for an installed web app's Window Controls Overlay. What the system
// does with a title bar (moving, snapping, the system menu, maximizing on a
// double click) it does here from the hit test: resize edges, the buttons,
// HTCAPTION where the drag regions say, the page elsewhere.
class ZebkitFrameView : public views::FrameView {
  METADATA_HEADER(ZebkitFrameView, views::FrameView)

 public:
  ZebkitFrameView(views::Widget* widget, Window* window) : widget_(widget), window_(window) {
    if (window->options().frame != WindowOptions::Frame::kOverlay)
      return;
    auto add = [&](views::Button::PressedCallback callback, views::CaptionButtonIcon icon, int hit,
                   const gfx::VectorIcon& image) {
      auto* button = AddChildView(std::make_unique<views::FrameCaptionButton>(std::move(callback), icon, hit));
      button->SetImage(icon, views::FrameCaptionButton::Animate::kNo, image);
      return button;
    };
    minimize_ = add(base::BindRepeating(&views::Widget::Minimize, base::Unretained(widget)),
                    views::CAPTION_BUTTON_ICON_MINIMIZE, HTMINBUTTON, views::kWindowControlMinimizeOldIcon);
    maximize_ = add(base::BindRepeating(&views::Widget::Maximize, base::Unretained(widget)),
                    views::CAPTION_BUTTON_ICON_MAXIMIZE_RESTORE, HTMAXBUTTON, views::kWindowControlMaximizeOldIcon);
    restore_ = add(base::BindRepeating(&views::Widget::Restore, base::Unretained(widget)),
                   views::CAPTION_BUTTON_ICON_MAXIMIZE_RESTORE, HTMAXBUTTON, views::kWindowControlRestoreOldIcon);
    close_ = add(base::BindRepeating(&views::Widget::CloseWithReason, base::Unretained(widget),
                                     views::Widget::ClosedReason::kCloseButtonClicked),
                 views::CAPTION_BUTTON_ICON_CLOSE, HTCLOSE, views::kWindowControlCloseOldIcon);
    UpdateColors();
  }

  // The title bar area and its buttons, in the content area's DIPs.
  int TitlebarHeight() const {
    return std::max(window_->options().controls_height, kButtonHeight);
  }
  gfx::Rect ButtonsBounds() const {
    if (!close_)
      return gfx::Rect();
    const int width = kButtonWidth * 3;
    return gfx::Rect(std::max(0, this->width() - width), 0, std::min(width, this->width()), TitlebarHeight());
  }
  gfx::Rect Titlebar() const {
    if (!close_)
      return gfx::Rect();
    return gfx::Rect(0, 0, std::max(0, width() - ButtonsBounds().width()), TitlebarHeight());
  }

  void UpdateColors() {
    if (!close_)
      return;
    const WindowOptions& options = window_->options();
    for (views::FrameCaptionButton* button : {minimize_.get(), maximize_.get(), restore_.get(), close_.get()}) {
      if (options.symbol_color)
        button->SetIconColor(*options.symbol_color);
      else if (options.controls_color)
        button->SetBackgroundColor(*options.controls_color);
      button->SetBackground(options.controls_color ? views::CreateSolidBackground(*options.controls_color)
                                                   : nullptr);
    }
    SchedulePaint();
  }

  void UpdateState() {
    if (!close_)
      return;
    const bool maximized = widget_->IsMaximized();
    // In fullscreen the system hides its buttons; so does an overlay.
    const bool shown = !widget_->IsFullscreen();
    maximize_->SetVisible(shown && !maximized && widget_->widget_delegate()->CanMaximize());
    restore_->SetVisible(shown && maximized);
    minimize_->SetVisible(shown && widget_->widget_delegate()->CanMinimize());
    close_->SetVisible(shown);
    InvalidateLayout();
    SchedulePaint();
  }

  // views::FrameView:
  gfx::Rect GetBoundsForClientView() const override { return GetLocalBounds(); }
  gfx::Rect GetWindowBoundsForClientBounds(const gfx::Rect& client_bounds) const override {
    return client_bounds;
  }
  int NonClientHitTest(const gfx::Point& point) override {
    if (!bounds().Contains(point))
      return HTNOWHERE;
    const bool resizable = widget_->widget_delegate()->CanResize() && !widget_->IsMaximized() &&
                           !widget_->IsFullscreen();
    const int edge = GetHTComponentForFrame(point, gfx::Insets(kResizeBorder), kResizeCorner, kResizeCorner,
                                            resizable);
    if (edge != HTNOWHERE && edge != HTCAPTION && edge != HTCLIENT)
      return edge;
    for (views::FrameCaptionButton* button : {minimize_.get(), maximize_.get(), restore_.get(), close_.get()}) {
      if (button && button->GetVisible() && button->GetMirroredBounds().Contains(point))
        return button == close_ ? HTCLOSE : button == minimize_ ? HTMINBUTTON : HTMAXBUTTON;
    }
    // Where the page's app-region or the application says: the title bar's.
    if (window_->InDragRegion(point))
      return HTCAPTION;
    return HTCLIENT;
  }
  void InsertClientView(views::ClientView* client_view) override {
    client_view_ = AddChildViewAt(client_view, 0);
  }
  Views GetChildrenInZOrder() override {
    // The page first, the buttons over it.
    Views ordered;
    if (client_view_)
      ordered.push_back(client_view_.get());
    for (views::View* child : children())
      if (child != client_view_)
        ordered.push_back(child);
    return ordered;
  }
  void Layout(PassKey) override {
    if (client_view_)
      client_view_->SetBoundsRect(GetLocalBounds());
    if (!close_)
      return;
    int x = width();
    for (views::FrameCaptionButton* button : {close_.get(), restore_.get(), maximize_.get(), minimize_.get()}) {
      if (!button->GetVisible())
        continue;
      x -= kButtonWidth;
      button->SetBounds(x, 0, kButtonWidth, TitlebarHeight());
    }
  }

  // The rectangles of the buttons shown, in content DIPs.
  std::vector<gfx::Rect> Controls() const {
    std::vector<gfx::Rect> controls;
    for (views::FrameCaptionButton* button : {minimize_.get(), maximize_.get(), restore_.get(), close_.get()})
      if (button && button->GetVisible())
        controls.push_back(button->bounds());
    return controls;
  }

 private:
  // Windows' own caption buttons are 46 by 32; Linux's desktops near that.
  static constexpr int kButtonWidth = 46;
  static constexpr int kButtonHeight = 32;
  static constexpr int kResizeBorder = 6;
  static constexpr int kResizeCorner = 16;

  raw_ptr<views::Widget> widget_;
  raw_ptr<Window> window_;
  raw_ptr<views::View> client_view_ = nullptr;
  raw_ptr<views::FrameCaptionButton> minimize_ = nullptr, maximize_ = nullptr, restore_ = nullptr,
                                      close_ = nullptr;
};

BEGIN_METADATA(ZebkitFrameView)
END_METADATA

// The state of a window to its Window: the system's changes, whoever made them.
class StateObserver : public views::WidgetObserver {
 public:
  explicit StateObserver(Window* window) : window_(window) {}
  void OnWidgetVisibilityChanged(views::Widget* widget, bool visible) override {
    if (visible)
      Shown(widget);
    Changed(widget);
  }
  void OnWidgetActivationChanged(views::Widget* widget, bool active) override {
    if (active)
      Shown(widget);
    Changed(widget);
  }
  void OnWidgetBoundsChanged(views::Widget* widget, const gfx::Rect& bounds) override { Changed(widget); }
  void OnWidgetShowStateChanged(views::Widget* widget) override { Changed(widget); }
  void OnWidgetDestroying(views::Widget* widget) override {
#if BUILDFLAG(IS_WIN)
    if (aura::Window* native = widget->GetNativeWindow(); native && native->GetHost())
      ForgetDockWindow(native->GetHost()->GetAcceleratedWidget());
#endif
  }

 private:
  // The taskbar's badge and progress, which a window made after they were set
  // has not had: once its button is there (by its first activation, as
  // Chrome's taskbar decorations).
  void Shown(views::Widget* widget) {
#if BUILDFLAG(IS_WIN)
    if (aura::Window* native = widget->GetNativeWindow(); native && native->GetHost())
      ApplyDockStateToWindow(native->GetHost()->GetAcceleratedWidget());
#endif
  }
  void Changed(views::Widget* widget) {
    if (auto* frame = static_cast<ZebkitFrameView*>(widget->non_client_view()
                                                   ? widget->non_client_view()->frame_view()
                                                   : nullptr);
        frame && window_->options().frame != WindowOptions::Frame::kSystem)
      frame->UpdateState();
    window_->PlatformChanged();
  }
  raw_ptr<Window> window_;
};

class NativePlatform : public WindowPlatform {
 public:
  void Create(Window* window, const gfx::Size& initial_size) override {
    DCHECK(!windows_.contains(window));
    Data& data = windows_[window];
    data.content_size = initial_size;
    // Under --headless the process's platform has no system windows at all.
    const bool no_window =
        window->kind() == Window::Kind::kOffscreen && !zebkit_is_headless();
    data.widget = new views::Widget();
    views::Widget::InitParams params(
        views::Widget::InitParams::NATIVE_WIDGET_OWNS_WIDGET);
    params.bounds = gfx::Rect(initial_size);
    const WindowOptions& options = window->options();
    if (options.position)
      params.bounds.set_origin(*options.position);
    // No system frame: the page from the window's top (FrameView).
    const bool custom = options.frame != WindowOptions::Frame::kSystem && !no_window;
    if (custom)
      params.remove_standard_frame = true;
    // The contents own the window; the delegate outlives the widget's use of
    // it as it closes (CleanUp()).
    data.delegate = std::make_unique<views::WidgetDelegate>();
    data.delegate->SetContentsView(
        std::make_unique<WindowView>(base::WrapUnique(window)));
    data.delegate->SetHasWindowSizeControls(true);
    if (custom) {
      data.delegate->SetFrameViewFactory(base::BindRepeating(
          [](Window* window, views::Widget* widget) -> std::unique_ptr<views::FrameView> {
            return std::make_unique<ZebkitFrameView>(widget, window);
          },
          window));
    }
    params.delegate = data.delegate.get();
#if BUILDFLAG(IS_LINUX)
    // The application's own class, which its desktop entry names
    // (StartupWMClass), as Electron's windows have: the program's name.
    params.wm_class_class = base::CommandLine::ForCurrentProcess()
                                ->GetProgram()
                                .BaseName()
                                .AsUTF8Unsafe();
    params.wm_class_name = params.wm_class_class;
#endif
    if (no_window) {
      ScopedNoSystemWindow scoped(params.bounds);
      data.widget->Init(std::move(params));
      data.parenting = std::make_unique<NoSystemWindowParenting>(
          data.widget->GetNativeWindow()->GetRootWindow());
    } else {
      data.widget->Init(std::move(params));
    }
    data.observer = std::make_unique<StateObserver>(window);
    data.widget->AddObserver(data.observer.get());
    // Shown in SetContents(), so that the platform window need not change its
    // size again for the layout.
  }

  void SetContents(Window* window) override {
    Data& data = windows_[window];
    ViewOf(data.widget)->SetWebContents(window->web_contents(), data.content_size);
    ApplyBackground(window, data);
    // Hidden until the "show" action, when the options say so: not shown, not
    // activated.
    if (window->options().show) {
      data.widget->GetNativeWindow()->GetHost()->Show();
      data.widget->Show();
    }
    window->PlatformChanged();
  }

  void ResizeContents(Window* window, const gfx::Size& content_size) override {
    Data* data = Find(window);
    views::Widget* widget = data ? data->widget.get() : nullptr;
    // A window of the system's: its content area that size, the frame
    // around it, as macOS's does; without one, the page.
    if (!widget || !HasSystemWindow(window) || !widget->non_client_view()) {
      window->web_contents()->Resize(gfx::Rect(content_size));
      return;
    }
    const gfx::Rect client(widget->GetClientAreaBoundsInScreen().origin(), content_size);
    widget->SetBounds(widget->non_client_view()->GetWindowBoundsForClientBounds(client));
  }

  void SetTitle(Window* window, const std::u16string& title) override {
    if (Data* data = Find(window))
      data->widget->widget_delegate()->SetTitle(title);
  }

  gfx::NativeWindow GetNativeWindow(Window* window) override {
    Data* data = Find(window);
    return data ? data->widget->GetNativeWindow() : gfx::NativeWindow();
  }

  void CleanUp(Window* window) override {
    auto found = windows_.find(window);
    if (found->second.widget && found->second.observer)
      found->second.widget->RemoveObserver(found->second.observer.get());
    base::SequencedTaskRunner::GetCurrentDefault()->DeleteSoon(
        FROM_HERE, std::move(found->second.delegate));
    windows_.erase(found);
  }

  bool Destroy(Window* window) override {
    Data* data = Find(window);
    if (!data)
      return false;
    // Before the widget frees the root, and the client under this one.
    data->parenting.reset();
    data->widget->CloseNow();
    return true;
  }

  void BringToFront(Window* window) override {
    if (Data* data = Find(window))
      data->widget->Activate();
    window->web_contents()->Focus();
  }

  void SetSizeLimits(Window* window,
                     const gfx::Size& minimum,
                     bool resizable) override {
    Data* data = Find(window);
    views::Widget* widget = data ? data->widget.get() : nullptr;
    if (!widget)
      return;
    ViewOf(widget)->set_minimum_size(minimum);
    widget->widget_delegate()->SetCanResize(resizable);
    widget->widget_delegate()->SetCanMaximize(resizable);
    widget->OnSizeConstraintsChanged();
  }

  WindowCapabilities Capabilities(Window* window) override {
    WindowCapabilities capabilities;
    if (zebkit_is_headless() || (window && !HasSystemWindow(window)))
      return capabilities;
    capabilities.overlay = true;
    capabilities.none = true;
    capabilities.actions = true;
    capabilities.controls_drawn_by_engine = true;
    capabilities.controls_height = true;
#if BUILDFLAG(IS_OZONE)
    // Wayland's compositor places windows; X11's window manager takes a place.
    capabilities.position =
        ui::OzonePlatform::GetInstance()->GetPlatformProperties().supports_global_screen_coordinates;
#else
    capabilities.position = true;
#endif
    return capabilities;
  }

  WindowGeometry Geometry(Window* window) override {
    WindowGeometry geometry = WindowPlatform::Geometry(window);
    Data* data = Find(window);
    if (!data)
      return geometry;
    views::Widget* widget = data->widget;
    geometry.state = widget->IsFullscreen()  ? WindowGeometry::State::kFullscreen
                     : widget->IsMinimized() ? WindowGeometry::State::kMinimized
                     : widget->IsMaximized() ? WindowGeometry::State::kMaximized
                                             : WindowGeometry::State::kNormal;
    geometry.visible = widget->IsVisible();
    geometry.focused = widget->IsActive();
    geometry.bounds = widget->GetWindowBoundsInScreen();
    geometry.content = widget->GetClientAreaBoundsInScreen().size();
    if (auto* compositor = widget->GetCompositor())
      geometry.scale = compositor->device_scale_factor();
    if (auto* frame = FrameOf(window); frame && window->options().frame == WindowOptions::Frame::kOverlay &&
                                       geometry.state != WindowGeometry::State::kFullscreen) {
      geometry.titlebar = frame->Titlebar();
      geometry.controls = frame->Controls();
    }
    return geometry;
  }

  bool PerformAction(Window* window, WindowAction action, std::string* error) override {
    Data* data = Find(window);
    if (!data || !HasSystemWindow(window))
      return WindowPlatform::PerformAction(window, action, error);
    views::Widget* widget = data->widget;
    switch (action) {
      case WindowAction::kShow:
        widget->GetNativeWindow()->GetHost()->Show();
        widget->Show();
        break;
      case WindowAction::kHide: widget->Hide(); break;
      case WindowAction::kMinimize: widget->Minimize(); break;
      case WindowAction::kMaximize:
        if (!widget->widget_delegate()->CanMaximize()) {
          *error = "unsupported: the window is not resizable";
          return false;
        }
        widget->Maximize();
        break;
      case WindowAction::kRestore: widget->Restore(); break;
      case WindowAction::kFullscreen: widget->SetFullscreen(true); break;
      case WindowAction::kExitFullscreen: widget->SetFullscreen(false); break;
      case WindowAction::kClose: break;  // zebkit_view_request_close(), in window_api.cc.
    }
    return true;
  }

  void OptionsChanged(Window* window) override {
    Data* data = Find(window);
    if (!data)
      return;
    views::Widget* widget = data->widget;
    const WindowOptions& options = window->options();
    if (options.position && widget->GetWindowBoundsInScreen().origin() != *options.position) {
      gfx::Rect bounds = widget->GetWindowBoundsInScreen();
      bounds.set_origin(*options.position);
      widget->SetBounds(bounds);
    }
    ApplyBackground(window, *data);
    if (auto* frame = FrameOf(window)) {
      frame->UpdateColors();
      frame->UpdateState();
    }
  }

  void SetFullscreen(Window* window, bool fullscreen) override {
    if (Data* data = Find(window); data && HasSystemWindow(window))
      data->widget->SetFullscreen(fullscreen);
  }

 private:
  struct Data {
    gfx::Size content_size;
    std::unique_ptr<StateObserver> observer;
    // Destroys itself, through CloseNow().
    raw_ptr<views::Widget> widget = nullptr;
    std::unique_ptr<views::WidgetDelegate> delegate;
    std::unique_ptr<NoSystemWindowParenting> parenting;
  };

  // The window's, while it has a widget; never inserts one.
  Data* Find(Window* window) {
    auto found = windows_.find(window);
    return found != windows_.end() && found->second.widget ? &found->second : nullptr;
  }

  // A window the system shows: not headless and not off-screen.
  static bool HasSystemWindow(Window* window) {
    return !zebkit_is_headless() && window->kind() != Window::Kind::kOffscreen;
  }

  static WindowView* ViewOf(views::Widget* widget) {
    return static_cast<WindowView*>(widget->widget_delegate()->GetContentsView());
  }

  ZebkitFrameView* FrameOf(Window* window) {
    Data* data = Find(window);
    if (!data || window->options().frame == WindowOptions::Frame::kSystem || !data->widget->non_client_view())
      return nullptr;
    return static_cast<ZebkitFrameView*>(data->widget->non_client_view()->frame_view());
  }

  // The window's colour before the page's first frame and under a transparent
  // page: the options', or the toolkit's.
  static void ApplyBackground(Window* window, Data& data) {
    const SkColor color = window->options().background.value_or(
        data.widget->GetColorProvider()->GetColor(ui::kColorWindowBackground));
    ViewOf(data.widget)->SetBackground(views::CreateSolidBackground(color));
    // Again for a change: it keeps the colour it was made with otherwise.
    window->web_contents()->RemoveUserData(views::WebContentsSetBackgroundColor::UserDataKey());
    views::WebContentsSetBackgroundColor::CreateForWebContentsWithColor(window->web_contents(), color);
  }

  base::flat_map<Window*, Data> windows_;
};

NativePlatform* g_native = nullptr;
wm::WMState* g_wm_state = nullptr;
display::Screen* g_screen = nullptr;
ViewsDelegate* g_views_delegate = nullptr;

}  // namespace

void InitializePlatforms() {
  g_wm_state = new wm::WMState();
  if (!display::Screen::HasScreen())
    g_screen = views::CreateDesktopScreen().release();
  g_views_delegate = new ViewsDelegate();
  g_native = new NativePlatform();
}

void ShutdownPlatforms() {
  delete g_native;
  g_native = nullptr;
  delete g_views_delegate;
  g_views_delegate = nullptr;
  delete g_screen;
  g_screen = nullptr;
  delete g_wm_state;
  g_wm_state = nullptr;
}

WindowPlatform* PlatformFor(Window::Kind kind) {
  return g_native;
}

}  // namespace zebkit
