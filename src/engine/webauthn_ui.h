// Platform presentation adapter; protocol and request ownership live in
// webauthn.cc.
#ifndef ZEBKIT_ENGINE_WEBAUTHN_UI_H_
#define ZEBKIT_ENGINE_WEBAUTHN_UI_H_
#include <memory>
#include "base/functional/callback.h"
#include "base/values.h"
namespace content {
class RenderFrameHost;
}
namespace device {
class FidoDiscoveryFactory;
}
namespace zebkit {
class AuthenticationUI {
 public:
  virtual ~AuthenticationUI() = default;
  virtual bool Available() const = 0;
  virtual bool Focused() const = 0;
  virtual void Configure(device::FidoDiscoveryFactory* factory) = 0;
  virtual void Show(const base::DictValue& event) = 0;
  virtual void Dismiss() = 0;
  virtual void PrepareSystem() = 0;
};
using AuthenticationResponse = base::RepeatingCallback<void(base::DictValue)>;
std::unique_ptr<AuthenticationUI> CreateAuthenticationUI(
    content::RenderFrameHost* frame,
    bool host_ui,
    AuthenticationResponse response);
bool SystemPasskeysAvailable();
}  // namespace zebkit
#endif
