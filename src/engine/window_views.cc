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
#include "zebkit/engine/include/zebkit/zebkit.h"
#include "zebkit/engine/window.h"
#include "zebkit/engine/window_platform.h"

#if BUILDFLAG(IS_WIN)
#include <fcntl.h>
#include <io.h>
#endif

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
class NoSystemWindowParenting : public aura::client::WindowParentingClient {
 public:
  explicit NoSystemWindowParenting(aura::Window* root)
      : root_(root), inner_(aura::client::GetWindowParentingClient(root)) {
    aura::client::SetWindowParentingClient(root_, this);
  }
  ~NoSystemWindowParenting() override {
    aura::client::SetWindowParentingClient(root_, inner_);
  }

  aura::Window* GetDefaultParent(aura::Window* window,
                                 const gfx::Rect& bounds,
                                 int64_t display_id) override {
    ScopedNoSystemWindow no_window(bounds);
    return inner_->GetDefaultParent(window, bounds, display_id);
  }

 private:
  const raw_ptr<aura::Window> root_;
  const raw_ptr<aura::client::WindowParentingClient> inner_;
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
    // The contents own the window; the delegate outlives the widget's use of
    // it as it closes (CleanUp()).
    data.delegate = std::make_unique<views::WidgetDelegate>();
    data.delegate->SetContentsView(
        std::make_unique<WindowView>(base::WrapUnique(window)));
    data.delegate->SetHasWindowSizeControls(true);
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
    // Shown in SetContents(), so that the platform window need not change its
    // size again for the layout.
  }

  void SetContents(Window* window) override {
    Data& data = windows_[window];
    ViewOf(data.widget)->SetWebContents(window->web_contents(), data.content_size);
    views::WebContentsSetBackgroundColor::CreateForWebContentsWithColor(
        window->web_contents(),
        data.widget->GetColorProvider()->GetColor(ui::kColorWindowBackground));
    data.widget->GetNativeWindow()->GetHost()->Show();
    data.widget->Show();
  }

  void ResizeContents(Window* window, const gfx::Size& content_size) override {
    window->web_contents()->Resize(gfx::Rect(content_size));
  }

  void SetTitle(Window* window, const std::u16string& title) override {
    windows_[window].widget->widget_delegate()->SetTitle(title);
  }

  gfx::NativeWindow GetNativeWindow(Window* window) override {
    return windows_[window].widget->GetNativeWindow();
  }

  void CleanUp(Window* window) override {
    auto found = windows_.find(window);
    base::SequencedTaskRunner::GetCurrentDefault()->DeleteSoon(
        FROM_HERE, std::move(found->second.delegate));
    windows_.erase(found);
  }

  bool Destroy(Window* window) override {
    windows_[window].widget->CloseNow();
    return true;
  }

  void BringToFront(Window* window) override {
    windows_[window].widget->Activate();
    window->web_contents()->Focus();
  }

  void SetSizeLimits(Window* window,
                     const gfx::Size& minimum,
                     bool resizable) override {
    views::Widget* widget = windows_[window].widget;
    if (!widget)
      return;
    ViewOf(widget)->set_minimum_size(minimum);
    widget->widget_delegate()->SetCanResize(resizable);
    widget->widget_delegate()->SetCanMaximize(resizable);
    widget->OnSizeConstraintsChanged();
  }

 private:
  struct Data {
    gfx::Size content_size;
    // Destroys itself, through CloseNow().
    raw_ptr<views::Widget> widget = nullptr;
    std::unique_ptr<views::WidgetDelegate> delegate;
    std::unique_ptr<NoSystemWindowParenting> parenting;
  };

  static WindowView* ViewOf(views::Widget* widget) {
    return static_cast<WindowView*>(widget->widget_delegate()->GetContentsView());
  }

  base::flat_map<Window*, Data> windows_;
};

NativePlatform* g_native = nullptr;
wm::WMState* g_wm_state = nullptr;
display::Screen* g_screen = nullptr;
ViewsDelegate* g_views_delegate = nullptr;

}  // namespace

void InitializePlatforms() {
#if BUILDFLAG(IS_WIN)
  _setmode(_fileno(stdout), _O_BINARY);
  _setmode(_fileno(stderr), _O_BINARY);
#endif
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
