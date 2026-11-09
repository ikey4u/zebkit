// The system's handlers (desktop_actions.h), on macOS: NSWorkspace.

#import <Cocoa/Cocoa.h>

#include "base/strings/sys_string_conversions.h"
#include "zebkit/engine/desktop_actions.h"

namespace zebkit {

bool OpenWithSystem(const GURL& url, std::string* error) {
  NSURL* target = [NSURL URLWithString:base::SysUTF8ToNSString(url.spec())];
  if (!target || ![NSWorkspace.sharedWorkspace openURL:target]) {
    *error = "no program opens this URL";
    return false;
  }
  return true;
}

bool ShowInFileManager(const base::FilePath& path, std::string* error) {
  NSURL* file = [NSURL fileURLWithPath:base::SysUTF8ToNSString(path.value())];
  [NSWorkspace.sharedWorkspace activateFileViewerSelectingURLs:@[ file ]];
  return true;
}

}  // namespace zebkit
