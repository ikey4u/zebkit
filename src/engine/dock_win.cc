// The taskbar (dock.h): each engine window's button shows the progress, the
// badge as an overlay icon, and flashes for attention.

#include <windows.h>

#include <shobjidl.h>
#include <wrl/client.h>

#include <map>
#include <string>
#include <vector>

#include "base/no_destructor.h"
#include "base/strings/utf_string_conversions.h"
#include "base/win/scoped_gdi_object.h"
#include "cc/paint/paint_flags.h"
#include "third_party/skia/include/core/SkBitmap.h"
#include "ui/aura/window.h"
#include "ui/aura/window_tree_host.h"
#include "ui/gfx/canvas.h"
#include "ui/gfx/font_list.h"
#include "ui/gfx/geometry/point_f.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/gfx/win/icon_util.h"
#include "zebkit/engine/dock.h"
#include "zebkit/engine/window.h"

namespace zebkit {

namespace {

std::vector<HWND> Windows() {
  std::vector<HWND> windows;
  for (Window* window : Window::windows()) {
    aura::Window* native = window->native_window();
    if (native && native->GetHost())
      windows.push_back(native->GetHost()->GetAcceleratedWidget());
  }
  return windows;
}

Microsoft::WRL::ComPtr<ITaskbarList3> Taskbar() {
  static base::NoDestructor<Microsoft::WRL::ComPtr<ITaskbarList3>> taskbar;
  if (!*taskbar && SUCCEEDED(CoCreateInstance(CLSID_TaskbarList, nullptr, CLSCTX_INPROC_SERVER,
                                              IID_PPV_ARGS(&*taskbar))))
    (*taskbar)->HrInit();
  return *taskbar;
}

// The badge's text on a red disc, in the system's UI font; the taskbar shows
// its overlay at 16 by 16.
base::win::ScopedGDIObject<HICON> BadgeIcon(const std::string& text) {
  gfx::Canvas canvas(gfx::Size(32, 32), 1.0f, /*is_opaque=*/false);
  canvas.DrawColor(SK_ColorTRANSPARENT, SkBlendMode::kSrc);
  cc::PaintFlags disc;
  disc.setAntiAlias(true);
  disc.setColor(SkColorSetRGB(0xd9, 0x30, 0x25));
  canvas.DrawCircle(gfx::PointF(16, 16), 16, disc);
  const std::u16string shown = base::UTF8ToUTF16(text.size() > 3 ? text.substr(0, 3) : text);
  const gfx::FontList font = gfx::FontList().DeriveWithSizeDelta(shown.size() > 2 ? 2 : 8).DeriveWithWeight(
      gfx::Font::Weight::BOLD);
  canvas.DrawStringRectWithFlags(shown, font, SK_ColorWHITE, gfx::Rect(0, 0, 32, 32), gfx::Canvas::TEXT_ALIGN_CENTER);
  const SkBitmap bitmap = canvas.GetBitmap();
  return IconUtil::CreateHICONFromSkBitmap(bitmap);
}

// What each window's button was given last, so that showing or activating a
// window applies only what it lacks.
std::map<HWND, DockState>& Applied() {
  static base::NoDestructor<std::map<HWND, DockState>> applied;
  return *applied;
}

void ApplyTo(ITaskbarList3* taskbar, HWND window, const DockState& state, HICON badge) {
  if (state.progress < 0) {
    taskbar->SetProgressState(window, TBPF_NOPROGRESS);
  } else {
    taskbar->SetProgressState(window, TBPF_NORMAL);
    taskbar->SetProgressValue(window, static_cast<ULONGLONG>(state.progress * 1000), 1000);
  }
  // Before its button is there the taskbar refuses: tried again when the
  // window is next shown or activated.
  if (SUCCEEDED(taskbar->SetOverlayIcon(window, badge, base::UTF8ToWide(state.badge).c_str())))
    Applied()[window] = state;
}

bool Same(const DockState& a, const DockState& b) {
  return a.badge == b.badge && a.progress == b.progress;
}

}  // namespace

void ApplyDockState(const DockState& state) {
  auto taskbar = Taskbar();
  if (!taskbar)
    return;
  base::win::ScopedGDIObject<HICON> badge;
  if (!state.badge.empty())
    badge = BadgeIcon(state.badge);
  std::erase_if(Applied(), [](const auto& entry) { return !::IsWindow(entry.first); });
  for (HWND window : Windows())
    ApplyTo(taskbar.Get(), window, state, badge.get());
}

void ApplyDockStateToWindow(gfx::AcceleratedWidget window) {
  const DockState& state = CurrentDockState();
  auto found = Applied().find(window);
  // Nothing set yet, or this button has it already.
  if ((found == Applied().end() && state.badge.empty() && state.progress < 0) ||
      (found != Applied().end() && Same(found->second, state)))
    return;
  auto taskbar = Taskbar();
  if (!taskbar)
    return;
  base::win::ScopedGDIObject<HICON> badge;
  if (!state.badge.empty())
    badge = BadgeIcon(state.badge);
  ApplyTo(taskbar.Get(), window, state, badge.get());
}

void ForgetDockWindow(gfx::AcceleratedWidget window) {
  Applied().erase(window);
}

void RequestDockAttention(bool critical) {
  for (HWND window : Windows()) {
    FLASHWINFO flash = {sizeof(flash), window, static_cast<DWORD>(critical ? FLASHW_ALL | FLASHW_TIMERNOFG
                                                                           : FLASHW_TRAY | FLASHW_TIMERNOFG),
                        critical ? 0u : 3u, 0};
    FlashWindowEx(&flash);
  }
}

bool DockMenuSupported() {
  return false;
}

}  // namespace zebkit
