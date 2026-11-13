// The system's handlers (desktop_actions.h), on macOS: NSWorkspace.

#import <Cocoa/Cocoa.h>

#include <string>
#include <utility>

#include "base/functional/bind.h"
#include "base/strings/sys_string_conversions.h"
#include "base/task/sequenced_task_runner.h"
#include "zebkit/engine/desktop_actions.h"

namespace zebkit {

// NSWorkspace on the engine's (main) thread, as AppKit wants; the answer is
// posted, so that it comes after the call that asked.
void OpenWithSystem(const GURL& url, DesktopDone done) {
  NSURL* target = [NSURL URLWithString:base::SysUTF8ToNSString(url.spec())];
  const bool opened = target && [NSWorkspace.sharedWorkspace openURL:target];
  base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE, base::BindOnce(std::move(done), opened, opened ? std::string() : "no program opens this URL"));
}

// The Finder says nothing of how it went: done once it is asked, the file
// being there.
void ShowInFileManager(const base::FilePath& path, DesktopDone done) {
  NSURL* file = [NSURL fileURLWithPath:base::SysUTF8ToNSString(path.value())];
  [NSWorkspace.sharedWorkspace activateFileViewerSelectingURLs:@[ file ]];
  base::SequencedTaskRunner::GetCurrentDefault()->PostTask(FROM_HERE,
                                                           base::BindOnce(std::move(done), true, std::string()));
}

}  // namespace zebkit
