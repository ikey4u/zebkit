#include "zebkit/engine/desktop_actions.h"

#include <string.h>

#include "base/files/file_util.h"
#include "content/public/browser/browser_thread.h"
#include "zebkit/engine/include/zebkit/zebkit.h"

extern "C" {

bool zebkit_open_external(const char* url, char** error) {
  auto fail = [&](const std::string& message) {
    if (error)
      *error = strdup(message.c_str());
    return false;
  };
  if (error)
    *error = nullptr;
  if (!content::BrowserThread::CurrentlyOn(content::BrowserThread::UI))
    return fail("the engine's thread");
  const GURL parsed(url ? url : "");
  // What a page could run or carry inline is not a program's to open.
  if (!parsed.is_valid() || parsed.SchemeIs("javascript") || parsed.SchemeIs("data") || parsed.SchemeIs("blob") ||
      parsed.SchemeIs("about") || parsed.spec().size() > 65536)
    return fail("a valid URL to open, not javascript:, data:, blob: or about:");
  std::string message;
  if (!zebkit::OpenWithSystem(parsed, &message))
    return fail(message);
  return true;
}

bool zebkit_show_in_folder(const char* path, char** error) {
  auto fail = [&](const std::string& message) {
    if (error)
      *error = strdup(message.c_str());
    return false;
  };
  if (error)
    *error = nullptr;
  if (!content::BrowserThread::CurrentlyOn(content::BrowserThread::UI))
    return fail("the engine's thread");
  const base::FilePath file = base::FilePath::FromUTF8Unsafe(path ? path : "");
  if (!file.IsAbsolute() || file.ReferencesParent())
    return fail("an absolute path");
  std::string message;
  if (!zebkit::ShowInFileManager(file, &message))
    return fail(message);
  return true;
}

}  // extern "C"
