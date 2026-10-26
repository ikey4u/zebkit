#ifndef ZEBKIT_ENGINE_WEBAUTHN_H_
#define ZEBKIT_ENGINE_WEBAUTHN_H_

#include <memory>

namespace content {
class AuthenticatorRequestClientDelegate;
class RenderFrameHost;
class WebAuthenticationDelegate;
}  // namespace content

namespace zebkit {
// macOS request UI and platform discoveries, without Chrome's browser layer.
content::WebAuthenticationDelegate* GetWebAuthnDelegate();
std::unique_ptr<content::AuthenticatorRequestClientDelegate>
CreateWebAuthnRequestDelegate(content::RenderFrameHost* frame);
}  // namespace zebkit
#endif  // ZEBKIT_ENGINE_WEBAUTHN_H_
