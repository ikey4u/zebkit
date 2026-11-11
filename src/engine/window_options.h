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
  bool resizable = true;
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
};

struct WindowGeometry {
  enum class State { kNormal, kMinimized, kMaximized, kFullscreen };
  State state = State::kNormal;
  bool visible = false;
  bool focused = false;
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
