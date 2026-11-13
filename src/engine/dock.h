// The application's place in the Dock or the taskbar (docs/SYSTEM_INTEGRATION.md,
// 12): a badge, a progress, a request for the person's attention, and on macOS
// the Dock's menu. NSDockTile on macOS, ITaskbarList3 on Windows, the launcher
// API (com.canonical.Unity.LauncherEntry) on Linux, which desktops without it
// ignore.

#ifndef ZEBKIT_ENGINE_DOCK_H_
#define ZEBKIT_ENGINE_DOCK_H_

#include <string>
#include <vector>

#include "build/build_config.h"
#include "ui/gfx/native_ui_types.h"
#include "zebkit/engine/app.h"

namespace zebkit {

struct DockState {
  std::string badge;      // "" for none
  double progress = -1;   // 0..1, or below 0 for none
};

// What zebkit_app_set_badge() and zebkit_app_set_progress() set last.
const DockState& CurrentDockState();

// dock_mac.mm, dock_win.cc, dock_linux.cc.
void ApplyDockState(const DockState& state);
#if BUILDFLAG(IS_WIN)
// A window shown or activated (window_views.cc): its taskbar button, which a
// window made after the state was set has not had, gets the state too.
void ApplyDockStateToWindow(gfx::AcceleratedWidget window);
// A window going (window_views.cc): what its button had is forgotten, so that
// a later window given the same HWND is not judged to have it.
void ForgetDockWindow(gfx::AcceleratedWidget window);
#endif
void RequestDockAttention(bool critical);
// macOS: whether the Dock has a menu of the application's (engine_mac.mm asks).
bool DockMenuSupported();
// The Dock's menu (zebkit_app_set_dock_menu), and an item of it chosen.
const std::vector<MenuItem>& DockMenu();
void RunDockMenuItem(const std::string& id);

}  // namespace zebkit

#endif  // ZEBKIT_ENGINE_DOCK_H_
