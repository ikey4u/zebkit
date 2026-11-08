// zebkit_app_*: the application's menu, files opened through the system,
// the Dock's reopen, and quit (app.h).

#include "zebkit/engine/app.h"

#include <stdlib.h>

#include <algorithm>
#include <functional>
#include <iterator>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "base/check.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/no_destructor.h"
#include "base/strings/string_util.h"
#include "base/values.h"
#include "build/build_config.h"
#include "components/input/native_web_keyboard_event.h"
#include "content/public/browser/browser_thread.h"
#include "third_party/blink/public/common/input/web_input_event.h"
#include "ui/events/keycodes/keyboard_codes.h"
#include "zebkit/engine/include/zebkit/zebkit.h"

namespace zebkit {

namespace {

struct App {
  std::vector<MenuItem> menu;
  zebkit_menu_func on_menu = nullptr;
  void* on_menu_data = nullptr;
  zebkit_paths_func on_open = nullptr;
  void* on_open_data = nullptr;
  // Files asked for before the application listened.
  std::vector<std::string> pending;
  zebkit_app_func on_reopen = nullptr;
  void* on_reopen_data = nullptr;
  zebkit_app_func on_quit = nullptr;
  void* on_quit_data = nullptr;
};

App& TheApp() {
  static base::NoDestructor<App> app;
  return *app;
}

constexpr const char* kRoles[] = {
    "undo",   "redo",     "cut",        "copy",       "paste",
    "selectAll", "minimize", "zoom",     "close",      "fullscreen",
    "front",  "quit",     "hide",       "hideOthers", "showAll",
    "about",  "services", "appMenu", "pasteAndMatchStyle",
};

std::optional<std::vector<MenuItem>> ParseItems(const base::ListValue& list,
                                                std::string& problem) {
  std::vector<MenuItem> items;
  for (const base::Value& value : list) {
    if (!value.is_dict()) {
      problem = "a menu item is not an object";
      return std::nullopt;
    }
    const base::DictValue& dict = value.GetDict();
    MenuItem item;
    item.separator = dict.FindBool("separator").value_or(false);
    if (const std::string* title = dict.FindString("title"))
      item.title = *title;
    if (const std::string* id = dict.FindString("id"))
      item.id = *id;
    if (const std::string* role = dict.FindString("role")) {
      if (std::ranges::find(kRoles, *role) == std::end(kRoles)) {
        problem = "no menu role " + *role;
        return std::nullopt;
      }
      item.role = *role;
    }
    if (const std::string* key = dict.FindString("key"))
      item.key = *key;
    item.enabled = dict.FindBool("enabled").value_or(true);
    item.checked = dict.FindBool("checked").value_or(false);
    if (const std::string* group = dict.FindString("radioGroup"))
      item.radio_group = *group;
    if (const base::ListValue* modifiers = dict.FindList("modifiers")) {
      for (const base::Value& modifier : *modifiers) {
        const std::string* name = modifier.GetIfString();
        if (name && *name == "shift") {
          item.modifiers |= kMenuShift;
        } else if (name && *name == "alt") {
          item.modifiers |= kMenuAlt;
        } else if (name && *name == "ctrl") {
          item.modifiers |= kMenuCtrl;
        } else {
          problem = "modifiers are \"shift\", \"alt\" and \"ctrl\"";
          return std::nullopt;
        }
      }
    }
    if (const base::ListValue* children = dict.FindList("items")) {
      std::optional<std::vector<MenuItem>> submenu =
          ParseItems(*children, problem);
      if (!submenu)
        return std::nullopt;
      item.items = std::move(*submenu);
    }
    if (!item.separator && item.title.empty() && item.role.empty()) {
      problem = "a menu item needs a title";
      return std::nullopt;
    }
    items.push_back(std::move(item));
  }
  return items;
}

// The key code of a menu item's key: a character, or "f1" to "f24".
ui::KeyboardCode KeyCode(const std::string& key) {
  const std::string k = base::ToLowerASCII(key);
  if (k.size() == 1) {
    const char c = k[0];
    if (c >= 'a' && c <= 'z')
      return static_cast<ui::KeyboardCode>(ui::VKEY_A + (c - 'a'));
    if (c >= '0' && c <= '9')
      return static_cast<ui::KeyboardCode>(ui::VKEY_0 + (c - '0'));
    switch (c) {
      case '=':
      case '+':
        return ui::VKEY_OEM_PLUS;
      case '-':
        return ui::VKEY_OEM_MINUS;
      case ',':
        return ui::VKEY_OEM_COMMA;
      case '.':
        return ui::VKEY_OEM_PERIOD;
      case '/':
        return ui::VKEY_OEM_2;
      case ';':
        return ui::VKEY_OEM_1;
      case '[':
        return ui::VKEY_OEM_4;
      case ']':
        return ui::VKEY_OEM_6;
    }
  }
  if (k.size() >= 2 && k[0] == 'f') {
    const int n = atoi(k.c_str() + 1);
    if (n >= 1 && n <= 24)
      return static_cast<ui::KeyboardCode>(ui::VKEY_F1 + n - 1);
  }
  return ui::VKEY_UNKNOWN;
}

std::string Find(const std::vector<MenuItem>& items,
                 ui::KeyboardCode code,
                 int modifiers) {
  for (const MenuItem& item : items) {
    // A disabled item's key is not the menu's: the page gets it.
    if (!item.id.empty() && !item.key.empty() && item.enabled && KeyCode(item.key) == code &&
        item.modifiers == modifiers) {
      return item.id;
    }
    if (std::string found = Find(item.items, code, modifiers); !found.empty())
      return found;
  }
  return std::string();
}

// Every item with @id, wherever it is.
void ForEachWithId(std::vector<MenuItem>& items, const std::string& id,
                   const std::function<void(MenuItem&)>& function) {
  for (MenuItem& item : items) {
    if (item.id == id)
      function(item);
    ForEachWithId(item.items, id, function);
  }
}

void UncheckGroup(std::vector<MenuItem>& items, const std::string& group, const std::string& except) {
  for (MenuItem& item : items) {
    if (item.radio_group == group && item.id != except)
      item.checked = false;
    UncheckGroup(item.items, group, except);
  }
}

base::ListValue ItemsValue(const std::vector<MenuItem>& items) {
  base::ListValue list;
  for (const MenuItem& item : items) {
    base::DictValue value;
    if (item.separator) {
      value.Set("separator", true);
      list.Append(std::move(value));
      continue;
    }
    if (!item.title.empty()) value.Set("title", item.title);
    if (!item.id.empty()) value.Set("id", item.id);
    if (!item.role.empty()) value.Set("role", item.role);
    if (!item.key.empty()) value.Set("key", item.key);
    value.Set("enabled", item.enabled);
    value.Set("checked", item.checked);
    if (!item.radio_group.empty()) value.Set("radioGroup", item.radio_group);
    if (!item.items.empty()) value.Set("items", ItemsValue(item.items));
    list.Append(std::move(value));
  }
  return list;
}

bool AnyEnabled(const std::vector<MenuItem>& items, const std::string& id, bool* found) {
  for (const MenuItem& item : items) {
    if (item.id == id) {
      *found = true;
      if (item.enabled)
        return true;
    }
    if (AnyEnabled(item.items, id, found))
      return true;
  }
  return false;
}

}  // namespace

const std::vector<MenuItem>& AppMenu() {
  return TheApp().menu;
}

bool MenuItemEnabled(const std::string& id) {
  bool found = false;
  const bool enabled = AnyEnabled(TheApp().menu, id, &found);
  return enabled || !found;
}

void RunMenuItem(const std::string& id) {
  App& app = TheApp();
  if (app.on_menu && !id.empty())
    app.on_menu(id.c_str(), app.on_menu_data);
}

std::string MenuItemForKey(const input::NativeWebKeyboardEvent& event) {
#if BUILDFLAG(IS_MAC)
  // The menu bar takes its keys itself.
  return std::string();
#else
  if (event.GetType() != blink::WebInputEvent::Type::kRawKeyDown)
    return std::string();
  const int state = event.GetModifiers();
  // The command key is Ctrl here; Ctrl as a further modifier is macOS's.
  if (!(state & blink::WebInputEvent::kControlKey) ||
      (state & blink::WebInputEvent::kMetaKey)) {
    return std::string();
  }
  int modifiers = 0;
  if (state & blink::WebInputEvent::kShiftKey)
    modifiers |= kMenuShift;
  if (state & blink::WebInputEvent::kAltKey)
    modifiers |= kMenuAlt;
  return Find(TheApp().menu,
              static_cast<ui::KeyboardCode>(event.windows_key_code),
              modifiers);
#endif
}

void OpenFiles(const std::vector<std::string>& paths) {
  App& app = TheApp();
  if (!app.on_open) {
    app.pending.insert(app.pending.end(), paths.begin(), paths.end());
    return;
  }
  base::ListValue list;
  for (const std::string& path : paths)
    list.Append(path);
  const std::string json = base::WriteJson(list).value_or("[]");
  app.on_open(json.c_str(), app.on_open_data);
}

void Reopen() {
  App& app = TheApp();
  if (app.on_reopen)
    app.on_reopen(app.on_reopen_data);
}

void QuitRequested() {
  App& app = TheApp();
  if (app.on_quit)
    app.on_quit(app.on_quit_data);
  else
    zebkit_quit();
}

}  // namespace zebkit

extern "C" {

bool zebkit_app_set_menu(const char* json,
                         zebkit_menu_func func,
                         void* data,
                         char** error) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  std::optional<base::Value> parsed = base::JSONReader::Read(
      json ? json : "", base::JSON_PARSE_CHROMIUM_EXTENSIONS);
  std::string problem = "the menu is not a JSON array";
  std::optional<std::vector<zebkit::MenuItem>> menu;
  if (parsed && parsed->is_list())
    menu = zebkit::ParseItems(parsed->GetList(), problem);
  if (!menu) {
    if (error)
      *error = strdup(problem.c_str());
    return false;
  }
  zebkit::App& app = zebkit::TheApp();
  app.menu = std::move(*menu);
  app.on_menu = func;
  app.on_menu_data = data;
#if BUILDFLAG(IS_MAC)
  zebkit::SetUpMenuBar();
#endif
  return true;
}

bool zebkit_app_update_menu_item(const char* id, const char* json, char** error) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  auto fail = [&](const char* message) {
    if (error)
      *error = strdup(message);
    return false;
  };
  if (error)
    *error = nullptr;
  if (!id || !*id)
    return fail("a menu item's id");
  std::optional<base::Value> parsed = base::JSONReader::Read(json ? json : "", base::JSON_PARSE_RFC);
  if (!parsed || !parsed->is_dict())
    return fail("the item's state is not a JSON object");
  const base::DictValue& state = parsed->GetDict();
  for (auto [key, value] : state) {
    if (key != "enabled" && key != "checked" && key != "title")
      return fail("an item's enabled, checked and title change");
    if ((key == "title" && !value.is_string()) || (key != "title" && !value.is_bool()))
      return fail("enabled and checked are booleans, title a string");
  }
  zebkit::App& app = zebkit::TheApp();
  bool found = false;
  std::string group;
  zebkit::ForEachWithId(app.menu, id, [&](zebkit::MenuItem& item) {
    found = true;
    if (auto enabled = state.FindBool("enabled")) item.enabled = *enabled;
    if (auto checked = state.FindBool("checked")) {
      item.checked = *checked;
      if (*checked && !item.radio_group.empty()) group = item.radio_group;
    }
    if (const std::string* title = state.FindString("title")) item.title = *title;
  });
  if (!found)
    return fail("no menu item has that id");
  if (!group.empty())
    zebkit::UncheckGroup(app.menu, group, id);
#if BUILDFLAG(IS_MAC)
  zebkit::SetUpMenuBar();
#endif
  return true;
}

char* zebkit_app_get_menu(void) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  return strdup(base::WriteJson(zebkit::ItemsValue(zebkit::TheApp().menu)).value_or("[]").c_str());
}

void zebkit_app_on_open_files(zebkit_paths_func func, void* data) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  zebkit::App& app = zebkit::TheApp();
  app.on_open = func;
  app.on_open_data = data;
  if (func && !app.pending.empty())
    zebkit::OpenFiles(std::exchange(app.pending, {}));
}

void zebkit_app_on_reopen(zebkit_app_func func, void* data) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  zebkit::TheApp().on_reopen = func;
  zebkit::TheApp().on_reopen_data = data;
}

void zebkit_app_on_quit_request(zebkit_app_func func, void* data) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  zebkit::TheApp().on_quit = func;
  zebkit::TheApp().on_quit_data = data;
}

}  // extern "C"
