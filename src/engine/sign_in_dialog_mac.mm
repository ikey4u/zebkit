// The sign-in dialog as a sheet on the page's window (sign_in_dialog.h), on
// macOS: an NSAlert with a username and a secure password field.

#import <Cocoa/Cocoa.h>

#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "base/functional/bind.h"
#include "base/strings/sys_string_conversions.h"
#include "content/public/browser/web_contents.h"
#include "zebkit/engine/engine.h"
#include "zebkit/engine/sign_in_dialog.h"

namespace zebkit {

base::OnceClosure ShowSignInDialog(content::WebContents* contents, const std::u16string& title,
                                   const std::u16string& message, SignInAnswer& answer) {
  if (HasNoDisplay(contents))
    return base::OnceClosure();
  NSWindow* window = contents->GetTopLevelNativeWindow().GetNativeNSWindow();
  if (!window)
    return base::OnceClosure();
  NSAlert* alert = [[NSAlert alloc] init];
  alert.messageText = base::SysUTF16ToNSString(title);
  alert.informativeText = base::SysUTF16ToNSString(message);
  [alert addButtonWithTitle:@"Sign In"];
  [alert addButtonWithTitle:@"Cancel"];
  NSView* fields = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 260, 54)];
  NSTextField* username = [[NSTextField alloc] initWithFrame:NSMakeRect(0, 30, 260, 24)];
  username.placeholderString = @"Username";
  NSSecureTextField* password = [[NSSecureTextField alloc] initWithFrame:NSMakeRect(0, 0, 260, 24)];
  password.placeholderString = @"Password";
  [fields addSubview:username];
  [fields addSubview:password];
  alert.accessoryView = fields;
  alert.window.initialFirstResponder = username;
  auto shared = std::make_shared<SignInAnswer>(std::move(answer));
  // A sheet: it answers after this returns, as the views dialog does.
  [alert beginSheetModalForWindow:window
                completionHandler:^(NSModalResponse response) {
                  if (!*shared)
                    return;
                  if (response == NSAlertFirstButtonReturn)
                    std::move(*shared).Run(std::make_pair(base::SysNSStringToUTF16(username.stringValue),
                                                          base::SysNSStringToUTF16(password.stringValue)));
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
