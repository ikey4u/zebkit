// zebkit_view_attach() on views (Linux): the view's page in a WebView of
// its own, over the parent's page in the parent's window, placed by bounds
// rather than by the window's layout. zebkit_view_attach_native() on X11: the
// view's whole window becomes a child of the application's. engine_mac.mm has
// macOS's.

#include <algorithm>
#include <limits>
#include <map>
#include <memory>
#include <string>

#include "base/memory/raw_ptr.h"
#include "base/no_destructor.h"
#include "base/strings/string_number_conversions.h"
#include "content/public/browser/web_contents.h"
#include "content/shell/browser/shell.h"
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
  content::Shell* shell = view ? view->shell() : nullptr;
  return shell ? views::Widget::GetWidgetForNativeWindow(shell->window())
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

#endif  // defined(ZEBKIT_X11)

}  // namespace zebkit
