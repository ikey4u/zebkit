// The launcher (dock.h) on Linux: com.canonical.Unity.LauncherEntry's Update
// signal, which docks and panels that know it (Plank, Dash to Dock, KDE's
// task manager) show on the application's icon; others ignore it. The
// application is its desktop file, named after the program, as its windows'
// class is (window_views.cc).

#include <string>

#include "base/command_line.h"
#include "base/files/file_path.h"
#include "base/no_destructor.h"
#include "base/strings/string_number_conversions.h"
#include "zebkit/engine/dock.h"

#if defined(ZEBKIT_COOKIES_USE_DBUS)
#include "components/dbus/thread_linux/dbus_thread_linux.h"
#include "dbus/bus.h"
#include "dbus/exported_object.h"
#include "dbus/message.h"
#include "dbus/object_path.h"
#endif

namespace zebkit {

namespace {

bool g_urgent = false;

#if defined(ZEBKIT_COOKIES_USE_DBUS)
void Send(const DockState& state) {
  scoped_refptr<dbus::Bus> bus = dbus_thread_linux::GetSharedSessionBus();
  static dbus::ExportedObject* object =
      bus->GetExportedObject(dbus::ObjectPath("/com/canonical/unity/launcherentry/zebkit"));
  const std::string application =
      "application://" + base::CommandLine::ForCurrentProcess()->GetProgram().BaseName().value() + ".desktop";
  dbus::Signal signal("com.canonical.Unity.LauncherEntry", "Update");
  dbus::MessageWriter writer(&signal);
  writer.AppendString(application);
  dbus::MessageWriter properties(nullptr);
  writer.OpenArray("{sv}", &properties);
  auto entry = [&](const char* key, auto write) {
    dbus::MessageWriter pair(nullptr);
    properties.OpenDictEntry(&pair);
    pair.AppendString(key);
    write(pair);
    properties.CloseContainer(&pair);
  };
  int64_t count = 0;
  const bool counted = base::StringToInt64(state.badge, &count);
  entry("count", [&](dbus::MessageWriter& w) { w.AppendVariantOfInt64(counted ? count : 0); });
  entry("count-visible", [&](dbus::MessageWriter& w) { w.AppendVariantOfBool(counted); });
  entry("progress", [&](dbus::MessageWriter& w) { w.AppendVariantOfDouble(state.progress < 0 ? 0 : state.progress); });
  entry("progress-visible", [&](dbus::MessageWriter& w) { w.AppendVariantOfBool(state.progress >= 0); });
  entry("urgent", [&](dbus::MessageWriter& w) { w.AppendVariantOfBool(g_urgent); });
  writer.CloseContainer(&properties);
  object->SendSignal(&signal);
}
#endif

DockState& Last() {
  static base::NoDestructor<DockState> state;
  return *state;
}

}  // namespace

void ApplyDockState(const DockState& state) {
  Last() = state;
#if defined(ZEBKIT_COOKIES_USE_DBUS)
  Send(state);
#endif
}

void RequestDockAttention(bool critical) {
  g_urgent = true;
#if defined(ZEBKIT_COOKIES_USE_DBUS)
  Send(Last());
#endif
  g_urgent = false;
}

bool DockMenuSupported() {
  return false;
}

}  // namespace zebkit
