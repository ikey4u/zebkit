// <zebkit/window.h>: a view's window made with options, its state, actions,
// events and drag regions (docs/SYSTEM_INTEGRATION.md, section 7).

#include <string.h>

#include <string>
#include <utility>

#include "base/json/json_writer.h"
#include "base/strings/utf_string_conversions.h"
#include "base/values.h"
#include "build/build_config.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/web_contents.h"
#include "ui/display/screen.h"
#include "zebkit/engine/engine.h"
#include "zebkit/engine/include/zebkit/page.h"
#include "zebkit/engine/include/zebkit/window.h"
#include "zebkit/engine/view.h"
#include "zebkit/engine/window.h"
#include "zebkit/engine/window_options.h"
#include "zebkit/engine/window_platform.h"

namespace zebkit {

namespace {

bool OnEngineThread() {
  return CurrentlyOnEngineThread();
}

bool Fail(char** error, std::string message) {
  if (error)
    *error = strdup(message.c_str());
  return false;
}

base::DictValue RectValue(const gfx::Rect& rect) {
  base::DictValue value;
  value.Set("x", rect.x());
  value.Set("y", rect.y());
  value.Set("width", rect.width());
  value.Set("height", rect.height());
  return value;
}

base::DictValue CapabilitiesValue(const WindowCapabilities& capabilities) {
  base::DictValue value;
  base::ListValue frames;
  frames.Append("system");
  if (capabilities.overlay)
    frames.Append("overlay");
  if (capabilities.none)
    frames.Append("none");
  value.Set("frames", std::move(frames));
  value.Set("position", capabilities.position);
  value.Set("actions", capabilities.actions);
  value.Set("controlsDrawnByEngine", capabilities.controls_drawn_by_engine);
  value.Set("controlsHeight", capabilities.controls_height);
  value.Set("appearance", capabilities.appearance);
  return value;
}

const char* OwnerName(zebkit_view* view, Window* window) {
  if (!window || window->kind() == Window::Kind::kOffscreen || zebkit_is_headless())
    return "none";
  switch (ViewWindowOwner(view)) {
    case WindowOwner::kEngine: return "engine";
    case WindowOwner::kView: return "view";
    case WindowOwner::kApplication: return "application";
  }
  return "engine";
}

std::string StateJson(zebkit_view* view) {
  Window* window = view->window();
  base::DictValue state;
  if (!window)
    return "{}";
  const WindowGeometry geometry = window->Geometry();
  const std::string owner = OwnerName(view, window);
  state.Set("revision", static_cast<double>(window->revision()));
  state.Set("owner", owner);
  state.Set("state", StateName(geometry.state));
  state.Set("pageFullscreen", window->page_fullscreen());
  state.Set("visible", geometry.visible);
  state.Set("focused", geometry.focused);
  state.Set("bounds", RectValue(geometry.bounds));
  base::DictValue content;
  content.Set("width", geometry.content.width());
  content.Set("height", geometry.content.height());
  state.Set("content", std::move(content));
  state.Set("scale", geometry.scale);
  base::DictValue frame;
  frame.Set("requested", FrameName(window->requested_frame()));
  frame.Set("effective", FrameName(window->options().frame));
  state.Set("frame", std::move(frame));
  state.Set("titlebarArea", RectValue(window->options().frame == WindowOptions::Frame::kOverlay
                                          ? geometry.titlebar : gfx::Rect()));
  base::ListValue controls;
  for (const gfx::Rect& rect : geometry.controls)
    controls.Append(RectValue(rect));
  state.Set("controls", std::move(controls));
  state.Set("capabilities", CapabilitiesValue(window->Capabilities()));
  base::DictValue options;
  options.Set("explicit", window->options().given.Clone());
  state.Set("options", std::move(options));
  return base::WriteJson(state).value_or("{}");
}

void TellState(base::WeakPtr<zebkit_view> view) {
  if (!view || !view->on_window_state_)
    return;
  const std::string json = StateJson(view.get());
  view->on_window_state_(view.get(), json.c_str(), view->on_window_state_data_);
}

}  // namespace

}  // namespace zebkit

extern "C" {

zebkit_view* zebkit_view_new_with_options(zebkit_context* context, const char* url,
                                          const char* options_json, char** error) {
  using namespace zebkit;
  if (error)
    *error = nullptr;
  if (!OnEngineThread()) {
    Fail(error, "views are made on the engine's thread");
    return nullptr;
  }
  WindowOptions options;
  std::string message;
  if (!ParseWindowOptions(options_json ? options_json : "{}", true, WindowOptions(), &options, &message)) {
    Fail(error, message);
    return nullptr;
  }
  // A frame the system cannot give: an error, unless the system's will do.
  if (Window::IsShutDown()) {
    Fail(error, "the engine is stopping");
    return nullptr;
  }
  const WindowCapabilities capabilities = PlatformFor(Window::Kind::kNative)->Capabilities(nullptr);
  const bool frame_ok = options.frame == WindowOptions::Frame::kSystem ||
                        (options.frame == WindowOptions::Frame::kOverlay && capabilities.overlay) ||
                        (options.frame == WindowOptions::Frame::kNone && capabilities.none);
  if (!frame_ok && !options.fallback_to_system && !zebkit_is_headless()) {
    Fail(error, std::string("unsupported: frame ") + FrameName(options.frame) +
                    " on this system; \"fallback\": \"system\" takes the system's frame instead");
    return nullptr;
  }
  if (options.position && !capabilities.position && !zebkit_is_headless()) {
    Fail(error, "unsupported: x and y; this system's windows are placed by its compositor");
    return nullptr;
  }
  zebkit_view* view = NewView(context, url, options.size.width(), options.size.height(),
                              Window::Kind::kNative, &options);
  if (!view)
    Fail(error, "the view could not be made");
  return view;
}

char* zebkit_view_get_window(zebkit_view* view) {
  using namespace zebkit;
  if (!LiveView(view) || !view->window())
    return nullptr;
  return strdup(StateJson(view).c_str());
}

bool zebkit_view_set_window(zebkit_view* view, const char* options_json, char** error) {
  using namespace zebkit;
  if (error)
    *error = nullptr;
  if (!LiveView(view) || !view->window())
    return Fail(error, "a live view on the engine's thread");
  Window* window = view->window();
  WindowOptions options;
  std::string message;
  if (!ParseWindowOptions(options_json ? options_json : "{}", false, window->options(), &options, &message))
    return Fail(error, message);
  if (options.position != window->options().position && options.position &&
      !window->Capabilities().position && !zebkit_is_headless())
    return Fail(error, "unsupported: x and y; this system's windows are placed by its compositor");
  // What was given before and now, as given.
  base::DictValue given = window->options().given.Clone();
  for (auto [key, value] : options.given)
    given.Set(key, value.Clone());
  options.given = std::move(given);
  window->SetOptions(std::move(options));
  return true;
}

bool zebkit_view_window_action(zebkit_view* view, const char* action, char** error) {
  using namespace zebkit;
  if (error)
    *error = nullptr;
  if (!LiveView(view) || !view->window())
    return Fail(error, "a live view on the engine's thread");
  auto parsed = WindowActionNamed(action ? action : "");
  if (!parsed)
    return Fail(error, "unknown action; one of show, hide, minimize, maximize, restore, fullscreen, "
                       "exit-fullscreen, close");
  if (*parsed == WindowAction::kClose) {
    zebkit_view_request_close(view);
    return true;
  }
  Window* window = view->window();
  if (ViewWindowOwner(view) != WindowOwner::kEngine)
    return Fail(error, "not the engine's window: the view is in another window");
  std::string message;
  if (!window->PerformAction(*parsed, &message))
    return Fail(error, message);
  return true;
}

void zebkit_view_on_window_state(zebkit_view* view, zebkit_window_state_func func, void* data) {
  using namespace zebkit;
  if (!LiveView(view) || !view->window())
    return;
  view->on_window_state_ = func;
  view->on_window_state_data_ = data;
  view->window()->SetStateObserver(func ? base::BindRepeating(&TellState, view->GetWeakPtr())
                                        : base::RepeatingClosure());
}

bool zebkit_view_set_drag_regions(zebkit_view* view, const char* regions_json, char** error) {
  using namespace zebkit;
  if (error)
    *error = nullptr;
  if (!LiveView(view) || !view->window())
    return Fail(error, "a live view on the engine's thread");
  std::vector<DragRegion> regions;
  std::string message;
  if (!ParseDragRegions(regions_json ? regions_json : "[]", &regions, &message) ||
      !view->window()->SetHostDragRegions(std::move(regions), &message))
    return Fail(error, message);
  return true;
}

bool zebkit_view_set_popup_window_options(zebkit_view* view, const char* options_json, char** error) {
  using namespace zebkit;
  if (error)
    *error = nullptr;
  if (!LiveView(view) || !view->window())
    return Fail(error, "a live view on the engine's thread");
  if (!options_json) {
    view->window()->SetPopupOptions(std::nullopt);
    return true;
  }
  WindowOptions options;
  std::string message;
  if (!ParsePopupWindowOptions(options_json, &options, &message))
    return Fail(error, message);
  view->window()->SetPopupOptions(std::move(options));
  return true;
}

char* zebkit_get_capabilities(void) {
  using namespace zebkit;
  if (!OnEngineThread())
    return nullptr;
  base::DictValue value;
  value.Set("headless", zebkit_is_headless());
#if BUILDFLAG(IS_MAC)
  value.Set("platform", "macos");
#elif BUILDFLAG(IS_WIN)
  value.Set("platform", "windows");
#else
  value.Set("platform", "linux");
#endif
  value.Set("window", CapabilitiesValue(zebkit_is_headless() || Window::IsShutDown()
                                            ? WindowCapabilities()
                                            : PlatformFor(Window::Kind::kNative)->Capabilities(nullptr)));
  return strdup(base::WriteJson(value).value_or("{}").c_str());
}

}  // extern "C"
