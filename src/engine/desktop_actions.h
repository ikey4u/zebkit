// What the application asks the system to open (docs/SYSTEM_INTEGRATION.md,
// 13): a URL with the system's handler for it, a file shown in the file
// manager. Never through a shell: the URL or path is one argument.

#ifndef ZEBKIT_ENGINE_DESKTOP_ACTIONS_H_
#define ZEBKIT_ENGINE_DESKTOP_ACTIONS_H_

#include <string>

#include "base/files/file_path.h"
#include "url/gurl.h"

namespace zebkit {

// desktop_actions_linux.cc, desktop_actions_mac.mm, desktop_actions_win.cc.
bool OpenWithSystem(const GURL& url, std::string* error);
bool ShowInFileManager(const base::FilePath& path, std::string* error);

}  // namespace zebkit

#endif  // ZEBKIT_ENGINE_DESKTOP_ACTIONS_H_
