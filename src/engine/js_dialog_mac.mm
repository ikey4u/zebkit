// Copyright 2013 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "zebkit/engine/js_dialog.h"

#import <Cocoa/Cocoa.h>

#include "base/memory/raw_ptr.h"
#include "base/strings/sys_string_conversions.h"
#include "zebkit/engine/js_dialog_manager.h"

// Helper object that receives the notification that the dialog/sheet is
// going away. The sheet's completion handler holds it, not the C++ dialog:
// a sheet AppKit queues behind another on the same window can end after
// its dialog was cancelled and deleted (JavaScriptDialogManagerImpl::
// CancelDialogs()), and then the handler does nothing.
@interface ZebkitJavaScriptDialogHelper : NSObject<NSAlertDelegate> {
  NSAlert* __strong _alert;
  NSTextField* __weak _textField;
  NSWindow* __weak _parent;
  // Cancelled or gone: the end of the sheet answers nothing.
  BOOL _cancelled;

  // Copies of the fields in JavaScriptDialog because they're private.
  raw_ptr<zebkit::JavaScriptDialogManagerImpl> _manager;
  // The dialog, for the manager; cleared when it is cancelled or deleted.
  raw_ptr<zebkit::JavaScriptDialog> _dialog;
  content::JavaScriptDialogManager::DialogClosedCallback _callback;
}

- (id)initHelperWithManager:(zebkit::JavaScriptDialogManagerImpl*)manager
                     dialog:(zebkit::JavaScriptDialog*)dialog
                     parent:(NSWindow*)parent
                andCallback:(content::JavaScriptDialogManager::DialogClosedCallback)callback;
- (NSAlert*)alert;
- (NSTextField*)textField;
- (void)alertDidEndWithResult:(NSModalResponse)returnCode;
- (void)cancel;
// The dialog is deleted without being cancelled (its manager went).
- (void)detach;

@end

@implementation ZebkitJavaScriptDialogHelper

- (id)initHelperWithManager:(zebkit::JavaScriptDialogManagerImpl*)manager
                     dialog:(zebkit::JavaScriptDialog*)dialog
                     parent:(NSWindow*)parent
                andCallback:(content::JavaScriptDialogManager::DialogClosedCallback)callback {
  if (self = [super init]) {
    _manager = manager;
    _dialog = dialog;
    _parent = parent;
    _callback = std::move(callback);
  }

  return self;
}

- (NSAlert*)alert {
  _alert = [[NSAlert alloc] init];
  return _alert;
}

- (NSTextField*)textField {
  NSTextField* textField =
      [[NSTextField alloc] initWithFrame:NSMakeRect(0, 0, 300, 22)];
  textField.cell.lineBreakMode = NSLineBreakByTruncatingTail;

  _alert.accessoryView = textField;
  _alert.window.initialFirstResponder = textField;

  _textField = textField;
  return textField;
}

- (void)alertDidEndWithResult:(NSModalResponse)returnCode {
  // The sheet is over: the alert is let go here, not while it may still be
  // on the screen (or queued) after a cancel. Held to the end of this method,
  // for its text field.
  NSAlert* alert = _alert;
  _alert = nil;
  if (_cancelled || returnCode == NSModalResponseStop || !_callback) {
    return;
  }

  bool success = returnCode == NSAlertFirstButtonReturn;
  std::u16string input;
  if (alert && _textField) {
    input = base::SysNSStringToUTF16(_textField.stringValue);
  }

  // The manager lets the dialog go first (it detaches this helper, which the
  // sheet's handler keeps alive); then the page has its answer, which may
  // show the next dialog.
  auto callback = std::move(_callback);
  zebkit::JavaScriptDialog* dialog = _dialog;
  _dialog = nullptr;
  if (dialog && _manager)
    _manager->DialogClosed(dialog);
  std::move(callback).Run(success, input);
}

- (void)cancel {
  // First: the handler that ending the sheet runs does nothing.
  _cancelled = YES;
  _dialog = nullptr;
  NSAlert* alert = _alert;
  if (alert) {
    NSWindow* sheet = alert.window;
    NSWindow* parent = _parent;
    if (parent)
      [parent endSheet:sheet returnCode:NSModalResponseStop];
    else
      [NSApp endSheet:sheet];
  }
  if (_callback) {
    std::move(_callback).Run(false, std::u16string());
  }
}

- (void)detach {
  _cancelled = YES;
  _dialog = nullptr;
  _manager = nullptr;
}

@end

namespace zebkit {

JavaScriptDialog::JavaScriptDialog(
    JavaScriptDialogManagerImpl* manager,
    gfx::NativeWindow parent_window,
    JavaScriptDialogType dialog_type,
    const std::u16string& title,
    const std::u16string& message_text,
    const std::u16string& default_prompt_text,
    JavaScriptDialogManager::DialogClosedCallback callback) {
  bool text_field = dialog_type == JAVASCRIPT_DIALOG_TYPE_PROMPT;
  bool one_button = dialog_type == JAVASCRIPT_DIALOG_TYPE_ALERT;

  NSWindow* parent = parent_window.GetNativeNSWindow();
  helper_ = [[ZebkitJavaScriptDialogHelper alloc]
      initHelperWithManager:manager
                     dialog:this
                     parent:parent
                andCallback:std::move(callback)];

  // Show the modal dialog.
  NSAlert* alert = [helper_ alert];
  NSTextField* field = nil;
  if (text_field) {
    field = [helper_ textField];
    field.stringValue = base::SysUTF16ToNSString(default_prompt_text);
  }
  alert.delegate = helper_;
  // As the views dialog of Linux and Windows: the origin, then the text.
  if (title.empty()) {
    alert.messageText = base::SysUTF16ToNSString(message_text);
  } else {
    alert.messageText = base::SysUTF16ToNSString(title);
    alert.informativeText = base::SysUTF16ToNSString(message_text);
  }
  [alert addButtonWithTitle:@"OK"];
  if (!one_button) {
    NSButton* other = [alert addButtonWithTitle:@"Cancel"];
    other.keyEquivalent = @"\e";
  }

  // A sheet on the page's window, so that each window has its own; without
  // one, app-modal. The handler holds the helper, never this dialog.
  ZebkitJavaScriptDialogHelper* helper = helper_;
  [alert beginSheetModalForWindow:parent
                completionHandler:^void(NSModalResponse returnCode) {
                  [helper alertDidEndWithResult:returnCode];
                }];
}

JavaScriptDialog::~JavaScriptDialog() {
  [helper_ detach];
}

void JavaScriptDialog::Cancel() {
  [helper_ cancel];
}

}  // namespace zebkit
