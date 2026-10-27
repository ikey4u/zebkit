// Cocoa presentation only. WebAuthn/FIDO and host events live in webauthn.cc.
#import <AppKit/AppKit.h>
#import <CoreImage/CoreImage.h>
#include <algorithm>
#include "base/apple/owned_objc.h"
#include "base/memory/weak_ptr.h"
#include "base/strings/sys_string_conversions.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/web_contents.h"
#include "device/fido/fido_discovery_factory.h"
#include "device/fido/mac/icloud_keychain.h"
#include "zebkit/engine/view.h"
#include "zebkit/engine/webauthn_ui.h"
namespace zebkit {
namespace {
// Core Image is an OS framework: no QR renderer or browser UI is linked in.
NSView* QRView(const std::string& qr) {
  CIFilter* filter = [CIFilter filterWithName:@"CIQRCodeGenerator"];
  [filter setValue:[NSData dataWithBytes:qr.data() length:qr.size()]
            forKey:@"inputMessage"];
  [filter setValue:@"M" forKey:@"inputCorrectionLevel"];
  CIImage* code = filter.outputImage;
  if (!code)
    return nil;
  // Render at an integer scale, with the four-module quiet zone kept white.
  const CGFloat scale = 5;
  const CGFloat margin = 4 * scale;
  NSSize size = NSMakeSize(code.extent.size.width * scale + 2 * margin,
                           code.extent.size.height * scale + 2 * margin);
  NSImage* image = [[NSImage alloc] initWithSize:size];
  [image lockFocus];
  [[NSColor whiteColor] setFill];
  NSRectFill(NSMakeRect(0, 0, size.width, size.height));
  NSGraphicsContext.currentContext.imageInterpolation =
      NSImageInterpolationNone;
  NSCIImageRep* rep = [NSCIImageRep imageRepWithCIImage:code];
  [rep drawInRect:NSMakeRect(margin, margin, size.width - 2 * margin,
                             size.height - 2 * margin)];
  [image unlockFocus];
  NSImageView* view = [[NSImageView alloc]
      initWithFrame:NSMakeRect(0, 0, size.width, size.height)];
  view.image = image;
  return view;
}

NSString* String(const base::DictValue& event, std::string_view key) {
  const auto* value = event.FindString(key);
  return value ? base::SysUTF8ToNSString(*value) : @"";
}
class MacAuthenticationUI : public AuthenticationUI {
 public:
  MacAuthenticationUI(content::RenderFrameHost* frame,
                      bool host_ui,
                      AuthenticationResponse response)
      : response_(std::move(response)) {
    auto* contents = content::WebContents::FromRenderFrameHost(frame);
    NSWindow* window = contents->GetNativeView().GetNativeNSView().window;
    if (!window)
      window = contents->GetTopLevelNativeWindow().GetNativeNSWindow();
    window_ = base::apple::WeakNSWindow(window);
    if (!window && (!host_ui || SystemPasskeysAvailable())) {
      previous_app_ = NSWorkspace.sharedWorkspace.frontmostApplication;
      panel_ = [[NSPanel alloc]
          initWithContentRect:NSMakeRect(0, 0, 440, 120)
                    styleMask:NSWindowStyleMaskTitled |
                              NSWindowStyleMaskClosable |
                              NSWindowStyleMaskNonactivatingPanel
                      backing:NSBackingStoreBuffered
                        defer:NO];
      panel_.releasedWhenClosed = NO;
      panel_.title = @"Passkey verification";
      panel_.hidesOnDeactivate = NO;
      panel_.floatingPanel = YES;
      panel_.level = NSModalPanelWindowLevel;
      [panel_ center];
      if (!host_ui)
        [panel_ makeKeyAndOrderFront:nil];
      window_ = base::apple::WeakNSWindow(panel_);
      auto weak = weak_factory_.GetWeakPtr();
      close_observer_ = [NSNotificationCenter.defaultCenter
          addObserverForName:NSWindowWillCloseNotification
                      object:panel_
                       queue:nil
                  usingBlock:^(NSNotification*) {
                    if (weak)
                      weak->Submit("cancel");
                  }];
    }
  }
  ~MacAuthenticationUI() override {
    weak_factory_.InvalidateWeakPtrs();
    Dismiss();
    if (close_observer_)
      [NSNotificationCenter.defaultCenter removeObserver:close_observer_];
    if (panel_) {
      const bool restore = NSApp.active;
      [panel_ close];
      if (restore)
        [previous_app_
            activateWithOptions:NSApplicationActivateIgnoringOtherApps];
    }
  }
  bool Available() const override { return window_.Get(); }
  bool Focused() const override {
    NSWindow* window = window_.Get();
    return window && (window.keyWindow || window.attachedSheet.keyWindow ||
                      (NSApp.active && window.mainWindow));
  }
  void Configure(device::FidoDiscoveryFactory* factory) override {
    if (window_.Get())
      factory->set_nswindow(window_);
  }
  void Dismiss() override {
    ++generation_;
    NSAlert* sheet = alert_;
    alert_ = nil;
    if (sheet.window.sheetParent)
      [sheet.window.sheetParent endSheet:sheet.window
                              returnCode:NSModalResponseCancel];
    [sheet.window orderOut:nil];
  }
  void PrepareSystem() override {
    Dismiss();
    if (panel_)
      [panel_ makeKeyAndOrderFront:nil];
  }
  void Submit(std::string action,
              NSSecureTextField* pin = nil,
              NSSecureTextField* confirm = nil,
              NSPopUpButton* accounts = nil) {
    base::DictValue response;
    if (const auto* request = event_.FindString("request"))
      response.Set("request", *request);
    response.Set("revision", event_.FindInt("revision").value_or(0));
    response.Set("action", action);
    if (action == "pin") {
      response.Set("pin", base::SysNSStringToUTF8(pin.stringValue));
      pin.stringValue = @"";
      if (confirm) {
        response.Set("confirmation",
                     base::SysNSStringToUTF8(confirm.stringValue));
        confirm.stringValue = @"";
      }
    }
    if (action == "account") {
      const auto* list = event_.FindList("accounts");
      NSInteger index = accounts.indexOfSelectedItem;
      if (list && index >= 0 && static_cast<size_t>(index) < list->size())
        response.Set("account", *(*list)[index].GetDict().FindString("id"));
    }
    auto callback = response_;
    callback.Run(std::move(response));
  }
  void Show(const base::DictValue& event) override {
    Dismiss();
    event_ = event.Clone();
    NSWindow* window = window_.Get();
    if (!window || !window.visible) {
      Submit("cancel");
      return;
    }
    NSAlert* alert = [[NSAlert alloc] init];
    alert.messageText = String(event, "title");
    NSMutableString* message =
        [NSMutableString stringWithFormat:@"Verify your identity for %@.",
                                          String(event, "origin")];
    if (event.FindBool("phone").value_or(false))
      [message appendString:@"\nScan with a nearby phone that has this "
                            @"account's passkey. Keep Bluetooth on."];
    if (event.FindBool("security_key").value_or(false))
      [message
          appendString:@"\nYou can also insert and touch a USB security key."];
    if ([String(event, "bluetooth") isEqualToString:@"denied"])
      [message appendString:@"\nAllow Bluetooth access for this app in System "
                            @"Settings to use your phone."];
    if ([String(event, "bluetooth") isEqualToString:@"off"])
      [message
          appendString:
              @"\nTurn on Bluetooth in System Settings to use your phone."];
    for (std::string_view key : {"message", "error"}) {
      if (const auto* text = event.FindString(key))
        [message appendFormat:@"\n%@", base::SysUTF8ToNSString(*text)];
    }
    if (auto attempts = event.FindInt("attempts"); attempts && *attempts > 0)
      [message appendFormat:@"\nAttempts remaining: %d", *attempts];
    alert.informativeText = message;
    NSSecureTextField* pin = nil;
    NSSecureTextField* confirm = nil;
    NSPopUpButton* accounts = nil;
    if (const auto* qr = event.FindString("qr")) {
      NSView* image = QRView(*qr);
      image.accessibilityLabel =
          @"Passkey QR code. Scan with your phone camera.";
      alert.accessoryView = image;
    } else if ([String(event, "state") isEqualToString:@"pin"]) {
      const bool setting = event.FindBool("setting").value_or(false);
      NSView* fields = [[NSView alloc]
          initWithFrame:NSMakeRect(0, 0, 320, setting ? 72 : 32)];
      pin = [[NSSecureTextField alloc]
          initWithFrame:NSMakeRect(0, setting ? 40 : 0, 320, 28)];
      pin.placeholderString =
          setting ? @"New security-key PIN" : @"Security-key PIN";
      pin.accessibilityLabel = pin.placeholderString;
      [fields addSubview:pin];
      if (setting) {
        confirm =
            [[NSSecureTextField alloc] initWithFrame:NSMakeRect(0, 0, 320, 28)];
        confirm.placeholderString = @"Confirm new PIN";
        confirm.accessibilityLabel = confirm.placeholderString;
        [fields addSubview:confirm];
      }
      alert.accessoryView = fields;
    } else if (const auto* list = event.FindList("accounts")) {
      accounts = [[NSPopUpButton alloc] initWithFrame:NSMakeRect(0, 0, 320, 30)
                                            pullsDown:NO];
      accounts.accessibilityLabel = @"Choose your account";
      size_t index = 0;
      for (const auto& value : *list) {
        const auto& account = value.GetDict();
        NSString* name = String(account, "display_name");
        if (name.length == 0)
          name = String(account, "name");
        [accounts addItemWithTitle:[NSString stringWithFormat:@"%zu. %@",
                                                              ++index, name]];
      }
      alert.accessoryView = accounts;
    }
    std::vector<std::string> choices;
    const auto* actions = event.FindList("actions");
    for (const auto& [action, label] :
         std::initializer_list<std::pair<const char*, NSString*>>{
             {"system", @"This Mac"},
             {"external", @"Phone or security key"},
             {"bluetooth", @"Enable Bluetooth"},
             {"pin", @"Continue"},
             {"account", @"Continue"},
             {"cancel", @"Cancel"}}) {
      if (actions &&
          std::ranges::any_of(*actions, [&](const base::Value& value) {
            return value == base::Value(action);
          })) {
        choices.emplace_back(action);
        [alert addButtonWithTitle:label];
      }
    }
    alert.buttons.lastObject.keyEquivalent = @"\x1b";
    alert_ = alert;
    const int generation = generation_;
    auto weak = weak_factory_.GetWeakPtr();
    if (pin)
      alert.window.initialFirstResponder = pin;
    [alert beginSheetModalForWindow:window
                  completionHandler:^(NSModalResponse result) {
                    if (!weak || weak->generation_ != generation)
                      return;
                    const NSInteger index = result - NSAlertFirstButtonReturn;
                    const std::string action =
                        index >= 0 &&
                                static_cast<size_t>(index) < choices.size()
                            ? choices[index]
                            : "cancel";
                    weak->Submit(action, pin, confirm, accounts);
                  }];
  }

 private:
  AuthenticationResponse response_;
  base::DictValue event_;
  base::apple::WeakNSWindow window_;
  NSPanel* __strong panel_ = nil;
  NSAlert* __strong alert_ = nil;
  NSRunningApplication* __strong previous_app_ = nil;
  id __strong close_observer_ = nil;
  int generation_ = 0;
  base::WeakPtrFactory<MacAuthenticationUI> weak_factory_{this};
};
}  // namespace
bool SystemPasskeysAvailable() {
  return device::fido::icloud_keychain::IsSupported() &&
         device::fido::icloud_keychain::HasPermission() != false;
}
std::unique_ptr<AuthenticationUI> CreateAuthenticationUI(
    content::RenderFrameHost* frame,
    bool host_ui,
    AuthenticationResponse response) {
  return std::make_unique<MacAuthenticationUI>(frame, host_ui,
                                               std::move(response));
}
}  // namespace zebkit
