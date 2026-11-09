// The desktop's handlers (desktop_actions.h), on Linux: xdg-open, and the
// file manager's org.freedesktop.FileManager1 for showing a file.

#include <string>
#include <vector>

#include "base/files/file_util.h"
#include "base/process/launch.h"
#include "net/base/filename_util.h"
#include "zebkit/engine/desktop_actions.h"

#if defined(ZEBKIT_COOKIES_USE_DBUS)
#include "components/dbus/thread_linux/dbus_thread_linux.h"
#include "components/dbus/utils/call_method.h"
#include "dbus/bus.h"
#include "dbus/object_proxy.h"
#endif

namespace zebkit {

namespace {

bool Launch(std::vector<std::string> argv, std::string* error) {
  base::LaunchOptions options;
  // Its own process group, not the engine's: it outlives it, as a desktop's
  // program does.
  options.new_process_group = true;
  if (!base::LaunchProcess(argv, options).IsValid()) {
    *error = "could not run " + argv[0];
    return false;
  }
  return true;
}

}  // namespace

bool OpenWithSystem(const GURL& url, std::string* error) {
  return Launch({"xdg-open", url.spec()}, error);
}

bool ShowInFileManager(const base::FilePath& path, std::string* error) {
#if defined(ZEBKIT_COOKIES_USE_DBUS)
  // The file selected in its folder, where the file manager has the interface.
  scoped_refptr<dbus::Bus> bus = dbus_thread_linux::GetSharedSessionBus();
  dbus::ObjectProxy* proxy =
      bus->GetObjectProxy("org.freedesktop.FileManager1", dbus::ObjectPath("/org/freedesktop/FileManager1"));
  const std::string folder = path.DirName().value();
  dbus_utils::CallMethod<"ass", "">(
      proxy, "org.freedesktop.FileManager1", "ShowItems",
      base::BindOnce(
          [](std::string folder, dbus_utils::CallMethodResult<> result) {
            std::string ignored;
            if (!result.has_value())
              Launch({"xdg-open", folder}, &ignored);
          },
          folder),
      std::vector<std::string>{net::FilePathToFileURL(path).spec()}, std::string());
  return true;
#else
  return Launch({"xdg-open", path.DirName().value()}, error);
#endif
}

}  // namespace zebkit
