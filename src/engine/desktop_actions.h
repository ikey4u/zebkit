// What the application asks the system to open (docs/SYSTEM_INTEGRATION.md,
// 13): a URL with the system's handler for it, a file shown in the file
// manager. Never through a shell: the URL or path is one argument.

#ifndef ZEBKIT_ENGINE_DESKTOP_ACTIONS_H_
#define ZEBKIT_ENGINE_DESKTOP_ACTIONS_H_

#include <string>

#include "base/files/file_path.h"
#include "base/functional/callback.h"
#include "url/gurl.h"

namespace zebkit {

// What the system did: done, or the error. Run on the engine's thread, after
// the call that asked returns; what may block (a launch, the shell's calls
// on Windows) runs off it.
using DesktopDone = base::OnceCallback<void(bool done, std::string error)>;

// desktop_actions_linux.cc, desktop_actions_mac.mm, desktop_actions_win.cc.
// ShowInFileManager() is given a path that exists.
void OpenWithSystem(const GURL& url, DesktopDone done);
void ShowInFileManager(const base::FilePath& path, DesktopDone done);

}  // namespace zebkit

#endif  // ZEBKIT_ENGINE_DESKTOP_ACTIONS_H_
