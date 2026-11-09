// The application as the system sees it: its menu, the files it is asked
// to open, the Dock's reopen and quit (zebkit_app_*, app.cc). The menu is
// the menu bar on macOS (engine_mac.mm); elsewhere, where windows have no
// menu bar, its keys work in the application's windows.

#ifndef ZEBKIT_ENGINE_APP_H_
#define ZEBKIT_ENGINE_APP_H_

#include <optional>
#include <string>
#include <vector>

namespace input {
struct NativeWebKeyboardEvent;
}

namespace zebkit {

// Keys besides the command key (⌘ on macOS, Ctrl elsewhere).
enum MenuModifier {
  kMenuShift = 1 << 0,
  kMenuAlt = 1 << 1,
  kMenuCtrl = 1 << 2,  // Control on macOS, where the command key is ⌘
};

struct MenuItem {
  std::string title;
  std::string id;    // what zebkit_app_set_menu()'s function gets
  std::string role;  // a standard command: "undo", "copy", "minimize", ...
  std::string key;   // "o", "=", "f5"; with the command key
  int modifiers = 0;
  bool separator = false;
  // State that changes without a new menu (zebkit_app_update_menu_item()).
  bool enabled = true;
  bool checked = false;
  std::string radio_group;  // one checked of the items in a group
  std::vector<MenuItem> items;  // a submenu
};

// Whether the item @id is enabled; true for an id of no item.
bool MenuItemEnabled(const std::string& id);

// Menu items as zebkit_app_set_menu() takes them, for the tray's menu too.
std::optional<std::vector<MenuItem>> ParseMenuItems(const std::string& json, std::string* problem);

// The menu zebkit_app_set_menu() was given; empty before.
const std::vector<MenuItem>& AppMenu();

// Tells the application that menu item @id was chosen.
void RunMenuItem(const std::string& id);

// The id of the item of the menu whose key @event is; "" for none.
std::string MenuItemForKey(const input::NativeWebKeyboardEvent& event);

// What the system asks of the application: files to open (kept until the
// application listens), its icon clicked with no window shown, and to
// quit (zebkit quits unless the application listens).
void OpenFiles(const std::vector<std::string>& paths);
void Reopen();
void QuitRequested();

#if defined(__APPLE__)
// The menu bar from AppMenu(), after the application menu (engine_mac.mm).
void SetUpMenuBar();
#endif

}  // namespace zebkit

#endif  // ZEBKIT_ENGINE_APP_H_
