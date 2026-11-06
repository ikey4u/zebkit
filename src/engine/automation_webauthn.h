// zebkit's automation, Playwright's virtual WebAuthn authenticator
// (server/credentials.ts): passkeys a context keeps, made and used for the
// page's navigator.credentials calls (injected/src/webAuthn.ts), with
// P-256 keys and ES256 signatures.

#ifndef ZEBKIT_ENGINE_AUTOMATION_WEBAUTHN_H_
#define ZEBKIT_ENGINE_AUTOMATION_WEBAUTHN_H_

#include <stdint.h>

#include <string>
#include <vector>

#include "base/values.h"

namespace zebkit {

struct Passkey {
  std::string id;           // base64url credential id
  std::string rp_id;
  std::string user_handle;  // base64url
  std::string private_key;  // base64url DER PKCS#8
  std::string public_key;   // base64url DER SPKI
  double sign_count = 0;
  bool resident = true;
};

// credentials.ts's create(): {rpId, id, userHandle, privateKey, publicKey,
// signCount}, a key pair made when none is given; false with @error.
bool CreatePasskey(const base::DictValue& options, Passkey* passkey, std::string* error);

// toPublic(): what the client gets.
base::DictValue PasskeyValue(const Passkey& passkey);

// _handleCreate() and _handleGet(): the page's request (webAuthn.ts's wire
// format) answered from @passkeys.
base::DictValue PasskeyRequest(std::vector<Passkey>& passkeys, const base::DictValue& request);

}  // namespace zebkit

#endif  // ZEBKIT_ENGINE_AUTOMATION_WEBAUTHN_H_
