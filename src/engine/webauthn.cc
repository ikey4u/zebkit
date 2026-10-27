// Chromium owns WebAuthn/FIDO. This adapter owns request state and the public
// host UI protocol; native presentation is isolated in webauthn_* adapters.
#include "zebkit/engine/webauthn.h"
#include <array>
#include <map>
#include <set>
#include <string>
#include <utility>
#include "base/command_line.h"
#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/memory/weak_ptr.h"
#include "base/no_destructor.h"
#include "base/scoped_observation.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/single_thread_task_runner.h"
#include "content/public/browser/authenticator_request_client_delegate.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/render_widget_host_view.h"
#include "content/public/browser/storage_partition.h"
#include "content/public/browser/web_authentication_delegate.h"
#include "content/public/browser/web_contents.h"
#include "crypto/random.h"
#include "device/fido/cable/v2_constants.h"
#include "device/fido/cable/v2_handshake.h"
#include "device/fido/fido_authenticator.h"
#include "device/fido/fido_discovery_factory.h"
#include "device/fido/pin.h"
#include "zebkit/engine/view.h"
#include "zebkit/engine/webauthn_ui.h"

namespace zebkit {
namespace {
using Request = content::AuthenticatorRequestClientDelegate;
using Handler = device::FidoRequestHandlerBase;
using Transport = device::FidoTransportProtocol;
class WebAuthnRequest;
auto& ActiveRequests() {
  static base::NoDestructor<std::map<uint64_t, base::WeakPtr<WebAuthnRequest>>>
      requests;
  return *requests;
}
uint64_t NextRequest() {
  static uint64_t next = 0;
  return ++next;
}
zebkit_view* ViewFor(content::RenderFrameHost* frame) {
  return frame ? ViewOf(content::WebContents::FromRenderFrameHost(frame))
               : nullptr;
}
bool ViewCanPresent(content::RenderFrameHost* frame) {
  auto* view = ViewFor(frame);
  // A generic headless page has no trusted UI surface. Only an explicitly
  // opted-in off-screen host can present authentication from that process.
  return view && view->authentication_ui_enabled_ &&
         (view->offscreen_view_ ||
          !base::CommandLine::ForCurrentProcess()->HasSwitch("headless"));
}
bool PageFocused(content::RenderFrameHost* frame) {
  auto* view = ViewFor(frame);
  if (!ViewCanPresent(frame))
    return false;
  if (view->offscreen_view_)
    return view->offscreen_focused_;
  auto* contents = content::WebContents::FromRenderFrameHost(frame);
  auto* widget = contents->GetRenderWidgetHostView();
  return contents->GetVisibility() == content::Visibility::VISIBLE && widget &&
         widget->HasFocus();
}

class WebAuthnRequest : public Request {
 public:
  explicit WebAuthnRequest(content::RenderFrameHost* frame)
      : frame_id_(frame->GetGlobalId()),
        view_(ViewFor(frame)),
        owner_(view_ ? view_->GetWeakPtr() : base::WeakPtr<zebkit_view>()),
        request_id_(NextRequest()) {
    ActiveRequests().emplace(request_id_, weak_factory_.GetWeakPtr());
  }
  ~WebAuthnRequest() override {
    ActiveRequests().erase(request_id_);
    if (host_managed_)
      DeliverClosed();
    weak_factory_.InvalidateWeakPtrs();
    ui_.reset();
  }
  bool Focused() const {
    return owner_ && authorized_ &&
           (PageFocused(frame()) || (ui_ && ui_->Focused()));
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
    rp_id_ = rp_id;
    if (!factory || IsVirtualEnvironmentEnabled() ||
        presentation_ != UIPresentation::kModal)
      return;
    authorized_ =
        PageFocused(frame()) &&
        (!view_->offscreen_view_ || frame()->HasTransientUserActivation());
    if (!authorized_)
      return;
    // One modal authentication UI per view, including requests from its frames.
    // Defer cancellation so a host callback cannot destroy this request while
    // ConfigureDiscoveries is still using it.
    for (const auto& [id, previous] : ActiveRequests()) {
      if (id < request_id_ && previous && previous->BelongsTo(view_))
        base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
            FROM_HERE, base::BindOnce(&WebAuthnRequest::Superseded, previous));
    }
    host_managed_ = view_->on_authentication_;
    ui_ = CreateAuthenticationUI(
        frame(), host_managed_,
        base::BindRepeating(
            [](base::WeakPtr<WebAuthnRequest> self, base::DictValue action) {
              if (self)
                self->QueueAction(action);
            },
            weak_factory_.GetWeakPtr()));
    if (!host_managed_ && (!ui_ || !ui_->Available())) {
      authorized_ = false;
      return;
    }
    if (ui_)
      ui_->Configure(factory);
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
    if (!IsVirtualEnvironmentEnabled() && !authorized_)
      base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
          FROM_HERE,
          base::BindOnce(&WebAuthnRequest::Cancel, weak_factory_.GetWeakPtr()));
  }
  void StartObserving(Handler* handler) override {
    observation_.Observe(handler);
  }
  void StopObserving(Handler* handler) override { observation_.Reset(); }
  void OnTransportAvailabilityEnumerated(
      Handler::TransportAvailabilityInfo info) override {
    if (IsVirtualEnvironmentEnabled() || !authorized_)
      return;
    info_ = std::move(info);
    transports_enumerated_ = true;
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE, base::BindOnce(&WebAuthnRequest::ShowOptions,
                                  weak_factory_.GetWeakPtr()));
  }
  bool EmbedderControlsAuthenticatorDispatch(
      const device::FidoAuthenticator& authenticator) override {
    // Only the platform provider that this adapter explicitly selects is held.
    // Other native implementations keep Chromium's normal dispatch behavior.
    return !IsVirtualEnvironmentEnabled() &&
           authenticator.GetType() ==
               device::AuthenticatorType::kICloudKeychain;
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
    if (state_ == "external")
      ShowExternal();
  }
  bool DoesBlockRequestOnFailure(InterestingFailureReason reason) override {
    outcome_ = "failed";
    if (ui_)
      ui_->Dismiss();
    return false;
  }
  void OnTransactionSuccessful(RequestSource,
                               device::FidoRequestType,
                               device::AuthenticatorType) override {
    outcome_ = "success";
    if (ui_)
      ui_->Dismiss();
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
    base::ListValue accounts;
    for (size_t i = 0; i < responses.size(); ++i) {
      base::DictValue account;
      account.Set("id", base::NumberToString(i));
      account.Set("name", responses[i].user_entity
                              ? responses[i].user_entity->name.value_or("")
                              : "");
      account.Set("display_name",
                  responses[i].user_entity
                      ? responses[i].user_entity->display_name.value_or("")
                      : "");
      accounts.Append(std::move(account));
    }
    responses_ = std::move(responses);
    account_callback_ = std::move(callback);
    base::DictValue extra;
    extra.Set("accounts", std::move(accounts));
    Publish("account", "Choose your account", {"account", "cancel"},
            std::move(extra));
  }
  bool SupportsPIN() const override { return authorized_; }
  void CollectPIN(CollectPINOptions options,
                  base::OnceCallback<void(std::u16string)> callback) override {
    pin_callback_ = std::move(callback);
    PromptPIN(options);
  }
  void FinishCollectToken() override {
    if (transports_enumerated_)
      ShowExternal();
  }
  void StartBioEnrollment(base::OnceClosure next) override { Cancel(); }
  void OnRetryUserVerification(int attempts) override {
    base::DictValue extra;
    extra.Set("message", "Verify on your security key again.");
    extra.Set("attempts", attempts);
    Publish("waiting", "Check your security key", {"cancel"}, std::move(extra));
  }
  void Superseded() {
    outcome_ = "superseded";
    Cancel();
  }
  void PolicyDisabled() {
    authorized_ = false;
    outcome_ = "disabled";
    Cancel();
  }
  bool BelongsTo(zebkit_view* view) const { return owner_.get() == view; }
  void DetachHost() {
    host_managed_ = false;
    authorized_ = false;
    ui_.reset();
    Cancel();
  }
  bool QueueAction(const base::DictValue& action) {
    const auto* id = action.FindString("request");
    const auto* name = action.FindString("action");
    const auto revision = action.FindInt("revision");
    if (!authorized_ || !id || *id != base::NumberToString(request_id_) ||
        !revision || *revision != revision_ || !name ||
        !actions_.contains(*name) || action_pending_)
      return false;
    if (*name != "cancel" && !Focused())
      return false;
    if (*name == "pin" &&
        (!action.FindString("pin") ||
         (pin_options_.reason != device::pin::PINEntryReason::kChallenge &&
          !action.FindString("confirmation"))))
      return false;
    if (*name == "account") {
      const auto* account = action.FindString("account");
      size_t index;
      if (!account || !base::StringToSizeT(*account, &index) ||
          index >= responses_.size())
        return false;
    }
    action_pending_ = true;
    // Never run a FIDO callback on the embedder's callback stack.
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE,
        base::BindOnce(&WebAuthnRequest::RunAction, weak_factory_.GetWeakPtr(),
                       revision_, action.Clone()));
    return true;
  }

 private:
  content::RenderFrameHost* frame() const {
    return content::RenderFrameHost::FromID(frame_id_);
  }
  bool HostAlive() const { return owner_ && owner_->on_authentication_; }
  void DeliverClosed() {
    if (!HostAlive())
      return;
    base::DictValue event;
    event.Set("version", 1);
    event.Set("request", base::NumberToString(request_id_));
    event.Set("revision", ++revision_);
    event.Set("state", "closed");
    event.Set("outcome", outcome_);
    std::string json;
    base::JSONWriter::Write(event, &json);
    // Closing a request can happen while a host callback is still running
    // (for example when it disables UI). Deliver after that callback returns.
    // The view weak pointer and handler generation protect freed/replaced data.
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE,
        base::BindOnce(
            [](base::WeakPtr<zebkit_view> view, uint64_t generation,
               std::string json) {
              if (view &&
                  view->authentication_handler_generation_ == generation &&
                  view->on_authentication_) {
                view->on_authentication_(view.get(), json.c_str(),
                                         view->on_authentication_data_);
              }
            },
            owner_, owner_->authentication_handler_generation_,
            std::move(json)));
  }
  void Publish(std::string state,
               std::string title,
               std::set<std::string> actions,
               base::DictValue extra = {}) {
    state_ = std::move(state);
    actions_ = std::move(actions);
    action_pending_ = false;
    extra.Set("version", 1);
    extra.Set("request", base::NumberToString(request_id_));
    extra.Set("revision", ++revision_);
    extra.Set("state", state_);
    extra.Set("origin", origin_);
    extra.Set("rp_id", rp_id_);
    extra.Set("title", title);
    base::ListValue list;
    for (const auto& action : actions_)
      list.Append(action);
    extra.Set("actions", std::move(list));
    std::string json;
    base::JSONWriter::Write(extra, &json);
    // A bounded event is safe to forward through a host's private IPC. Reject
    // oversized authenticator metadata instead of silently truncating JSON.
    if (json.size() > 28 * 1024) {
      base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
          FROM_HERE,
          base::BindOnce(&WebAuthnRequest::Cancel, weak_factory_.GetWeakPtr()));
      return;
    }
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE, base::BindOnce(
                       [](base::WeakPtr<WebAuthnRequest> self, int revision,
                          std::string json) {
                         if (!self || self->revision_ != revision)
                           return;
                         if (!self->host_managed_) {
                           if (self->using_system_ && self->state_ == "waiting")
                             return;
                           auto event = base::JSONReader::ReadDict(
                               json, base::JSON_PARSE_RFC);
                           if (self->ui_ && event)
                             self->ui_->Show(*event);
                           return;
                         }
                         if (!self->HostAlive()) {
                           self->Cancel();
                           return;
                         }
                         auto* view = self->owner_.get();
                         view->on_authentication_(
                             view, json.c_str(), view->on_authentication_data_);
                       },
                       weak_factory_.GetWeakPtr(), revision_, std::move(json)));
  }
  void RunAction(int revision, base::DictValue action) {
    if (!authorized_ || revision_ != revision)
      return;
    action_pending_ = false;
    const std::string name = *action.FindString("action");
    if (name != "cancel" && !Focused()) {
      Cancel();
      return;
    }
    if (name == "cancel") {
      outcome_ = "cancelled";
      Cancel();
    } else if (name == "external") {
      ShowExternal();
    } else if (name == "system") {
      using_system_ = true;
      Publish("waiting", "Complete verification on this Mac", {"cancel"});
      DispatchSystem();
    } else if (name == "bluetooth") {
      // The permission callback can be asynchronous. Disable repeated actions.
      Publish("waiting", "Waiting for Bluetooth permission", {"cancel"});
      EnableBluetooth();
    } else if (name == "account") {
      size_t index;
      base::StringToSizeT(*action.FindString("account"), &index);
      selecting_account_ = false;
      auto response = std::move(responses_[index]);
      responses_.clear();
      auto callback = std::move(account_callback_);
      Publish("waiting", "Completing verification", {"cancel"});
      std::move(callback).Run(std::move(response));
    } else if (name == "pin") {
      const std::string pin = *action.FindString("pin");
      const auto* confirm = action.FindString("confirmation");
      const bool setting =
          pin_options_.reason != device::pin::PINEntryReason::kChallenge;
      const auto value = base::UTF8ToUTF16(pin);
      auto error = device::pin::ValidatePIN(value, pin_options_.min_pin_length);
      if (setting && (!confirm || *confirm != pin))
        error = device::pin::PINEntryError::kWrongPIN;
      if (error != device::pin::PINEntryError::kNoError) {
        pin_options_.error = error;
        PromptPIN(pin_options_);
        return;
      }
      auto callback = std::move(pin_callback_);
      Publish("waiting", "Check your security key", {"cancel"});
      std::move(callback).Run(value);
    }
  }

  void Cancel() {
    if (ui_)
      ui_->Dismiss();
    if (presentation_ == UIPresentation::kModalImmediate &&
        immediate_not_found_)
      std::move(immediate_not_found_).Run();
    else if (cancel_)
      std::move(cancel_).Run();
  }
  void ShowOptions() {
    if (!authorized_ || pin_callback_ || selecting_account_)
      return;
    if (!HasSystem()) {
      ShowExternal();
      return;
    }
    std::set<std::string> actions = {"system", "cancel"};
    if (info_.available_transports.contains(Transport::kHybrid) ||
        info_.available_transports.contains(
            Transport::kUsbHumanInterfaceDevice))
      actions.insert("external");
    Publish("choose", "Use a passkey", std::move(actions));
  }
  bool HasSystem() const {
#if BUILDFLAG(IS_MAC)
    return info_.has_icloud_keychain && SystemPasskeysAvailable();
#else
    return false;
#endif
  }
  void DispatchSystem() {
    if (!system_id_.empty()) {
      if (ui_)
        ui_->PrepareSystem();
      auto callback = dispatch_;
      const std::string id = system_id_;
      callback.Run(id);
    }
  }
  void ShowExternal() {
    if (!authorized_ || pin_callback_ || selecting_account_)
      return;
    const bool denied =
        info_.ble_status == Handler::BleStatus::kPermissionDenied;
    const bool phone =
        !denied && info_.available_transports.contains(Transport::kHybrid);
    const bool usb = info_.available_transports.contains(
        Transport::kUsbHumanInterfaceDevice);
    base::DictValue extra;
    if (!phone && !usb) {
      extra.Set("message",
                "No passkey provider is available. Choose another "
                "verification method on the website.");
      Publish("unavailable", "Choose another verification method", {"cancel"},
              std::move(extra));
      return;
    }
    const bool enable =
        phone &&
        (info_.ble_status == Handler::BleStatus::kPendingPermissionRequest ||
         (info_.ble_status == Handler::BleStatus::kOff &&
          info_.can_power_on_ble_adapter));
    extra.Set("phone", phone);
    extra.Set("security_key", usb);
    extra.Set("bluetooth", denied   ? "denied"
                           : enable ? "required"
                           : info_.ble_status == Handler::BleStatus::kOff
                               ? "off"
                               : "ready");
    if (phone)
      extra.Set("qr", qr_);
    std::set<std::string> actions = {"cancel"};
    if (enable)
      actions.insert("bluetooth");
    if (HasSystem())
      actions.insert("system");
    Publish("external", "Use a phone or security key", std::move(actions),
            std::move(extra));
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
    if (!pin_callback_ && !selecting_account_)
      Publish("waiting", "Follow the instructions on your phone", {"cancel"});
  }
  void PromptPIN(CollectPINOptions options) {
    pin_options_ = options;
    base::DictValue extra;
    extra.Set("setting",
              options.reason != device::pin::PINEntryReason::kChallenge);
    extra.Set("min_pin_length", static_cast<int>(options.min_pin_length));
    extra.Set("attempts", options.attempts);
    if (options.error != device::pin::PINEntryError::kNoError)
      extra.Set("error",
                "The PIN was not accepted. Check the PIN and "
                "confirmation before trying again.");
    Publish("pin", "Security-key PIN", {"pin", "cancel"}, std::move(extra));
  }
  content::GlobalRenderFrameHostId frame_id_;
  zebkit_view* view_;
  base::WeakPtr<zebkit_view> owner_;
  const uint64_t request_id_;
  bool host_managed_ = false, authorized_ = false, action_pending_ = false;
  int revision_ = 0;
  std::string rp_id_, state_, origin_, qr_, system_id_, outcome_ = "ended";
  std::set<std::string> actions_;
  CollectPINOptions pin_options_;
  std::vector<device::AuthenticatorGetAssertionResponse> responses_;
  base::OnceCallback<void(device::AuthenticatorGetAssertionResponse)>
      account_callback_;
  std::unique_ptr<AuthenticationUI> ui_;
  UIPresentation presentation_ = UIPresentation::kModal;
  Handler::TransportAvailabilityInfo info_;
  bool using_system_ = false, transports_enumerated_ = false,
       selecting_account_ = false;
  base::OnceClosure cancel_, immediate_not_found_;
  Handler::RequestCallback dispatch_;
  base::RepeatingClosure power_on_;
  base::RepeatingCallback<void(Handler::BlePermissionCallback)> permission_;
  base::OnceCallback<void(std::u16string)> pin_callback_;
  base::ScopedObservation<Handler, Handler::Observer> observation_{this};
  base::WeakPtrFactory<WebAuthnRequest> weak_factory_{this};
};
class WebAuthnDelegate : public content::WebAuthenticationDelegate {
 public:
  bool SupportsResidentKeys(content::RenderFrameHost* frame) override {
    return WebAuthenticationDelegate::SupportsResidentKeys(frame) ||
           ViewCanPresent(frame);
  }
  bool SupportsConditionalMediation(content::RenderFrameHost* frame) override {
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
                ViewCanPresent(frame) && SystemPasskeysAvailable(),
                std::move(callback)));
  }
  bool IsFocused(content::WebContents* contents) override {
    if (WebAuthenticationDelegate::SupportsResidentKeys(
            contents->GetPrimaryMainFrame()))
      return true;
    auto* view = ViewOf(contents);
    if (!view || !view->authentication_ui_enabled_)
      return false;
    for (const auto& [id, request] : ActiveRequests()) {
      if (request && request->BelongsTo(view) && request->Focused())
        return true;
    }
    auto* frame = contents->GetPrimaryMainFrame();
    return PageFocused(frame) &&
           (!view->offscreen_view_ || frame->HasTransientUserActivation());
  }
};
}  // namespace

void AuthenticationHandlerChanged(zebkit_view* view) {
  ++view->authentication_handler_generation_;
  // Detach delivery before canceling: the old callback data may be released
  // immediately by the host after this function returns.
  view->on_authentication_ = nullptr;
  view->on_authentication_data_ = nullptr;
  std::vector<base::WeakPtr<WebAuthnRequest>> requests;
  for (const auto& [id, request] : ActiveRequests()) {
    if (request && request->BelongsTo(view))
      requests.push_back(request);
  }
  for (const auto& request : requests) {
    if (request)
      request->DetachHost();
  }
}
void AuthenticationPolicyDisabled(zebkit_view* view) {
  std::vector<base::WeakPtr<WebAuthnRequest>> requests;
  for (const auto& [id, request] : ActiveRequests()) {
    if (request && request->BelongsTo(view))
      requests.push_back(request);
  }
  for (const auto& request : requests) {
    if (request)
      request->PolicyDisabled();
  }
}
bool AuthenticationAction(zebkit_view* view, const char* json) {
  if (std::char_traits<char>::length(json) > 16384)
    return false;
  auto parsed = base::JSONReader::Read(json, base::JSON_PARSE_RFC);
  if (!parsed || !parsed->is_dict())
    return false;
  const auto* id = parsed->GetDict().FindString("request");
  uint64_t request_id;
  if (!id || !base::StringToUint64(*id, &request_id))
    return false;
  auto it = ActiveRequests().find(request_id);
  return it != ActiveRequests().end() && it->second &&
         it->second->BelongsTo(view) &&
         it->second->QueueAction(parsed->GetDict());
}

content::WebAuthenticationDelegate* GetWebAuthnDelegate() {
  static base::NoDestructor<WebAuthnDelegate> delegate;
  return delegate.get();
}
std::unique_ptr<content::AuthenticatorRequestClientDelegate>
CreateWebAuthnRequestDelegate(content::RenderFrameHost* frame) {
  return std::make_unique<WebAuthnRequest>(frame);
}
}  // namespace zebkit
