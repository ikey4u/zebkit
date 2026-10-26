// WebAuthn uses Chromium's FIDO stack; Cocoa supplies the small browser UI.
// AuthenticationServices owns iCloud credentials and its biometric prompts.
#include "zebkit/engine/webauthn.h"

#import <AppKit/AppKit.h>
#import <CoreImage/CoreImage.h>

#include <array>
#include <map>
#include <string>
#include <utility>

#include "base/apple/owned_objc.h"
#include "base/functional/bind.h"
#include "base/memory/weak_ptr.h"
#include "base/no_destructor.h"
#include "base/scoped_observation.h"
#include "base/strings/sys_string_conversions.h"
#include "base/task/single_thread_task_runner.h"
#include "content/public/browser/authenticator_request_client_delegate.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/storage_partition.h"
#include "content/public/browser/web_authentication_delegate.h"
#include "content/public/browser/web_contents.h"
#include "crypto/random.h"
#include "device/fido/cable/v2_constants.h"
#include "device/fido/cable/v2_handshake.h"
#include "device/fido/fido_authenticator.h"
#include "device/fido/fido_discovery_factory.h"
#include "device/fido/mac/icloud_keychain.h"
#include "device/fido/pin.h"
#include "ui/gfx/native_ui_types.h"
#include "zebkit/engine/view.h"

namespace zebkit {
namespace {
using Request = content::AuthenticatorRequestClientDelegate;
using Handler = device::FidoRequestHandlerBase;
using Transport = device::FidoTransportProtocol;

auto& AuthenticationPanels() {
  static base::NoDestructor<
      std::map<content::GlobalRenderFrameHostId, base::apple::WeakNSWindow>>
      panels;
  return *panels;
}

zebkit_view* ViewFor(content::RenderFrameHost* frame) {
  return frame ? ViewOf(content::WebContents::FromRenderFrameHost(frame))
               : nullptr;
}

bool OffscreenUIEnabled(content::RenderFrameHost* frame) {
  auto* view = ViewFor(frame);
  return view && view->offscreen_view_ && view->authentication_ui_enabled_;
}

NSWindow* WindowFor(content::RenderFrameHost* frame) {
  if (!frame)
    return nil;
  auto* contents = content::WebContents::FromRenderFrameHost(frame);
  if (!contents)
    return nil;
  if (auto* view = ViewOf(contents); view && !view->authentication_ui_enabled_)
    return nil;
  NSWindow* window = contents->GetNativeView().GetNativeNSView().window;
  return window ?: contents->GetTopLevelNativeWindow().GetNativeNSWindow();
}

bool CanUseSystemPasskeys() {
  // IsSupported checks Apple's browser entitlement as well as the OS version.
  // A permission that has already been denied is never treated as available.
  return device::fido::icloud_keychain::IsSupported() &&
         device::fido::icloud_keychain::HasPermission() != false;
}

class WebAuthnDelegate : public content::WebAuthenticationDelegate {
 public:
  bool SupportsResidentKeys(content::RenderFrameHost* frame) override {
    return WebAuthenticationDelegate::SupportsResidentKeys(frame) ||
           WindowFor(frame) || OffscreenUIEnabled(frame);
  }
  bool SupportsConditionalMediation(content::RenderFrameHost* frame) override {
    // There is no autofill surface in this library. Keep the virtual test API.
    return WebAuthenticationDelegate::SupportsResidentKeys(frame);
  }
  void IsUserVerifyingPlatformAuthenticatorAvailableOverride(
      content::RenderFrameHost* frame,
      base::OnceCallback<void(std::optional<bool>)> callback) override {
    WebAuthenticationDelegate::
        IsUserVerifyingPlatformAuthenticatorAvailableOverride(
            frame,
            base::BindOnce(
                [](bool available,
                   base::OnceCallback<void(std::optional<bool>)> callback,
                   std::optional<bool> test_override) {
                  std::move(callback).Run(test_override.value_or(available));
                },
                (WindowFor(frame) || OffscreenUIEnabled(frame)) &&
                    CanUseSystemPasskeys(),
                std::move(callback)));
  }
  bool IsFocused(content::WebContents* contents) override {
    if (WebAuthenticationDelegate::SupportsResidentKeys(
            contents->GetPrimaryMainFrame()))
      return true;
    auto* view = ViewOf(contents);
    if (view && !view->authentication_ui_enabled_)
      return false;
    for (const auto& [id, panel] : AuthenticationPanels()) {
      auto* frame = content::RenderFrameHost::FromID(id);
      NSWindow* window = panel.Get();
      if (frame &&
          content::WebContents::FromRenderFrameHost(frame) == contents &&
          window && (window.keyWindow || window.attachedSheet.keyWindow))
        return true;
    }
    auto* frame = contents->GetPrimaryMainFrame();
    if (OffscreenUIEnabled(frame))
      return view->offscreen_focused_ && frame->HasTransientUserActivation();
    NSWindow* window = contents->GetNativeView().GetNativeNSView().window;
    return contents->GetVisibility() == content::Visibility::VISIBLE &&
           NSApp.active && window && (window.keyWindow || window.mainWindow);
  }
};

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

class WebAuthnRequest : public Request {
 public:
  explicit WebAuthnRequest(content::RenderFrameHost* frame)
      : frame_id_(frame->GetGlobalId()), window_(WindowFor(frame)) {}
  ~WebAuthnRequest() override {
    weak_factory_.InvalidateWeakPtrs();
    CloseSheet();
    if (close_observer_)
      [NSNotificationCenter.defaultCenter removeObserver:close_observer_];
    AuthenticationPanels().erase(frame_id_);
    if (panel_) {
      const bool restore = NSApp.active;
      [panel_ close];
      if (restore)
        [previous_app_
            activateWithOptions:NSApplicationActivateIgnoringOtherApps];
    }
  }

  void SetUIPresentation(UIPresentation presentation) override {
    presentation_ = presentation;
  }

  void ConfigureDiscoveries(
      const url::Origin& origin,
      const std::string& rp_id,
      RequestSource source,
      device::FidoRequestType type,
      std::optional<device::ResidentKeyRequirement> resident_key,
      device::UserVerificationRequirement verification,
      bool cmtg_key,
      std::optional<std::string_view> user_name,
      bool enclave_available,
      device::FidoDiscoveryFactory* factory) override {
    origin_ = origin.Serialize();
    if (!factory || IsVirtualEnvironmentEnabled() ||
        presentation_ != UIPresentation::kModal)
      return;
    if (!window_.Get())
      CreateOffscreenPanel();
    if (!window_.Get())
      return;
    factory->set_nswindow(window_);
    std::array<uint8_t, device::cablev2::kQRKeySize> key;
    crypto::RandBytes(key);
    qr_ = device::cablev2::qr::Encode(key, type);
    factory->set_cable_data(type, key);
    factory->set_network_context_factory(base::BindRepeating(
        [](content::GlobalRenderFrameHostId id) {
          auto* frame = content::RenderFrameHost::FromID(id);
          return frame ? frame->GetStoragePartition()->GetNetworkContext()
                       : nullptr;
        },
        frame_id_));
    factory->set_cable_event_callback(base::BindRepeating(
        &WebAuthnRequest::OnPhoneEvent, weak_factory_.GetWeakPtr()));
  }

  void RegisterActionCallbacks(
      base::OnceClosure cancel,
      base::OnceClosure immediate_not_found,
      base::RepeatingClosure start_over,
      AccountPreselectedCallback preselect,
      PasswordSelectedCallback password,
      Handler::RequestCallback dispatch,
      base::OnceClosure cancel_ui_timeout,
      base::RepeatingClosure power_on,
      base::RepeatingCallback<void(Handler::BlePermissionCallback)> permission)
      override {
    cancel_ = std::move(cancel);
    immediate_not_found_ = std::move(immediate_not_found);
    dispatch_ = std::move(dispatch);
    power_on_ = std::move(power_on);
    permission_ = std::move(permission);
    // Never create a native dialog for autofill, unsupported ambient/upgrade
    // requests, or a renderer that has no actual host window.
    if (!IsVirtualEnvironmentEnabled() &&
        (!window_.Get() || presentation_ != UIPresentation::kModal)) {
      base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
          FROM_HERE,
          base::BindOnce(&WebAuthnRequest::Cancel, weak_factory_.GetWeakPtr()));
    }
  }

  void StartObserving(Handler* handler) override {
    observation_.Observe(handler);
  }
  void StopObserving(Handler* handler) override { observation_.Reset(); }

  void OnTransportAvailabilityEnumerated(
      Handler::TransportAvailabilityInfo info) override {
    if (IsVirtualEnvironmentEnabled() ||
        presentation_ != UIPresentation::kModal)
      return;
    info_ = std::move(info);
    transports_enumerated_ = true;
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE, base::BindOnce(&WebAuthnRequest::ShowOptions,
                                  weak_factory_.GetWeakPtr()));
  }

  bool EmbedderControlsAuthenticatorDispatch(
      const device::FidoAuthenticator& authenticator) override {
    return !IsVirtualEnvironmentEnabled() &&
           authenticator.AuthenticatorTransport() == Transport::kInternal;
  }
  void FidoAuthenticatorAdded(
      const device::FidoAuthenticator& authenticator) override {
    if (authenticator.GetType() == device::AuthenticatorType::kICloudKeychain) {
      system_id_ = authenticator.GetId();
      if (using_system_)
        DispatchSystem();
    }
  }
  void FidoAuthenticatorRemoved(std::string_view id) override {
    if (id == system_id_)
      system_id_.clear();
  }
  void BluetoothAdapterStatusChanged(Handler::BleStatus status) override {
    info_.ble_status = status;
  }
  bool DoesBlockRequestOnFailure(InterestingFailureReason reason) override {
    CloseSheet();
    return false;
  }
  void OnTransactionSuccessful(RequestSource,
                               device::FidoRequestType,
                               device::AuthenticatorType) override {
    CloseSheet();
  }

  void SelectAccount(
      std::vector<device::AuthenticatorGetAssertionResponse> responses,
      base::OnceCallback<void(device::AuthenticatorGetAssertionResponse)>
          callback) override {
    if (IsVirtualEnvironmentEnabled()) {
      Request::SelectAccount(std::move(responses), std::move(callback));
      return;
    }
    if (responses.empty()) {
      Cancel();
      return;
    }
    selecting_account_ = true;
    NSPopUpButton* accounts =
        [[NSPopUpButton alloc] initWithFrame:NSMakeRect(0, 0, 320, 30)
                                   pullsDown:NO];
    size_t ordinal = 0;
    for (const auto& response : responses) {
      std::string name =
          response.user_entity
              ? response.user_entity->display_name.value_or(
                    response.user_entity->name.value_or("Account"))
              : "Account";
      [accounts
          addItemWithTitle:[NSString
                               stringWithFormat:@"%zu. %@", ++ordinal,
                                                base::SysUTF8ToNSString(name)]];
    }
    Show(
        @"Choose your account", Site(), @[ @"Continue", @"Cancel" ], accounts,
        base::BindOnce(
            [](base::WeakPtr<WebAuthnRequest> self, NSPopUpButton* accounts,
               std::vector<device::AuthenticatorGetAssertionResponse> responses,
               base::OnceCallback<void(
                   device::AuthenticatorGetAssertionResponse)> callback,
               NSModalResponse result) {
              if (!self)
                return;
              self->selecting_account_ = false;
              const NSInteger index = accounts.indexOfSelectedItem;
              if (result != NSAlertFirstButtonReturn || index < 0 ||
                  static_cast<size_t>(index) >= responses.size()) {
                self->Cancel();
                return;
              }
              std::move(callback).Run(std::move(responses[index]));
            },
            weak_factory_.GetWeakPtr(), accounts, std::move(responses),
            std::move(callback)));
  }

  bool SupportsPIN() const override { return window_.Get(); }
  void CollectPIN(CollectPINOptions options,
                  base::OnceCallback<void(std::u16string)> callback) override {
    pin_callback_ = std::move(callback);
    PromptPIN(options);
  }
  void FinishCollectToken() override {
    CloseSheet();
    if (transports_enumerated_)
      ShowExternal();
  }
  void StartBioEnrollment(base::OnceClosure next) override {
    // Enrolling new device biometrics is outside this request UI. End the
    // request rather than leave a hardware authenticator waiting indefinitely.
    Cancel();
  }
  void OnRetryUserVerification(int attempts) override {
    if (sheet_)
      sheet_.informativeText = [Site()
          stringByAppendingFormat:
              @"\nVerify on your security key again. Attempts remaining: %d",
              attempts];
  }

 private:
  void CreateOffscreenPanel() {
    auto* frame = content::RenderFrameHost::FromID(frame_id_);
    auto* view = ViewFor(frame);
    if (!OffscreenUIEnabled(frame) || !view->offscreen_focused_ ||
        !frame->HasTransientUserActivation())
      return;
    auto* contents = content::WebContents::FromRenderFrameHost(frame);
    for (const auto& [id, panel] : AuthenticationPanels()) {
      auto* other = content::RenderFrameHost::FromID(id);
      if (other && content::WebContents::FromRenderFrameHost(other) == contents)
        return;
    }
    previous_app_ = NSWorkspace.sharedWorkspace.frontmostApplication;
    panel_ =
        [[NSPanel alloc] initWithContentRect:NSMakeRect(0, 0, 440, 120)
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
    // An off-screen browser may be an agent behind a separate host UI.
    // Keep verification reachable above that host without a second Dock app.
    [panel_ makeKeyAndOrderFront:nil];
    window_ = base::apple::WeakNSWindow(panel_);
    AuthenticationPanels().emplace(frame_id_, window_);
    auto weak = weak_factory_.GetWeakPtr();
    close_observer_ = [NSNotificationCenter.defaultCenter
        addObserverForName:NSWindowWillCloseNotification
                    object:panel_
                     queue:nil
                usingBlock:^(NSNotification*) {
                  if (weak)
                    weak->Cancel();
                }];
  }
  NSString* Site() const {
    return base::SysUTF8ToNSString("Verify your identity for " + origin_ + ".");
  }
  void CloseSheet() {
    NSAlert* sheet = sheet_;
    sheet_ = nil;
    if (sheet.window.sheetParent)
      [sheet.window.sheetParent endSheet:sheet.window
                              returnCode:NSModalResponseCancel];
    [sheet.window orderOut:nil];
  }
  void Show(NSString* title,
            NSString* message,
            NSArray<NSString*>* buttons,
            NSView* accessory,
            base::OnceCallback<void(NSModalResponse)> callback) {
    CloseSheet();
    NSWindow* window = window_.Get();
    if (!window || !window.visible) {
      Cancel();
      return;
    }
    NSAlert* alert = [[NSAlert alloc] init];
    alert.messageText = title;
    alert.informativeText = message;
    for (NSString* button_title in buttons)
      [alert addButtonWithTitle:button_title];
    alert.buttons.lastObject.keyEquivalent = @"\x1b";
    alert.accessoryView = accessory;
    sheet_ = alert;
    auto weak = weak_factory_.GetWeakPtr();
    __block auto completion = std::move(callback);
    [alert beginSheetModalForWindow:window
                  completionHandler:^(NSModalResponse result) {
                    if (!weak || weak->sheet_ != alert)
                      return;
                    weak->sheet_ = nil;
                    std::move(completion).Run(result);
                  }];
  }
  void Cancel() {
    CloseSheet();
    if (presentation_ == UIPresentation::kModalImmediate &&
        immediate_not_found_)
      std::move(immediate_not_found_).Run();
    else if (cancel_)
      std::move(cancel_).Run();
    // The callback can destroy this request.
  }
  void ShowOptions() {
    // USB discovery may have reached a PIN/account prompt before transport
    // enumeration finishes. Do not replace a prompt with initial choices.
    if (pin_callback_ || selecting_account_)
      return;
    if (!window_.Get()) {
      Cancel();
      return;
    }
    if (!info_.has_icloud_keychain || !CanUseSystemPasskeys()) {
      ShowExternal();
      return;
    }
    const bool external =
        info_.available_transports.contains(Transport::kHybrid) ||
        info_.available_transports.contains(
            Transport::kUsbHumanInterfaceDevice);
    Show(@"Use a passkey", Site(),
         external ? @[ @"This Mac", @"Phone or security key", @"Cancel" ]
                  : @[ @"This Mac", @"Cancel" ],
         nil,
         base::BindOnce(
             [](base::WeakPtr<WebAuthnRequest> self, bool offer_external,
                NSModalResponse result) {
               if (!self)
                 return;
               if (result == NSAlertFirstButtonReturn) {
                 self->using_system_ = true;
                 self->DispatchSystem();
               } else if (offer_external &&
                          result == NSAlertSecondButtonReturn) {
                 self->ShowExternal();
               } else {
                 self->Cancel();
               }
             },
             weak_factory_.GetWeakPtr(), external));
  }
  void DispatchSystem() {
    if (!system_id_.empty()) {
      CloseSheet();
      auto callback = dispatch_;
      const std::string id = system_id_;
      callback.Run(id);
    } else {
      Show(@"Preparing system authentication", Site(), @[ @"Cancel" ], nil,
           base::BindOnce(
               [](base::WeakPtr<WebAuthnRequest> self, NSModalResponse) {
                 if (self)
                   self->Cancel();
               },
               weak_factory_.GetWeakPtr()));
    }
  }
  void ShowExternal() {
    const bool denied =
        info_.ble_status == Handler::BleStatus::kPermissionDenied;
    const bool phone =
        !denied && info_.available_transports.contains(Transport::kHybrid);
    const bool usb = info_.available_transports.contains(
        Transport::kUsbHumanInterfaceDevice);
    if (!phone && !usb) {
      Show(@"No passkey provider is available",
           [Site() stringByAppendingString:
                       @" Choose another verification method on the website."],
           @[ @"Return to website" ], nil,
           base::BindOnce(
               [](base::WeakPtr<WebAuthnRequest> self, NSModalResponse) {
                 if (self)
                   self->Cancel();
               },
               weak_factory_.GetWeakPtr()));
      return;
    }
    NSString* instructions = phone ? @"\nScan this code with a nearby phone or "
                                     @"tablet. Keep Bluetooth on."
                                   : @"\nInsert and touch your security key.";
    if (phone && usb)
      instructions =
          [instructions stringByAppendingString:
                            @" You can also insert and touch a security key."];
    if (denied)
      instructions = [instructions
          stringByAppendingString:@"\nTo use a phone, allow this app to use "
                                  @"Bluetooth in System Settings."];
    const bool enable_ble =
        phone &&
        (info_.ble_status == Handler::BleStatus::kPendingPermissionRequest ||
         (info_.ble_status == Handler::BleStatus::kOff &&
          info_.can_power_on_ble_adapter));
    Show(@"Use a phone or security key",
         [Site() stringByAppendingString:instructions],
         enable_ble ? @[ @"Enable Bluetooth", @"Cancel" ] : @[ @"Cancel" ],
         phone ? QRView(qr_) : nil,
         base::BindOnce(
             [](base::WeakPtr<WebAuthnRequest> self, bool enable,
                NSModalResponse result) {
               if (!self)
                 return;
               if (enable && result == NSAlertFirstButtonReturn) {
                 self->EnableBluetooth();
               } else {
                 self->Cancel();
               }
             },
             weak_factory_.GetWeakPtr(), enable_ble));
  }
  void EnableBluetooth() {
    if (info_.ble_status == Handler::BleStatus::kPendingPermissionRequest) {
      permission_.Run(base::BindOnce(
          [](base::WeakPtr<WebAuthnRequest> self, Handler::BleStatus status) {
            if (!self)
              return;
            self->info_.ble_status = status;
            self->ShowExternal();
          },
          weak_factory_.GetWeakPtr()));
    } else {
      if (info_.ble_status == Handler::BleStatus::kOff &&
          info_.can_power_on_ble_adapter)
        power_on_.Run();
      ShowExternal();
    }
  }
  void OnPhoneEvent(device::cablev2::Event event) {
    if (sheet_ && !pin_callback_ && !selecting_account_)
      sheet_.informativeText = [Site()
          stringByAppendingString:
              @"\nFollow the instructions on your phone to finish verifying."];
  }
  void PromptPIN(CollectPINOptions options) {
    const bool setting =
        options.reason != device::pin::PINEntryReason::kChallenge;
    NSView* view =
        [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 320, setting ? 72 : 32)];
    NSSecureTextField* pin = [[NSSecureTextField alloc]
        initWithFrame:NSMakeRect(0, setting ? 40 : 0, 320, 28)];
    pin.placeholderString =
        setting ? @"New security-key PIN" : @"Security-key PIN";
    [view addSubview:pin];
    NSSecureTextField* confirm = nil;
    if (setting) {
      confirm =
          [[NSSecureTextField alloc] initWithFrame:NSMakeRect(0, 0, 320, 28)];
      confirm.placeholderString = @"Repeat the new PIN";
      [view addSubview:confirm];
    }
    NSString* message = Site();
    if (options.error != device::pin::PINEntryError::kNoError)
      message = [message
          stringByAppendingString:
              @"\nThe PIN was not accepted. Check it before trying again."];
    if (!setting && options.attempts > 0)
      message = [message stringByAppendingFormat:@"\nAttempts remaining: %d",
                                                 options.attempts];
    Show(setting ? @"Set a security-key PIN" : @"Enter your security-key PIN",
         message, @[ @"Continue", @"Cancel" ], view,
         base::BindOnce(
             [](base::WeakPtr<WebAuthnRequest> self, NSSecureTextField* pin,
                NSSecureTextField* confirm, CollectPINOptions options,
                NSModalResponse result) {
               if (!self)
                 return;
               if (result != NSAlertFirstButtonReturn) {
                 self->Cancel();
                 return;
               }
               const std::u16string value =
                   base::SysNSStringToUTF16(pin.stringValue);
               const bool match =
                   !confirm ||
                   [pin.stringValue isEqualToString:confirm.stringValue];
               pin.stringValue = @"";
               confirm.stringValue = @"";
               const auto error =
                   device::pin::ValidatePIN(value, options.min_pin_length);
               if (!match || error != device::pin::PINEntryError::kNoError) {
                 options.error = error == device::pin::PINEntryError::kNoError
                                     ? device::pin::PINEntryError::kWrongPIN
                                     : error;
                 self->PromptPIN(options);
                 return;
               }
               std::move(self->pin_callback_).Run(value);
             },
             weak_factory_.GetWeakPtr(), pin, confirm, options));
  }

  content::GlobalRenderFrameHostId frame_id_;
  base::apple::WeakNSWindow window_;
  NSAlert* __strong sheet_ = nil;
  NSPanel* __strong panel_ = nil;
  NSRunningApplication* __strong previous_app_ = nil;
  id __strong close_observer_ = nil;
  UIPresentation presentation_ = UIPresentation::kModal;
  Handler::TransportAvailabilityInfo info_;
  std::string origin_;
  std::string qr_;
  std::string system_id_;
  bool using_system_ = false;
  bool transports_enumerated_ = false;
  bool selecting_account_ = false;
  base::OnceClosure cancel_;
  base::OnceClosure immediate_not_found_;
  Handler::RequestCallback dispatch_;
  base::RepeatingClosure power_on_;
  base::RepeatingCallback<void(Handler::BlePermissionCallback)> permission_;
  base::OnceCallback<void(std::u16string)> pin_callback_;
  base::ScopedObservation<Handler, Handler::Observer> observation_{this};
  base::WeakPtrFactory<WebAuthnRequest> weak_factory_{this};
};
}  // namespace

content::WebAuthenticationDelegate* GetWebAuthnDelegate() {
  static base::NoDestructor<WebAuthnDelegate> delegate;
  return delegate.get();
}
std::unique_ptr<content::AuthenticatorRequestClientDelegate>
CreateWebAuthnRequestDelegate(content::RenderFrameHost* frame) {
  return std::make_unique<WebAuthnRequest>(frame);
}
}  // namespace zebkit
