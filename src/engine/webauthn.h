#ifndef ZEBKIT_ENGINE_WEBAUTHN_H_
#define ZEBKIT_ENGINE_WEBAUTHN_H_

#include <memory>

namespace content {
class AuthenticatorRequestClientDelegate;
class RenderFrameHost;
class WebAuthenticationDelegate;
}  // namespace content

struct zebkit_view;
namespace zebkit {
void AuthenticationHandlerChanged(zebkit_view* view);
void AuthenticationPolicyDisabled(zebkit_view* view);
bool AuthenticationAction(zebkit_view* view, const char* json);
// Shared FIDO request controller; presentation lives in platform adapters.
content::WebAuthenticationDelegate* GetWebAuthnDelegate();
std::unique_ptr<content::AuthenticatorRequestClientDelegate>
CreateWebAuthnRequestDelegate(content::RenderFrameHost* frame);
}  // namespace zebkit
#endif  // ZEBKIT_ENGINE_WEBAUTHN_H_
