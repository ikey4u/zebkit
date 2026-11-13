// The client certificate picker as a sheet (certificate_picker.h), on macOS.

#import <Cocoa/Cocoa.h>

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/strings/sys_string_conversions.h"
#include "content/public/browser/web_contents.h"
#include "zebkit/engine/certificate_picker.h"
#include "zebkit/engine/engine.h"

namespace zebkit {

base::OnceClosure ShowCertificatePicker(content::WebContents* contents, const std::u16string& host,
                                        const std::vector<std::u16string>& names, CertificateAnswer& answer) {
  if (HasNoDisplay(contents) || names.empty())
    return base::OnceClosure();
  NSWindow* window = contents->GetTopLevelNativeWindow().GetNativeNSWindow();
  if (!window)
    return base::OnceClosure();
  NSAlert* alert = [[NSAlert alloc] init];
  alert.messageText = @"Choose a certificate";
  alert.informativeText = base::SysUTF16ToNSString(host + u" asks for a certificate.");
  [alert addButtonWithTitle:@"OK"];
  [alert addButtonWithTitle:@"Cancel"];
  NSPopUpButton* list = [[NSPopUpButton alloc] initWithFrame:NSMakeRect(0, 0, 300, 26) pullsDown:NO];
  for (const std::u16string& name : names)
    [list addItemWithTitle:base::SysUTF16ToNSString(name)];
  alert.accessoryView = list;
  auto shared = std::make_shared<CertificateAnswer>(std::move(answer));
  [alert beginSheetModalForWindow:window
                completionHandler:^(NSModalResponse response) {
                  if (!*shared)
                    return;
                  if (response == NSAlertFirstButtonReturn && list.indexOfSelectedItem >= 0)
                    std::move(*shared).Run(static_cast<size_t>(list.indexOfSelectedItem));
                  else
                    std::move(*shared).Run(std::nullopt);
                }];
  // Closed unanswered: the answer is dropped first, so that the completion
  // handler ending the sheet runs nothing.
  __weak NSWindow* weak_window = window;
  __weak NSAlert* weak_alert = alert;
  return base::BindOnce(^{
    shared->Reset();
    NSAlert* shown = weak_alert;
    if (shown && weak_window)
      [weak_window endSheet:shown.window returnCode:NSModalResponseAbort];
  });
}

}  // namespace zebkit
