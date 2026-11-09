// The system's handlers (desktop_actions.h), on Windows: ShellExecute, and
// the Explorer's selection.

#include <windows.h>

#include <shellapi.h>
#include <shlobj.h>

#include "base/strings/utf_string_conversions.h"
#include "zebkit/engine/desktop_actions.h"

namespace zebkit {

bool OpenWithSystem(const GURL& url, std::string* error) {
  const std::wstring target = base::UTF8ToWide(url.spec());
  const auto result = reinterpret_cast<intptr_t>(
      ShellExecuteW(nullptr, L"open", target.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
  if (result <= 32) {
    *error = "no program opens this URL";
    return false;
  }
  return true;
}

bool ShowInFileManager(const base::FilePath& path, std::string* error) {
  PIDLIST_ABSOLUTE item = ILCreateFromPathW(path.value().c_str());
  if (!item) {
    *error = "no such file";
    return false;
  }
  const HRESULT result = SHOpenFolderAndSelectItems(item, 0, nullptr, 0);
  ILFree(item);
  if (FAILED(result)) {
    *error = "the Explorer did not show it";
    return false;
  }
  return true;
}

}  // namespace zebkit
