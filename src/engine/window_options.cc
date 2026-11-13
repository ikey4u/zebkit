#include "zebkit/engine/c_api_util.h"
#include "zebkit/engine/window_options.h"

#include <cmath>
#include <limits>
#include <utility>

#include "base/json/json_reader.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/utf_string_conversions.h"
#include "url/gurl.h"

namespace zebkit {

namespace {

constexpr size_t kMaxJson = 64 * 1024;
constexpr size_t kMaxRegions = 1024;
constexpr size_t kMaxOrigins = 64;
constexpr int kMaxDimension = 1 << 15;

constexpr const char* kCreationFields[] = {"width", "height", "frame", "fallback", "show"};
constexpr const char* kRuntimeFields[] = {
    "minWidth", "minHeight", "resizable", "x", "y", "title", "controls", "background",
    "appearance", "pageFullscreen", "dragRegions",
};

bool IsCreationField(std::string_view key) {
  for (const char* field : kCreationFields)
    if (key == field)
      return true;
  return false;
}

bool IsRuntimeField(std::string_view key) {
  for (const char* field : kRuntimeFields)
    if (key == field)
      return true;
  return false;
}

bool Integer(const base::Value& value, int low, int high, int* out) {
  if (!value.is_int() && !value.is_double())
    return false;
  const double number = value.GetDouble();
  if (!std::isfinite(number) || number != std::floor(number) || number < low || number > high)
    return false;
  *out = static_cast<int>(number);
  return true;
}

bool Color(const base::Value& value, SkColor* out) {
  if (!value.is_string())
    return false;
  const std::string& text = value.GetString();
  if ((text.size() != 7 && text.size() != 9) || text[0] != '#')
    return false;
  uint32_t parsed = 0;
  if (!base::HexStringToUInt(std::string_view(text).substr(1), &parsed))
    return false;
  *out = text.size() == 7 ? SkColorSetRGB(parsed >> 16, (parsed >> 8) & 0xff, parsed & 0xff)
                          : SkColorSetARGB(parsed & 0xff, parsed >> 24, (parsed >> 16) & 0xff,
                                           (parsed >> 8) & 0xff);
  return true;
}

bool Field(const base::DictValue& dict, std::string_view key, std::string* error,
           const base::DictValue*& nested) {
  nested = nullptr;
  const base::Value* value = dict.Find(key);
  if (!value)
    return true;
  nested = value->GetIfDict();
  return nested ? true : SetError(error, std::string(key) + ": expected an object");
}

bool Apply(const base::DictValue& dict, bool popup, WindowOptions* options, std::string* error) {
  for (auto [key, value] : dict) {
    if (!IsCreationField(key) && !IsRuntimeField(key))
      return SetError(error, "unknown window option: " + key);
    if (popup && key == "dragRegions")
      return SetError(error, "dragRegions: a popup never has its opener's regions");
  }
  auto integer = [&](const char* key, int low, int high, int* out) {
    const base::Value* value = dict.Find(key);
    if (!value)
      return true;
    return Integer(*value, low, high, out) ||
           SetError(error, std::string(key) + ": expected an integer from " + base::NumberToString(low) + " to " +
                           base::NumberToString(high));
  };
  auto boolean = [&](const char* key, bool* out) {
    const base::Value* value = dict.Find(key);
    if (!value)
      return true;
    if (!value->is_bool())
      return SetError(error, std::string(key) + ": expected a boolean");
    *out = value->GetBool();
    return true;
  };
  auto choice = [&](const char* key, std::initializer_list<const char*> names, int* out) {
    const base::Value* value = dict.Find(key);
    if (!value)
      return true;
    if (value->is_string()) {
      int index = 0;
      for (const char* name : names) {
        if (value->GetString() == name) {
          *out = index;
          return true;
        }
        ++index;
      }
    }
    std::string expected;
    for (const char* name : names)
      expected += (expected.empty() ? "" : ", ") + std::string(name);
    return SetError(error, std::string(key) + ": expected one of " + expected);
  };

  int width = options->size.width(), height = options->size.height();
  if (!integer("width", 0, kMaxDimension, &width) || !integer("height", 0, kMaxDimension, &height))
    return false;
  options->size = gfx::Size(width, height);
  int frame = static_cast<int>(options->frame);
  if (!choice("frame", {"system", "overlay", "none"}, &frame))
    return false;
  options->frame = static_cast<WindowOptions::Frame>(frame);
  int fallback = options->fallback_to_system ? 1 : 0;
  if (!choice("fallback", {"error", "system"}, &fallback))
    return false;
  options->fallback_to_system = fallback == 1;
  if (!boolean("show", &options->show))
    return false;

  int min_width = options->minimum.width(), min_height = options->minimum.height();
  if (!integer("minWidth", 0, kMaxDimension, &min_width) || !integer("minHeight", 0, kMaxDimension, &min_height))
    return false;
  options->minimum = gfx::Size(min_width, min_height);
  if (!boolean("resizable", &options->resizable))
    return false;
  const bool has_x = dict.Find("x"), has_y = dict.Find("y");
  if (has_x != has_y)
    return SetError(error, "x and y: both or neither");
  if (has_x) {
    int x = 0, y = 0;
    if (!integer("x", -kMaxDimension, kMaxDimension, &x) || !integer("y", -kMaxDimension, kMaxDimension, &y))
      return false;
    options->position = gfx::Point(x, y);
  }
  if (const base::Value* title = dict.Find("title")) {
    if (title->is_none())
      options->title.reset();
    else if (!title->is_string() || title->GetString().size() > 4096)
      return SetError(error, "title: expected a string of at most 4096 bytes, or null for the page's");
    else
      options->title = base::UTF8ToUTF16(title->GetString());
  }
  const base::DictValue* controls = nullptr;
  if (!Field(dict, "controls", error, controls))
    return false;
  if (controls) {
    for (auto [key, value] : *controls)
      if (key != "height" && key != "color" && key != "symbolColor")
        return SetError(error, "controls: unknown key " + key);
    if (const base::Value* value = controls->Find("height");
        value && !Integer(*value, 0, 200, &options->controls_height))
      return SetError(error, "controls.height: expected an integer from 0 to 200");
    SkColor color;
    if (const base::Value* value = controls->Find("color")) {
      if (!Color(*value, &color))
        return SetError(error, "controls.color: expected #rrggbb or #rrggbbaa");
      options->controls_color = color;
    }
    if (const base::Value* value = controls->Find("symbolColor")) {
      if (!Color(*value, &color))
        return SetError(error, "controls.symbolColor: expected #rrggbb or #rrggbbaa");
      options->symbol_color = color;
    }
  }
  if (const base::Value* value = dict.Find("background")) {
    SkColor color;
    if (value->is_none())
      options->background.reset();
    else if (!Color(*value, &color))
      return SetError(error, "background: expected #rrggbb or #rrggbbaa, or null");
    else
      options->background = color;
  }
  int appearance = static_cast<int>(options->appearance);
  if (!choice("appearance", {"system", "light", "dark"}, &appearance))
    return false;
  options->appearance = static_cast<WindowOptions::Appearance>(appearance);
  int page_fullscreen = static_cast<int>(options->page_fullscreen);
  if (!choice("pageFullscreen", {"content", "window"}, &page_fullscreen))
    return false;
  options->page_fullscreen = static_cast<WindowOptions::PageFullscreen>(page_fullscreen);

  const base::DictValue* regions = nullptr;
  if (!Field(dict, "dragRegions", error, regions))
    return false;
  if (regions) {
    for (auto [key, value] : *regions)
      if (key != "source" && key != "origins")
        return SetError(error, "dragRegions: unknown key " + key);
    int source = static_cast<int>(options->drag_source);
    const base::Value* given = regions->Find("source");
    if (given) {
      if (!given->is_string())
        return SetError(error, "dragRegions.source: expected none, css or host");
      const std::string& name = given->GetString();
      if (name == "none") source = 0;
      else if (name == "css") source = 1;
      else if (name == "host") source = 2;
      else return SetError(error, "dragRegions.source: expected none, css or host");
    }
    std::vector<url::Origin> origins = options->drag_origins;
    if (const base::Value* list = regions->Find("origins")) {
      if (!list->is_list() || list->GetList().size() > kMaxOrigins)
        return SetError(error, "dragRegions.origins: expected a list of at most 64 origins");
      origins.clear();
      for (const base::Value& item : list->GetList()) {
        const GURL url(item.is_string() ? item.GetString() : std::string());
        url::Origin origin = url::Origin::Create(url);
        if (!url.is_valid() || origin.opaque() || origin.Serialize() != item.GetString())
          return SetError(error, "dragRegions.origins: expected serialized origins, such as https://app.example");
        origins.push_back(std::move(origin));
      }
    }
    if (source == 1 && origins.empty())
      return SetError(error, "dragRegions: css needs the origins whose pages may set regions");
    options->drag_source = static_cast<WindowOptions::DragSource>(source);
    options->drag_origins = std::move(origins);
  }
  return true;
}

}  // namespace

WindowOptions::WindowOptions() = default;
WindowOptions::WindowOptions(WindowOptions&&) = default;
WindowOptions& WindowOptions::operator=(WindowOptions&&) = default;
WindowOptions::~WindowOptions() = default;
WindowOptions::WindowOptions(const WindowOptions& other) { *this = other; }
WindowOptions& WindowOptions::operator=(const WindowOptions& other) {
  if (this == &other)
    return *this;
  size = other.size;
  frame = other.frame;
  fallback_to_system = other.fallback_to_system;
  show = other.show;
  minimum = other.minimum;
  resizable = other.resizable;
  position = other.position;
  title = other.title;
  controls_height = other.controls_height;
  controls_color = other.controls_color;
  symbol_color = other.symbol_color;
  background = other.background;
  appearance = other.appearance;
  page_fullscreen = other.page_fullscreen;
  drag_source = other.drag_source;
  drag_origins = other.drag_origins;
  given = other.given.Clone();
  return *this;
}

bool ParseWindowOptions(std::string_view json, bool at_creation, const WindowOptions& base,
                        WindowOptions* out, std::string* error) {
  if (json.size() > kMaxJson)
    return SetError(error, "window options: at most 64 KiB of JSON");
  std::optional<base::Value> parsed = base::JSONReader::Read(json, base::JSON_PARSE_RFC);
  if (!parsed || !parsed->is_dict())
    return SetError(error, "window options: expected a JSON object");
  const base::DictValue& dict = parsed->GetDict();
  if (!at_creation) {
    for (auto [key, value] : dict)
      if (IsCreationField(key))
        return SetError(error, "requires-recreate: " + key + " is fixed when the window is made");
  }
  WindowOptions options = base;
  if (!Apply(dict, false, &options, error))
    return false;
  for (auto [key, value] : dict)
    options.given.Set(key, value.Clone());
  *out = std::move(options);
  return true;
}

bool ParsePopupWindowOptions(std::string_view json, WindowOptions* out, std::string* error) {
  if (json.size() > kMaxJson)
    return SetError(error, "window options: at most 64 KiB of JSON");
  std::optional<base::Value> parsed = base::JSONReader::Read(json, base::JSON_PARSE_RFC);
  if (!parsed || !parsed->is_dict())
    return SetError(error, "window options: expected a JSON object");
  WindowOptions options;
  if (!Apply(parsed->GetDict(), true, &options, error))
    return false;
  options.given = parsed->GetDict().Clone();
  *out = std::move(options);
  return true;
}

std::optional<WindowAction> WindowActionNamed(std::string_view name) {
  using A = WindowAction;
  for (A action : {A::kShow, A::kHide, A::kMinimize, A::kMaximize, A::kRestore, A::kFullscreen,
                   A::kExitFullscreen, A::kClose})
    if (name == WindowActionName(action))
      return action;
  return std::nullopt;
}

const char* WindowActionName(WindowAction action) {
  switch (action) {
    case WindowAction::kShow: return "show";
    case WindowAction::kHide: return "hide";
    case WindowAction::kMinimize: return "minimize";
    case WindowAction::kMaximize: return "maximize";
    case WindowAction::kRestore: return "restore";
    case WindowAction::kFullscreen: return "fullscreen";
    case WindowAction::kExitFullscreen: return "exit-fullscreen";
    case WindowAction::kClose: return "close";
  }
  return "";
}

bool ParseDragRegions(std::string_view json, std::vector<DragRegion>* out, std::string* error) {
  if (json.size() > kMaxJson)
    return SetError(error, "drag regions: at most 64 KiB of JSON");
  std::optional<base::Value> parsed = base::JSONReader::Read(json, base::JSON_PARSE_RFC);
  if (!parsed || !parsed->is_list() || parsed->GetList().size() > kMaxRegions)
    return SetError(error, "drag regions: expected a JSON array of at most 1024 rectangles");
  std::vector<DragRegion> regions;
  for (const base::Value& item : parsed->GetList()) {
    const base::DictValue* rect = item.GetIfDict();
    int x = 0, y = 0, width = 0, height = 0;
    if (!rect)
      return SetError(error, "drag regions: each is {x, y, width, height, drag}");
    for (auto [key, value] : *rect)
      if (key != "x" && key != "y" && key != "width" && key != "height" && key != "drag")
        return SetError(error, "drag regions: unknown key " + key);
    const base::Value *vx = rect->Find("x"), *vy = rect->Find("y"), *vw = rect->Find("width"),
                      *vh = rect->Find("height");
    if (!vx || !vy || !vw || !vh || !Integer(*vx, -kMaxDimension, kMaxDimension, &x) ||
        !Integer(*vy, -kMaxDimension, kMaxDimension, &y) || !Integer(*vw, 0, kMaxDimension, &width) ||
        !Integer(*vh, 0, kMaxDimension, &height))
      return SetError(error, "drag regions: x, y, width and height are integers, width and height not negative");
    DragRegion region{gfx::Rect(x, y, width, height), true};
    if (const base::Value* drag = rect->Find("drag")) {
      if (!drag->is_bool())
        return SetError(error, "drag regions: drag is a boolean");
      region.drag = drag->GetBool();
    }
    regions.push_back(region);
  }
  *out = std::move(regions);
  return true;
}

bool InDragRegion(const std::vector<DragRegion>& regions, const gfx::Point& point) {
  for (auto it = regions.rbegin(); it != regions.rend(); ++it)
    if (it->bounds.Contains(point))
      return it->drag;
  return false;
}

const char* StateName(WindowGeometry::State state) {
  switch (state) {
    case WindowGeometry::State::kNormal: return "normal";
    case WindowGeometry::State::kMinimized: return "minimized";
    case WindowGeometry::State::kMaximized: return "maximized";
    case WindowGeometry::State::kFullscreen: return "fullscreen";
  }
  return "normal";
}

const char* FrameName(WindowOptions::Frame frame) {
  switch (frame) {
    case WindowOptions::Frame::kSystem: return "system";
    case WindowOptions::Frame::kOverlay: return "overlay";
    case WindowOptions::Frame::kNone: return "none";
  }
  return "system";
}

}  // namespace zebkit
