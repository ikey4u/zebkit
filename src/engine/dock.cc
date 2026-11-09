#include "zebkit/engine/dock.h"

#include <string.h>

#include <cmath>
#include <string>
#include <utility>

#include "base/no_destructor.h"
#include "build/build_config.h"
#include "content/public/browser/browser_thread.h"
#include "zebkit/engine/include/zebkit/zebkit.h"

namespace zebkit {

namespace {

DockState& State() {
  static base::NoDestructor<DockState> state;
  return *state;
}

struct Menu {
  std::vector<MenuItem> items;
  zebkit_menu_func func = nullptr;
  void* data = nullptr;
};

Menu& TheMenu() {
  static base::NoDestructor<Menu> menu;
  return *menu;
}

bool OnEngineThread() {
  return content::BrowserThread::CurrentlyOn(content::BrowserThread::UI);
}

}  // namespace

const std::vector<MenuItem>& DockMenu() {
  return TheMenu().items;
}

void RunDockMenuItem(const std::string& id) {
  if (TheMenu().func && !id.empty())
    TheMenu().func(id.c_str(), TheMenu().data);
}

#if !BUILDFLAG(IS_MAC) && !BUILDFLAG(IS_WIN) && !(BUILDFLAG(IS_LINUX) && defined(ZEBKIT_COOKIES_USE_DBUS))
// No dock to tell here.
void ApplyDockState(const DockState& state) {}
void RequestDockAttention(bool critical) {}
bool DockMenuSupported() {
  return false;
}
#endif

}  // namespace zebkit

extern "C" {

bool zebkit_app_set_badge(const char* text) {
  if (!zebkit::OnEngineThread() || (text && strlen(text) > 64))
    return false;
  zebkit::State().badge = text ? text : "";
  zebkit::ApplyDockState(zebkit::State());
  return true;
}

bool zebkit_app_set_progress(double progress) {
  if (!zebkit::OnEngineThread() || std::isnan(progress) || progress > 1)
    return false;
  zebkit::State().progress = progress;
  zebkit::ApplyDockState(zebkit::State());
  return true;
}

bool zebkit_app_set_dock_menu(const char* json, zebkit_menu_func func, void* data, char** error) {
  auto fail = [&](const std::string& message) {
    if (error)
      *error = strdup(message.c_str());
    return false;
  };
  if (error)
    *error = nullptr;
  if (!zebkit::OnEngineThread())
    return fail("the engine's thread");
  if (!zebkit::DockMenuSupported())
    return fail("unsupported: the Dock's menu is macOS's");
  std::string problem;
  auto items = zebkit::ParseMenuItems(json ? json : "[]", &problem);
  if (!items)
    return fail(problem);
  zebkit::TheMenu() = {std::move(*items), func, data};
  return true;
}

void zebkit_app_request_attention(bool critical) {
  if (zebkit::OnEngineThread())
    zebkit::RequestDockAttention(critical);
}

}  // extern "C"
