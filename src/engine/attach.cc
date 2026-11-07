// zebkit_view_attach() on views (Linux): the view's page in a WebView of
// its own, over the parent's page in the parent's window, placed by bounds
// rather than by the window's layout. zebkit_view_attach_native() on X11: the
// view's whole window becomes a child of the application's, as on Windows with
// an HWND. engine_mac.mm has macOS's.

#include <algorithm>
#include <limits>
#include <map>
#include <memory>
#include <string>

#include "base/memory/raw_ptr.h"
#include "base/no_destructor.h"
#include "base/strings/string_number_conversions.h"
#include "build/build_config.h"
#include "content/public/browser/web_contents.h"
#include "zebkit/engine/window.h"
#include "ui/views/controls/webview/webview.h"
#include "ui/views/view_class_properties.h"
#include "ui/views/widget/widget.h"
#include "zebkit/engine/engine.h"
#include "zebkit/engine/view.h"

#if defined(ZEBKIT_X11)
#include "base/functional/bind.h"
#include "base/task/single_thread_task_runner.h"
#include "ui/aura/window.h"
#include "ui/aura/window_tree_host_platform.h"
#include "ui/gfx/x/connection.h"
#include "ui/gfx/x/event.h"
#include "ui/gfx/x/event_observer.h"
#include "ui/gfx/x/xinput.h"
#include "ui/gfx/x/xproto.h"
#include "ui/platform_window/extensions/x11_extension.h"
#include "ui/platform_window/platform_window.h"
#endif

#if BUILDFLAG(IS_WIN)
#include <windows.h>

#include "ui/aura/window.h"
#include "ui/aura/window_tree_host.h"
#include "ui/base/win/hwnd_subclass.h"
#endif

namespace zebkit {

namespace {

struct Attached {
  base::WeakPtr<zebkit_view> parent;
  raw_ptr<views::WebView> host = nullptr;
};

std::map<const zebkit_view*, Attached>& AttachedViews() {
  static base::NoDestructor<std::map<const zebkit_view*, Attached>> attached;
  return *attached;
}

views::Widget* WidgetOf(zebkit_view* view) {
  Window* window = view ? view->window() : nullptr;
  return window ? views::Widget::GetWidgetForNativeWindow(window->native_window())
                : nullptr;
}

// The WebView in @view's tree that shows @contents.
views::WebView* WebViewShowing(views::View* view,
                               content::WebContents* contents) {
  if (views::WebView* web_view = views::AsViewClass<views::WebView>(view)) {
    if (web_view->web_contents() == contents)
      return web_view;
  }
  for (views::View* child : view->children()) {
    if (views::WebView* found = WebViewShowing(child, contents))
      return found;
  }
  return nullptr;
}

}  // namespace

bool AttachView(zebkit_view* view, zebkit_view* parent, const gfx::Rect& bounds) {
  if (!view || !parent || view == parent || !view->web_contents())
    return false;
  views::Widget* parent_widget = WidgetOf(parent);
  views::View* contents =
      parent_widget ? parent_widget->GetContentsView() : nullptr;
  if (!contents)
    return false;
  DetachViewNative(view);
  DetachView(view);
  // Out of its own window's WebView first: a page's window has one parent.
  views::Widget* own = WidgetOf(view);
  if (own && own->GetRootView()) {
    if (views::WebView* old =
            WebViewShowing(own->GetRootView(), view->web_contents())) {
      old->SetWebContents(nullptr);
    }
  }
  auto host = std::make_unique<views::WebView>(
      view->web_contents()->GetBrowserContext());
  // Over the page, where the bounds say, whatever the window's layout does.
  host->SetProperty(views::kViewIgnoredByLayoutKey, true);
  views::WebView* added = contents->AddChildView(std::move(host));
  added->SetWebContents(view->web_contents());
  added->SetBoundsRect(bounds);
  if (own)
    own->Hide();
  AttachedViews()[view] = {parent->GetWeakPtr(), added};
  return true;
}

void SetAttachedBounds(zebkit_view* view, const gfx::Rect& bounds) {
  auto it = AttachedViews().find(view);
  if (it != AttachedViews().end() && it->second.parent)
    it->second.host->SetBoundsRect(bounds);
}

void SetAttachedVisible(zebkit_view* view, bool visible) {
  auto it = AttachedViews().find(view);
  if (it != AttachedViews().end() && it->second.parent)
    it->second.host->SetVisible(visible);
}

void DetachView(zebkit_view* view) {
  auto it = AttachedViews().find(view);
  if (it == AttachedViews().end())
    return;
  // The host goes with its parent's window when that closes first.
  if (it->second.parent && it->second.host->parent()) {
    views::WebView* host = it->second.host;
    host->SetWebContents(nullptr);
    host->parent()->RemoveChildViewT(host);
  }
  AttachedViews().erase(it);
}

#if defined(ZEBKIT_X11)

namespace {

// A window manager that framed the window before it was attached puts it back
// in the root as it lets it go, and that can arrive after the attachment.
constexpr int kMaxRestores = 5;

struct NativeAttached {
  base::WeakPtr<zebkit_view> view;
  x11::Window window = x11::Window::None;
  x11::Window parent = x11::Window::None;
  gfx::Rect bounds;
  int restores = 0;
};

ui::X11Extension* X11ExtensionOf(views::Widget* widget) {
  auto* host = widget ? aura::WindowTreeHostPlatform::GetHostForWindow(
                            widget->GetNativeWindow())
                      : nullptr;
  return host && host->platform_window()
             ? ui::GetX11Extension(*host->platform_window())
             : nullptr;
}

x11::Future<void> Reparent(x11::Connection* connection,
                           const NativeAttached& attached) {
  return connection->ReparentWindow({attached.window, attached.parent,
                                     static_cast<int16_t>(attached.bounds.x()),
                                     static_cast<int16_t>(attached.bounds.y())});
}

void Configure(x11::Connection* connection, const NativeAttached& attached) {
  x11::ConfigureWindowRequest configure{.window = attached.window};
  configure.x = attached.bounds.x();
  configure.y = attached.bounds.y();
  configure.width = attached.bounds.width();
  configure.height = attached.bounds.height();
  configure.border_width = 0;
  connection->ConfigureWindow(configure);
}

// What X tells of the attached windows. Their geometry is not here: Chromium's
// x11::GeometryCache follows the window and every ancestor, so the Aura root's
// bounds, which are in the screen's coordinates, follow without being told.
class NativeWindows : public x11::EventObserver {
 public:
  std::map<const zebkit_view*, NativeAttached> attached;

  void Add(const zebkit_view* view, const NativeAttached& entry) {
    if (attached.empty())
      x11::Connection::Get()->AddEventObserver(this);
    attached[view] = entry;
  }

  void Remove(const zebkit_view* view) {
    if (attached.erase(view) && attached.empty())
      x11::Connection::Get()->RemoveEventObserver(this);
  }

  void OnEvent(const x11::Event& event) override {
    if (auto* reparent = event.As<x11::ReparentNotifyEvent>()) {
      for (auto& [view, entry] : attached) {
        if (entry.window != reparent->window || entry.parent == reparent->parent)
          continue;
        if (entry.restores++ < kMaxRestores) {
          Reparent(x11::Connection::Get(), entry).IgnoreError();
          Configure(x11::Connection::Get(), entry);
        }
        return;
      }
    } else if (auto* destroy = event.As<x11::DestroyNotifyEvent>()) {
      for (auto it = attached.begin(); it != attached.end(); ++it) {
        if (it->second.window != destroy->window)
          continue;
        // The parent's end took the window with it, as for a view attached in
        // another view's window. Not now: this is inside the event's dispatch.
        base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
            FROM_HERE, base::BindOnce(
                           [](base::WeakPtr<zebkit_view> view) {
                             if (view)
                               view->Close();
                           },
                           it->second.view));
        Remove(it->first);
        return;
      }
    } else if (auto* device = event.As<x11::Input::DeviceEvent>()) {
      if (device->opcode == x11::Input::DeviceEvent::ButtonPress)
        Pressed(device->event, device->time);
    } else if (auto* button = event.As<x11::ButtonEvent>()) {
      if (button->opcode == x11::ButtonEvent::Press)
        Pressed(button->event, button->time);
    }
  }

 private:
  // A window of the application's has the keyboard; a click takes it, as a
  // click in a toolkit's own child window does. A window manager only gives
  // it to the top-level window.
  void Pressed(x11::Window window, x11::Time time) {
    for (const auto& [view, entry] : attached) {
      if (entry.window == window) {
        x11::Connection::Get()
            ->SetInputFocus({x11::InputFocus::Parent, window, time})
            .IgnoreError();
        return;
      }
    }
  }
};

NativeWindows& Natives() {
  static base::NoDestructor<NativeWindows> windows;
  return *windows;
}

NativeAttached* NativeOf(const zebkit_view* view) {
  auto it = Natives().attached.find(view);
  return it == Natives().attached.end() ? nullptr : &it->second;
}

}  // namespace

bool AttachViewNative(zebkit_view* view,
                      uintptr_t native_parent,
                      const gfx::Rect& bounds,
                      std::string* error) {
  views::Widget* widget = WidgetOf(view);
  ui::X11Extension* extension = X11ExtensionOf(widget);
  if (!extension) {
    *error = zebkit_is_headless()
                 ? "the engine runs headless and the view has no window to "
                   "parent; draw the page in your window with off-screen views "
                   "(<zebkit/offscreen.h>)"
                 : "a Wayland client cannot put its window in another's; draw "
                   "the page in your window with off-screen views "
                   "(<zebkit/offscreen.h>)";
    return false;
  }
  constexpr int kMaxCoordinate = std::numeric_limits<int16_t>::max();
  constexpr int kMinCoordinate = std::numeric_limits<int16_t>::min();
  if (bounds.width() <= 0 || bounds.height() <= 0 ||
      bounds.width() > kMaxCoordinate || bounds.height() > kMaxCoordinate ||
      bounds.x() < kMinCoordinate || bounds.x() > kMaxCoordinate ||
      bounds.y() < kMinCoordinate || bounds.y() > kMaxCoordinate) {
    *error = "the bounds are outside what X11 takes";
    return false;
  }
  if (native_parent > std::numeric_limits<uint32_t>::max()) {
    *error = "not an X11 window id";
    return false;
  }
  auto* connection = x11::Connection::Get();
  const auto parent = static_cast<x11::Window>(native_parent);
  auto geometry = connection->GetGeometry(parent).Sync();
  if (!geometry) {
    *error = "there is no X11 window " + base::NumberToString(native_parent);
    return false;
  }
  if (geometry->root != connection->default_root()) {
    *error = "the X11 window is on another screen than the engine's";
    return false;
  }

  DetachView(view);
  NativeAttached entry;
  entry.view = view->GetWeakPtr();
  entry.window = static_cast<x11::Window>(
      widget->GetNativeWindow()->GetHost()->GetAcceleratedWidget());
  entry.parent = parent;
  entry.bounds = bounds;
  // No window manager is to frame it again, here or after a later unmap.
  extension->SetOverrideRedirect(true);
  // Its own window, or one inside it, cannot be its parent.
  if (Reparent(connection, entry).Sync().error) {
    extension->SetOverrideRedirect(false);
    *error = "X11 refused that window as the parent";
    return false;
  }
  Natives().Add(view, entry);
  Configure(connection, entry);
  connection->Flush();
  return true;
}

bool SetAttachedNativeBounds(zebkit_view* view, const gfx::Rect& bounds) {
  NativeAttached* entry = NativeOf(view);
  if (!entry)
    return false;
  entry->bounds = gfx::Rect(bounds.origin(), gfx::Size(std::max(bounds.width(), 1),
                                                       std::max(bounds.height(), 1)));
  Configure(x11::Connection::Get(), *entry);
  x11::Connection::Get()->Flush();
  return true;
}

bool SetAttachedNativeSize(zebkit_view* view, const gfx::Size& size) {
  NativeAttached* entry = NativeOf(view);
  return entry && SetAttachedNativeBounds(
                      view, gfx::Rect(entry->bounds.origin(), size));
}

bool SetAttachedNativeVisible(zebkit_view* view, bool visible) {
  views::Widget* widget = NativeOf(view) ? WidgetOf(view) : nullptr;
  if (!widget)
    return false;
  // Shown without taking the keyboard from the application.
  if (visible)
    widget->ShowInactive();
  else
    widget->Hide();
  return true;
}

bool FocusAttachedNative(zebkit_view* view) {
  NativeAttached* entry = NativeOf(view);
  if (!entry)
    return false;
  x11::Connection::Get()
      ->SetInputFocus({x11::InputFocus::Parent, entry->window,
                       x11::Time::CurrentTime})
      .IgnoreError();
  x11::Connection::Get()->Flush();
  return true;
}

bool DetachViewNative(zebkit_view* view) {
  NativeAttached* entry = NativeOf(view);
  if (!entry)
    return false;
  const x11::Window window = entry->window;
  views::Widget* widget = WidgetOf(view);
  // Back in the root where it is on the screen, for the window manager again.
  const gfx::Rect screen =
      widget ? widget->GetWindowBoundsInScreen() : gfx::Rect();
  Natives().Remove(view);
  auto* connection = x11::Connection::Get();
  connection->ReparentWindow({window, connection->default_root(),
                              static_cast<int16_t>(screen.x()),
                              static_cast<int16_t>(screen.y())});
  if (widget) {
    if (ui::X11Extension* extension = X11ExtensionOf(widget))
      extension->SetOverrideRedirect(false);
    widget->Show();
  }
  connection->Flush();
  return true;
}

void ForgetAttachedNative(zebkit_view* view) {
  Natives().Remove(view);
}

#elif BUILDFLAG(IS_WIN)

namespace {

// A click takes the keyboard, as a click in a toolkit's own child window does:
// Windows activates only the top-level window, which has the application's focus.
class ClickTakesKeyboard : public ui::HWNDMessageFilter {
 public:
  bool FilterMessage(HWND hwnd,
                     UINT message,
                     WPARAM w_param,
                     LPARAM l_param,
                     LRESULT* result) override {
    // The press itself, or through the page's own child window, which keeps its
    // WM_MOUSEACTIVATE to itself but tells its parent of a press.
    const UINT child_event = LOWORD(w_param);
    const bool press =
        message == WM_MOUSEACTIVATE ||
        (message == WM_PARENTNOTIFY &&
         (child_event == WM_LBUTTONDOWN || child_event == WM_RBUTTONDOWN ||
          child_event == WM_MBUTTONDOWN));
    if (press && ::GetFocus() != hwnd && !::IsChild(hwnd, ::GetFocus()))
      ::SetFocus(hwnd);
    return false;
  }
};

ClickTakesKeyboard* Keyboard() {
  static base::NoDestructor<ClickTakesKeyboard> filter;
  return filter.get();
}

struct NativeAttached {
  HWND window = nullptr;
  HWND parent = nullptr;
  LONG_PTR style = 0;
  LONG_PTR ex_style = 0;
  gfx::Rect bounds;
};

std::map<const zebkit_view*, NativeAttached>& Natives() {
  static base::NoDestructor<std::map<const zebkit_view*, NativeAttached>> attached;
  return *attached;
}

NativeAttached* NativeOf(const zebkit_view* view) {
  auto it = Natives().find(view);
  return it == Natives().end() ? nullptr : &it->second;
}

void Place(const NativeAttached& entry) {
  ::SetWindowPos(entry.window, nullptr, entry.bounds.x(), entry.bounds.y(),
                 entry.bounds.width(), entry.bounds.height(),
                 SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
}

}  // namespace

bool AttachViewNative(zebkit_view* view,
                      uintptr_t native_parent,
                      const gfx::Rect& bounds,
                      std::string* error) {
  views::Widget* widget = WidgetOf(view);
  if (!widget || zebkit_is_headless()) {
    *error = "the engine runs headless and the view has no window to parent; "
             "draw the page in your window with off-screen views "
             "(<zebkit/offscreen.h>)";
    return false;
  }
  if (bounds.width() <= 0 || bounds.height() <= 0) {
    *error = "the bounds are empty";
    return false;
  }
  const HWND parent = reinterpret_cast<HWND>(native_parent);
  if (!::IsWindow(parent)) {
    *error = "there is no window " + base::NumberToString(native_parent);
    return false;
  }
  const HWND window = widget->GetNativeWindow()->GetHost()->GetAcceleratedWidget();
  // Its own window, or one inside it, cannot be its parent.
  if (window == parent || ::IsChild(window, parent)) {
    *error = "that window cannot be the parent";
    return false;
  }
  DetachView(view);
  DetachViewNative(view);
  NativeAttached entry;
  entry.window = window;
  entry.parent = parent;
  entry.bounds = bounds;
  entry.style = ::GetWindowLongPtrW(window, GWL_STYLE);
  entry.ex_style = ::GetWindowLongPtrW(window, GWL_EXSTYLE);
  ::SetWindowLongPtrW(
      window, GWL_STYLE,
      (entry.style & ~(WS_POPUP | WS_OVERLAPPEDWINDOW)) | WS_CHILD | WS_CLIPSIBLINGS);
  ::SetWindowLongPtrW(window, GWL_EXSTYLE,
                      entry.ex_style & ~(WS_EX_APPWINDOW | WS_EX_WINDOWEDGE | WS_EX_DLGMODALFRAME));
  if (!::SetParent(window, parent)) {
    ::SetWindowLongPtrW(window, GWL_STYLE, entry.style);
    ::SetWindowLongPtrW(window, GWL_EXSTYLE, entry.ex_style);
    *error = "Windows refused that window as the parent: error " +
             base::NumberToString(::GetLastError());
    return false;
  }
  Natives()[view] = entry;
  Place(entry);
  ui::HWNDSubclass::AddFilterToTarget(window, Keyboard());
  return true;
}

bool SetAttachedNativeBounds(zebkit_view* view, const gfx::Rect& bounds) {
  NativeAttached* entry = NativeOf(view);
  if (!entry)
    return false;
  entry->bounds = gfx::Rect(bounds.origin(), gfx::Size(std::max(bounds.width(), 1),
                                                       std::max(bounds.height(), 1)));
  Place(*entry);
  return true;
}

bool SetAttachedNativeSize(zebkit_view* view, const gfx::Size& size) {
  NativeAttached* entry = NativeOf(view);
  return entry && SetAttachedNativeBounds(
                      view, gfx::Rect(entry->bounds.origin(), size));
}

bool SetAttachedNativeVisible(zebkit_view* view, bool visible) {
  NativeAttached* entry = NativeOf(view);
  if (!entry)
    return false;
  // Shown without taking the keyboard from the application.
  ::ShowWindow(entry->window, visible ? SW_SHOWNOACTIVATE : SW_HIDE);
  return true;
}

bool FocusAttachedNative(zebkit_view* view) {
  NativeAttached* entry = NativeOf(view);
  if (!entry)
    return false;
  ::SetFocus(entry->window);
  return true;
}

bool DetachViewNative(zebkit_view* view) {
  NativeAttached* entry = NativeOf(view);
  if (!entry)
    return false;
  const NativeAttached copy = *entry;
  Natives().erase(view);
  ui::HWNDSubclass::RemoveFilterFromAllTargets(Keyboard());
  for (const auto& [other, attached] : Natives())
    ui::HWNDSubclass::AddFilterToTarget(attached.window, Keyboard());
  // Back on the desktop where it is on the screen.
  RECT screen = {};
  ::GetWindowRect(copy.window, &screen);
  ::SetParent(copy.window, nullptr);
  ::SetWindowLongPtrW(copy.window, GWL_STYLE, copy.style);
  ::SetWindowLongPtrW(copy.window, GWL_EXSTYLE, copy.ex_style);
  ::SetWindowPos(copy.window, nullptr, screen.left, screen.top,
                 screen.right - screen.left, screen.bottom - screen.top,
                 SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
  if (views::Widget* widget = WidgetOf(view))
    widget->Show();
  return true;
}

void ForgetAttachedNative(zebkit_view* view) {
  if (Natives().erase(view)) {
    ui::HWNDSubclass::RemoveFilterFromAllTargets(Keyboard());
    for (const auto& [other, attached] : Natives())
      ui::HWNDSubclass::AddFilterToTarget(attached.window, Keyboard());
  }
}

#else  // !defined(ZEBKIT_X11)

bool AttachViewNative(zebkit_view* view,
                      uintptr_t native_parent,
                      const gfx::Rect& bounds,
                      std::string* error) {
  *error = "windows of the application's are not built for this system";
  return false;
}
bool SetAttachedNativeBounds(zebkit_view* view, const gfx::Rect& bounds) {
  return false;
}
bool SetAttachedNativeSize(zebkit_view* view, const gfx::Size& size) {
  return false;
}
bool SetAttachedNativeVisible(zebkit_view* view, bool visible) {
  return false;
}
bool FocusAttachedNative(zebkit_view* view) {
  return false;
}
bool DetachViewNative(zebkit_view* view) {
  return false;
}
void ForgetAttachedNative(zebkit_view* view) {}

#endif  // defined(ZEBKIT_X11), BUILDFLAG(IS_WIN)

zebkit_native_window NativeWindowForView(zebkit_view* view) {
  // Follow the page's actual native view: its original Widget can be hidden
  // after AttachView has moved the contents into another zebkit window.
  auto* widget = views::Widget::GetTopLevelWidgetForNativeView(
      view->web_contents()->GetNativeView());
  if (!widget)
    return {};
#if defined(ZEBKIT_X11)
  if (!X11ExtensionOf(widget))
    return {};
  auto window = static_cast<x11::Window>(
      widget->GetNativeWindow()->GetHost()->GetAcceleratedWidget());
  for (const auto& [owner, attached] : Natives().attached) {
    if (attached.window == window) {
      window = attached.parent;
      break;
    }
  }
  // The top-level window, as GA_ROOT on Windows and the NSWindow on macOS: a
  // dialog is transient for that, not for the child an application attached
  // the page to. Its child of the root window, before any reparenting by the
  // window manager is looked through (_NET_WM frames are not the client's).
  auto* connection = x11::Connection::Get();
  const x11::Atom wm_state = connection->GetAtom("WM_STATE");
  for (int depth = 0; depth < 64; ++depth) {
    // Stop at the client window a window manager manages, the one with
    // WM_STATE; its frame, the next window up, is the manager's.
    uint32_t state = 0;
    if (connection->GetPropertyAs(window, wm_state, &state))
      break;
    auto tree = connection->QueryTree({window}).Sync();
    if (!tree || tree->parent == tree->root || tree->parent == x11::Window::None)
      break;
    window = tree->parent;
  }
  return {ZEBKIT_NATIVE_WINDOW_X11, static_cast<uintptr_t>(window)};
#elif BUILDFLAG(IS_WIN)
  HWND window = widget->GetNativeWindow()->GetHost()->GetAcceleratedWidget();
  HWND root = ::GetAncestor(window, GA_ROOT);
  return root ? zebkit_native_window{ZEBKIT_NATIVE_WINDOW_WIN32, reinterpret_cast<uintptr_t>(root)}
              : zebkit_native_window{};
#else
  return {};
#endif
}

}  // namespace zebkit
