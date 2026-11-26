// A window's options and state (<zebkit/window.h>, docs/SYSTEM_INTEGRATION.md
// section 7): parsed and validated here, at the C boundary, into typed values
// the platforms take; what the window is, as the C API reports it.

#ifndef ZEBKIT_ENGINE_WINDOW_OPTIONS_H_
#define ZEBKIT_ENGINE_WINDOW_OPTIONS_H_

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/values.h"
#include "third_party/skia/include/core/SkColor.h"
#include "ui/gfx/geometry/point.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/gfx/geometry/size.h"
#include "url/origin.h"

namespace zebkit {

struct WindowOptions {
  WindowOptions();
  WindowOptions(const WindowOptions& other);
  WindowOptions& operator=(const WindowOptions& other);
  WindowOptions(WindowOptions&&);
  WindowOptions& operator=(WindowOptions&&);
  ~WindowOptions();

  enum class Frame { kSystem, kOverlay, kNone };
  enum class Appearance { kSystem, kLight, kDark };
  enum class PageFullscreen { kContent, kWindow };
  enum class DragSource { kNone, kCss, kHost };

  // Creation only: a change later is requires-recreate.
  gfx::Size size;  // The content area, DIPs; empty for the engine's default.
  Frame frame = Frame::kSystem;
  bool fallback_to_system = false;
  bool show = true;

  // Runtime too.
  gfx::Size minimum;
  gfx::Size maximum;  // 0 in a dimension: no limit in it.
  bool resizable = true;
  // What the person can do to the window (the program's actions honour them
  // too): its buttons, its fullscreen, dragging it.
  bool minimizable = true, maximizable = true, closable = true, fullscreenable = true, movable = true;
  bool always_on_top = false;
  // How far above: a floating window's level, a status item's (above the
  // menu bar and docks), or the screen saver's (above everything).
  enum class OnTopLevel { kFloating, kStatus, kScreenSaver };
  OnTopLevel on_top_level = OnTopLevel::kFloating;
  float opacity = 1;
  // The content area's width over its height kept while resizing; 0 for none.
  double aspect_ratio = 0;
  // Left out of screenshots and screen sharing.
  bool content_protection = false;
  bool skip_taskbar = false;
  bool visible_on_all_workspaces = false;
  bool shadow = true;
  std::optional<gfx::Point> position;  // Screen DIPs.
  std::optional<std::u16string> title;
  int controls_height = 0;  // 0: the system's.
  std::optional<SkColor> controls_color, symbol_color;
  std::optional<SkColor> background;
  Appearance appearance = Appearance::kSystem;
  PageFullscreen page_fullscreen = PageFullscreen::kContent;
  DragSource drag_source = DragSource::kNone;
  std::vector<url::Origin> drag_origins;

  // The options the JSON gave, as given: "explicit" in the state.
  base::DictValue given;
};

// Parses @json over @base. At creation every field is allowed; later the
// creation fields fail with "requires-recreate: <field>". The whole of it is
// checked before anything is changed: unknown keys, wrong types and values
// out of range fail with a message naming the key, and @out is untouched.
bool ParseWindowOptions(std::string_view json, bool at_creation, const WindowOptions& base,
                        WindowOptions* out, std::string* error);

// Options for popups a page of a view opens: the creation and runtime fields
// but dragRegions, which a popup never takes from its opener.
bool ParsePopupWindowOptions(std::string_view json, WindowOptions* out, std::string* error);

enum class WindowAction {
  kShow, kHide, kMinimize, kMaximize, kRestore, kFullscreen, kExitFullscreen, kClose,
  // Centred on its screen; asks for the person's attention (the Dock's icon
  // bounces, the taskbar button flashes) until it is focused, or no longer.
  kCenter, kRequestAttention, kCancelAttention,
};
std::optional<WindowAction> WindowActionNamed(std::string_view name);
const char* WindowActionName(WindowAction action);

// A rectangle the window is dragged from (drag) or not (no-drag), in the
// content area's DIPs; later ones over earlier ones.
struct DragRegion {
  gfx::Rect bounds;
  bool drag = true;
};
// The application's regions, [{"x", "y", "width", "height", "drag"}], at most 1024.
bool ParseDragRegions(std::string_view json, std::vector<DragRegion>* out, std::string* error);
// Whether @point is in a drag region of @regions: the last that contains it decides.
bool InDragRegion(const std::vector<DragRegion>& regions, const gfx::Point& point);

// What a platform can do for a window, and what it is.
struct WindowCapabilities {
  bool overlay = false;
  bool none = false;
  bool position = false;  // Can be placed (not on Wayland).
  bool actions = false;   // minimize, maximize, fullscreen.
  bool controls_drawn_by_engine = false;  // Windows, Linux; macOS draws its own.
  bool controls_height = false;           // A title bar taller than the system's.
  bool appearance = false;  // The "appearance" option's light or dark frame (macOS).
  // Options honoured; where false, a value but the default fails "unsupported:
  // <option>" (UnsupportedOption()).
  bool always_on_top = false;  // Not on Wayland: no protocol for it.
  bool opacity = false;        // Not on Wayland.
  bool buttons = false;        // minimizable, maximizable, closable.
  bool movable = false;        // Not on Wayland, whose compositor moves windows.
  bool skip_taskbar = false;   // Windows, X11; macOS has no window in its Dock.
  bool visible_on_all_workspaces = false;  // macOS, X11.
  bool shadow = false;         // macOS; elsewhere the system's or compositor's.
  bool on_top_levels = false;  // alwaysOnTopLevel other than floating (macOS, Windows).
  bool aspect_ratio = false;   // Not on Wayland.
  bool content_protection = false;  // macOS, Windows: none on Linux.
};

// The first option of @options that @capabilities cannot honour with a value
// other than its default (alwaysOnTop false is every system's), as the C API
// reports it: "unsupported: <option> ... (capabilities.<name> is false)";
// empty when there is none.
std::string UnsupportedOption(const WindowCapabilities& capabilities, const WindowOptions& options);

struct WindowGeometry {
  enum class State { kNormal, kMinimized, kMaximized, kFullscreen };
  State state = State::kNormal;
  bool visible = false;
  bool focused = false;
  bool always_on_top = false;  // Above the other applications' windows, as the system has it.
  gfx::Rect bounds;    // Screen DIPs, with the frame.
  gfx::Size content;   // DIPs.
  float scale = 1;
  gfx::Rect titlebar;  // Content DIPs: the title bar's area free of buttons; empty for none.
  std::vector<gfx::Rect> controls;  // Content DIPs: the system's buttons over the page.
};

const char* StateName(WindowGeometry::State state);
const char* FrameName(WindowOptions::Frame frame);

}  // namespace zebkit

#endif  // ZEBKIT_ENGINE_WINDOW_OPTIONS_H_
