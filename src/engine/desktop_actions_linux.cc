// The desktop's handlers (desktop_actions.h), on Linux: xdg-open, and the
// file manager's org.freedesktop.FileManager1 for showing a file.

#include <string>
#include <utility>
#include <vector>

#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/process/kill.h"
#include "base/process/launch.h"
#include "base/task/thread_pool.h"
#include "net/base/filename_util.h"
#include "zebkit/engine/desktop_actions.h"

#if defined(ZEBKIT_USE_DBUS)
#include "components/dbus/thread_linux/dbus_thread_linux.h"
#include "components/dbus/utils/call_method.h"
#include "dbus/bus.h"
#include "dbus/object_proxy.h"
#endif

namespace zebkit {

namespace {

// Off the engine's thread: a fork and an exec.
std::string Launch(std::vector<std::string> argv) {
  base::LaunchOptions options;
  // Its own process group, not the engine's: it outlives it, as a desktop's
  // program does.
  options.new_process_group = true;
  base::Process process = base::LaunchProcess(argv, options);
  if (!process.IsValid())
    return "could not run " + argv[0];
  // Waited for when it ends, so that it leaves no zombie behind.
  base::EnsureProcessGetsReaped(std::move(process));
  return std::string();
}

void LaunchSoon(std::vector<std::string> argv, DesktopDone done) {
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE}, base::BindOnce(&Launch, std::move(argv)),
      base::BindOnce([](DesktopDone done, std::string error) { std::move(done).Run(error.empty(), error); },
                     std::move(done)));
}

}  // namespace

void OpenWithSystem(const GURL& url, DesktopDone done) {
  LaunchSoon({"xdg-open", url.spec()}, std::move(done));
}

void ShowInFileManager(const base::FilePath& path, DesktopDone done) {
#if defined(ZEBKIT_USE_DBUS)
  // The file selected in its folder, where the file manager has the interface;
  // its folder opened where it has not.
  scoped_refptr<dbus::Bus> bus = dbus_thread_linux::GetSharedSessionBus();
  dbus::ObjectProxy* proxy =
      bus->GetObjectProxy("org.freedesktop.FileManager1", dbus::ObjectPath("/org/freedesktop/FileManager1"));
  dbus_utils::CallMethod<"ass", "">(
      proxy, "org.freedesktop.FileManager1", "ShowItems",
      base::BindOnce(
          [](std::string folder, DesktopDone done, dbus_utils::CallMethodResult<> result) {
            if (result.has_value()) {
              std::move(done).Run(true, std::string());
              return;
            }
            LaunchSoon({"xdg-open", folder}, std::move(done));
          },
          path.DirName().value(), std::move(done)),
      std::vector<std::string>{net::FilePathToFileURL(path).spec()}, std::string());
#else
  LaunchSoon({"xdg-open", path.DirName().value()}, std::move(done));
#endif
}

}  // namespace zebkit
