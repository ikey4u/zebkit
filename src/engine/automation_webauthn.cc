// zebkit's automation, Playwright's virtual WebAuthn authenticator
// (automation_webauthn.h).

#ifdef UNSAFE_BUFFERS_BUILD
// BoringSSL's buffers are C's.
#pragma allow_unsafe_buffers
#endif

#include "zebkit/engine/automation_webauthn.h"

#include <algorithm>
#include <utility>

#include "base/base64url.h"
#include "base/json/json_writer.h"
#include "base/rand_util.h"
#include "base/strings/string_number_conversions.h"
#include "crypto/hash.h"
#include "crypto/openssl_util.h"
#include "third_party/boringssl/src/include/openssl/bytestring.h"
#include "third_party/boringssl/src/include/openssl/ec.h"
#include "third_party/boringssl/src/include/openssl/ec_key.h"
#include "third_party/boringssl/src/include/openssl/ecdsa.h"
#include "third_party/boringssl/src/include/openssl/evp.h"
#include "third_party/boringssl/src/include/openssl/mem.h"
#include "third_party/boringssl/src/include/openssl/nid.h"
#include "url/gurl.h"

namespace zebkit {

namespace {

using Bytes = std::vector<uint8_t>;

std::string ToBase64Url(base::span<const uint8_t> bytes) {
  std::string out;
  base::Base64UrlEncode(bytes, base::Base64UrlEncodePolicy::OMIT_PADDING, &out);
  return out;
}

Bytes FromBase64Url(const std::string& text) {
  std::string out;
  if (!base::Base64UrlDecode(text, base::Base64UrlDecodePolicy::IGNORE_PADDING, &out))
    return {};
  return Bytes(out.begin(), out.end());
}

std::string RandomBase64Url(size_t length) {
  Bytes bytes(length);
  base::RandBytes(bytes);
  return ToBase64Url(bytes);
}

Bytes Sha256(base::span<const uint8_t> data) {
  const auto digest = crypto::hash::Sha256(data);
  return Bytes(digest.begin(), digest.end());
}

Bytes Marshal(int (*marshal)(CBB*, const EVP_PKEY*), const EVP_PKEY* key) {
  bssl::ScopedCBB cbb;
  uint8_t* data = nullptr;
  size_t length = 0;
  if (!CBB_init(cbb.get(), 0) || !marshal(cbb.get(), key) || !CBB_finish(cbb.get(), &data, &length))
    return {};
  Bytes out(data, data + length);
  OPENSSL_free(data);
  return out;
}

// A P-256 pair: DER PKCS#8 and SPKI, and the public point's x and y.
struct KeyPair {
  Bytes private_key;
  Bytes public_key;
  Bytes x;
  Bytes y;
};

std::optional<KeyPair> NewKeyPair() {
  crypto::OpenSSLErrStackTracer clear_errors(FROM_HERE);
  bssl::UniquePtr<EC_KEY> ec(EC_KEY_new_by_curve_name(NID_X9_62_prime256v1));
  if (!ec || !EC_KEY_generate_key(ec.get()))
    return std::nullopt;
  uint8_t point[65];
  if (EC_POINT_point2oct(EC_KEY_get0_group(ec.get()), EC_KEY_get0_public_key(ec.get()),
                         POINT_CONVERSION_UNCOMPRESSED, point, sizeof(point), nullptr) != sizeof(point)) {
    return std::nullopt;
  }
  bssl::UniquePtr<EVP_PKEY> key(EVP_PKEY_new());
  if (!key || !EVP_PKEY_set1_EC_KEY(key.get(), ec.get()))
    return std::nullopt;
  KeyPair pair;
  pair.private_key = Marshal(EVP_marshal_private_key, key.get());
  pair.public_key = Marshal(EVP_marshal_public_key, key.get());
  pair.x.assign(point + 1, point + 33);
  pair.y.assign(point + 33, point + 65);
  if (pair.private_key.empty() || pair.public_key.empty())
    return std::nullopt;
  return pair;
}

// crypto.sign('sha256', data, key): an ECDSA signature, DER.
Bytes Sign(const Bytes& pkcs8, const Bytes& data) {
  crypto::OpenSSLErrStackTracer clear_errors(FROM_HERE);
  CBS cbs;
  CBS_init(&cbs, pkcs8.data(), pkcs8.size());
  bssl::UniquePtr<EVP_PKEY> key(EVP_parse_private_key(&cbs));
  const EC_KEY* ec = key ? EVP_PKEY_get0_EC_KEY(key.get()) : nullptr;
  if (!ec)
    return {};
  const Bytes digest = Sha256(data);
  Bytes signature(ECDSA_size(ec));
  unsigned int length = 0;
  if (!ECDSA_sign(0, digest.data(), digest.size(), signature.data(), &length, ec))
    return {};
  signature.resize(length);
  return signature;
}

// The minimal CBOR of credentials.ts.
void CborHead(Bytes& out, int major, uint64_t value) {
  const uint8_t m = static_cast<uint8_t>(major << 5);
  if (value < 24) {
    out.push_back(m | static_cast<uint8_t>(value));
  } else if (value < 0x100) {
    out.push_back(m | 24);
    out.push_back(static_cast<uint8_t>(value));
  } else if (value < 0x10000) {
    out.push_back(m | 25);
    out.push_back(static_cast<uint8_t>(value >> 8));
    out.push_back(static_cast<uint8_t>(value));
  } else {
    out.push_back(m | 26);
    for (int shift = 24; shift >= 0; shift -= 8)
      out.push_back(static_cast<uint8_t>(value >> shift));
  }
}
void CborInt(Bytes& out, int64_t value) {
  if (value >= 0)
    CborHead(out, 0, static_cast<uint64_t>(value));
  else
    CborHead(out, 1, static_cast<uint64_t>(-1 - value));
}
void CborBytes(Bytes& out, const Bytes& bytes) {
  CborHead(out, 2, bytes.size());
  out.insert(out.end(), bytes.begin(), bytes.end());
}
void CborText(Bytes& out, const std::string& text) {
  CborHead(out, 3, text.size());
  out.insert(out.end(), text.begin(), text.end());
}

void Append(Bytes& out, const Bytes& more) {
  out.insert(out.end(), more.begin(), more.end());
}

void AppendU32(Bytes& out, uint32_t value) {
  for (int shift = 24; shift >= 0; shift -= 8)
    out.push_back(static_cast<uint8_t>(value >> shift));
}

std::string Json(const std::string& text) {
  return base::WriteJson(base::Value(text)).value_or("\"\"");
}

// clientDataJSON, its keys in credentials.ts's order.
std::string ClientData(const std::string& type, const std::string& challenge, const std::string& origin) {
  return "{\"type\":" + Json(type) + ",\"challenge\":" + Json(challenge) + ",\"origin\":" + Json(origin) +
         ",\"crossOrigin\":false}";
}

std::string RpIdOf(const base::DictValue& request, const std::string* given) {
  if (given && !given->empty())
    return *given;
  const std::string* origin = request.FindString("origin");
  return origin ? std::string(GURL(*origin).host()) : std::string();
}

base::DictValue Failure(const std::string& name, const std::string& message) {
  base::DictValue failure;
  failure.Set("ok", false);
  failure.Set("name", name);
  failure.Set("message", message);
  return failure;
}

base::DictValue HandleCreate(std::vector<Passkey>& passkeys, const base::DictValue& request) {
  const std::string rp_id = RpIdOf(request, request.FindStringByDottedPath("rp.id"));
  const std::string* user = request.FindStringByDottedPath("user.id");
  if (const base::ListValue* excluded = request.FindList("excludeCredentials")) {
    for (const base::Value& entry : *excluded) {
      const std::string* id = entry.is_dict() ? entry.GetDict().FindString("id") : nullptr;
      if (id && std::any_of(passkeys.begin(), passkeys.end(),
                            [&](const Passkey& passkey) { return passkey.id == *id; })) {
        return Failure("InvalidStateError", "Credential excluded");
      }
    }
  }
  std::optional<KeyPair> pair = NewKeyPair();
  if (!pair)
    return Failure("NotAllowedError", "No key pair");
  Bytes credential_id(16);
  base::RandBytes(credential_id);
  Passkey passkey;
  passkey.id = ToBase64Url(credential_id);
  passkey.rp_id = rp_id;
  passkey.user_handle = user ? *user : std::string();
  passkey.private_key = ToBase64Url(pair->private_key);
  passkey.public_key = ToBase64Url(pair->public_key);
  const std::string* resident = request.FindString("residentKey");
  passkey.resident = resident && (*resident == "required" || *resident == "preferred");
  const std::string* challenge = request.FindString("challenge");
  const std::string* origin = request.FindString("origin");
  const std::string client_data =
      ClientData("webauthn.create", challenge ? *challenge : "", origin ? *origin : "");

  Bytes cose;
  CborHead(cose, 5, 5);
  CborInt(cose, 1);
  CborInt(cose, 2);  // kty: EC2
  CborInt(cose, 3);
  CborInt(cose, -7);  // alg: ES256
  CborInt(cose, -1);
  CborInt(cose, 1);  // crv: P-256
  CborInt(cose, -2);
  CborBytes(cose, pair->x);
  CborInt(cose, -3);
  CborBytes(cose, pair->y);

  Bytes auth_data = Sha256(base::as_byte_span(rp_id));
  auth_data.push_back(0x01 | 0x04 | 0x40);  // UP | UV | AT
  AppendU32(auth_data, 0);
  Append(auth_data, Bytes(16, 0));  // the all-zero AAGUID
  auth_data.push_back(static_cast<uint8_t>(credential_id.size() >> 8));
  auth_data.push_back(static_cast<uint8_t>(credential_id.size()));
  Append(auth_data, credential_id);
  Append(auth_data, cose);

  Bytes attestation;
  CborHead(attestation, 5, 3);
  CborText(attestation, "fmt");
  CborText(attestation, "none");
  CborText(attestation, "attStmt");
  CborHead(attestation, 5, 0);
  CborText(attestation, "authData");
  CborBytes(attestation, auth_data);

  base::DictValue response;
  response.Set("ok", true);
  response.Set("id", passkey.id);
  response.Set("clientDataJSON", ToBase64Url(base::as_byte_span(client_data)));
  response.Set("attestationObject", ToBase64Url(attestation));
  response.Set("authenticatorData", ToBase64Url(auth_data));
  response.Set("publicKey", passkey.public_key);
  passkeys.push_back(std::move(passkey));
  return response;
}

base::DictValue HandleGet(std::vector<Passkey>& passkeys, const base::DictValue& request) {
  const std::string rp_id = RpIdOf(request, request.FindString("rpId"));
  Passkey* candidate = nullptr;
  const base::ListValue* allowed = request.FindList("allowCredentials");
  if (allowed && !allowed->empty()) {
    for (const base::Value& entry : *allowed) {
      const std::string* id = entry.is_dict() ? entry.GetDict().FindString("id") : nullptr;
      for (Passkey& passkey : passkeys) {
        if (id && passkey.id == *id && passkey.rp_id == rp_id) {
          candidate = &passkey;
          break;
        }
      }
      if (candidate)
        break;
    }
  } else {
    for (Passkey& passkey : passkeys) {
      if (passkey.rp_id == rp_id && passkey.resident) {
        candidate = &passkey;
        break;
      }
    }
  }
  if (!candidate)
    return Failure("NotAllowedError", "No matching credential");
  const std::string* challenge = request.FindString("challenge");
  const std::string* origin = request.FindString("origin");
  const std::string client_data = ClientData("webauthn.get", challenge ? *challenge : "", origin ? *origin : "");
  candidate->sign_count += 1;
  Bytes auth_data = Sha256(base::as_byte_span(rp_id));
  auth_data.push_back(0x01 | 0x04);  // UP | UV
  AppendU32(auth_data, static_cast<uint32_t>(candidate->sign_count));
  Bytes to_sign = auth_data;
  Append(to_sign, Sha256(base::as_byte_span(client_data)));
  const Bytes signature = Sign(FromBase64Url(candidate->private_key), to_sign);
  if (signature.empty())
    return Failure("NotAllowedError", "The credential's key cannot sign");
  base::DictValue response;
  response.Set("ok", true);
  response.Set("id", candidate->id);
  response.Set("clientDataJSON", ToBase64Url(base::as_byte_span(client_data)));
  response.Set("authenticatorData", ToBase64Url(auth_data));
  response.Set("signature", ToBase64Url(signature));
  if (candidate->user_handle.empty())
    response.Set("userHandle", base::Value());
  else
    response.Set("userHandle", candidate->user_handle);
  return response;
}

}  // namespace

bool CreatePasskey(const base::DictValue& options, Passkey* passkey, std::string* error) {
  const double sign_count = options.FindDouble("signCount").value_or(0);
  if (sign_count < 0 || sign_count > 4294967295.0) {
    *error = "signCount must be between 0 and 4294967295, got " + base::NumberToString(sign_count);
    return false;
  }
  const std::string* rp_id = options.FindString("rpId");
  const std::string* id = options.FindString("id");
  const std::string* user = options.FindString("userHandle");
  const std::string* private_key = options.FindString("privateKey");
  const std::string* public_key = options.FindString("publicKey");
  passkey->rp_id = rp_id ? *rp_id : std::string();
  passkey->id = id && !id->empty() ? *id : RandomBase64Url(16);
  passkey->user_handle = user && !user->empty() ? *user : RandomBase64Url(16);
  if (private_key && !private_key->empty() && public_key && !public_key->empty()) {
    passkey->private_key = *private_key;
    passkey->public_key = *public_key;
  } else {
    std::optional<KeyPair> pair = NewKeyPair();
    if (!pair) {
      *error = "zebkit: no key pair";
      return false;
    }
    passkey->private_key = ToBase64Url(pair->private_key);
    passkey->public_key = ToBase64Url(pair->public_key);
  }
  passkey->sign_count = sign_count;
  passkey->resident = true;
  return true;
}

base::DictValue PasskeyValue(const Passkey& passkey) {
  base::DictValue value;
  value.Set("id", passkey.id);
  value.Set("rpId", passkey.rp_id);
  value.Set("userHandle", passkey.user_handle);
  value.Set("privateKey", passkey.private_key);
  value.Set("publicKey", passkey.public_key);
  value.Set("signCount", passkey.sign_count);
  return value;
}

base::DictValue PasskeyRequest(std::vector<Passkey>& passkeys, const base::DictValue& request) {
  const std::string* type = request.FindString("type");
  if (type && *type == "create")
    return HandleCreate(passkeys, request);
  if (type && *type == "get")
    return HandleGet(passkeys, request);
  return Failure("NotAllowedError", "Unknown WebAuthn request");
}

}  // namespace zebkit
