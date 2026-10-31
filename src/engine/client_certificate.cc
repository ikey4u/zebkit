// Client certificates (<zebkit/hooks.h>): ContentBrowserClient's
// SelectClientCertificate, answered with what the application gives.

#include <stdlib.h>
#include <string.h>

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/base64.h"
#include "base/memory/scoped_refptr.h"
#include "content/public/browser/client_certificate_delegate.h"
#include "content/public/browser/web_contents.h"
#include "crypto/openssl_util.h"
#include "net/cert/asn1_util.h"
#include "net/cert/x509_certificate.h"
#include "net/cert/x509_util.h"
#include "net/ssl/openssl_private_key.h"
#include "net/ssl/ssl_cert_request_info.h"
#include "net/ssl/ssl_private_key.h"
#include "third_party/boringssl/src/include/openssl/bio.h"
#include "third_party/boringssl/src/include/openssl/evp.h"
#include "third_party/boringssl/src/include/openssl/pem.h"
#include "zebkit/engine/hook_requests.h"
#include "zebkit/engine/page_hooks.h"

namespace zebkit {
namespace {

struct Traits {
  struct Decision {
    zebkit_client_certificate_action action = ZEBKIT_CLIENT_CERTIFICATE_CANCEL;
    scoped_refptr<net::X509Certificate> certificate;
    scoped_refptr<net::SSLPrivateKey> key;
  };
  using Request = zebkit_client_certificate_request;
  static constexpr auto kHandler = &zebkit_view::on_client_certificate_;
  static constexpr auto kData = &zebkit_view::on_client_certificate_data_;
  static constexpr auto kResult = &zebkit_view::on_client_certificate_result_;
  static constexpr auto kResultData = &zebkit_view::on_client_certificate_result_data_;
  static constexpr auto kGeneration = &zebkit_view::client_certificate_generation_;
  static constexpr auto kTimeout = &zebkit_view::client_certificate_timeout_ms_;
  static Decision Refusal() { return {}; }
  static Decision Unanswered() { return {}; }
  static const char* State(const Decision& decision) {
    return decision.action == ZEBKIT_CLIENT_CERTIFICATE_PROVIDE ? "provided"
           : decision.action == ZEBKIT_CLIENT_CERTIFICATE_NONE  ? "none"
                                                                : "cancelled";
  }
};
using Requests = HookRequests<Traits>;

// Unanswered, the delegate goes unused: Chromium fails the request and, unlike
// a choice of no certificate, remembers nothing for the host.
void Answer(std::unique_ptr<content::ClientCertificateDelegate> delegate,
            Traits::Decision decision) {
  if (decision.action != ZEBKIT_CLIENT_CERTIFICATE_CANCEL)
    delegate->ContinueWithCertificate(std::move(decision.certificate), std::move(decision.key));
}

}  // namespace

void InstallClientCertificateHooks() { Requests::Get().Start(); }
void StopClientCertificateHooks() { Requests::Get().Stop(); }
void CancelClientCertificateRequests(zebkit_view* view) {
  Requests::Get().CancelAll(view, "cancelled");
}

base::OnceClosure HandleClientCertificate(
    content::WebContents* contents, const net::SSLCertRequestInfo& info,
    std::unique_ptr<content::ClientCertificateDelegate> delegate) {
  auto& requests = Requests::Get();
  zebkit_view* view = contents ? ViewOf(contents) : nullptr;
  if (Requests::OrphanedPopup(view))
    return base::OnceClosure();  // The delegate goes: the request fails.
  if (!Requests::HasDecider(view)) {
    // As Chrome with no certificates to offer.
    delegate->ContinueWithCertificate(nullptr, nullptr);
    return base::OnceClosure();
  }
  base::DictValue metadata;
  metadata.Set("host", info.host_and_port.host());
  metadata.Set("port", static_cast<int>(info.host_and_port.port()));
  metadata.Set("proxy", info.is_proxy);
  base::ListValue authorities;
  for (const std::string& name : info.cert_authorities)
    authorities.Append(base::Base64Encode(name));
  metadata.Set("certificateAuthorities", std::move(authorities));
  Requests::Reply reply = base::BindOnce(&Answer, std::move(delegate));
  if (!requests.Ask(view, std::move(metadata), reply))
    return base::OnceClosure();
  return base::BindOnce([](uintptr_t id) { Requests::Get().Abandon(id); }, requests.last_id());
}

}  // namespace zebkit

namespace {
bool ParseIdentity(const char* certificate_pem, const char* key_pem,
                   zebkit::Traits::Decision* decision, const char** problem) {
  *problem = "invalid certificate";
  net::CertificateList list = net::X509Certificate::CreateCertificateListFromBytes(
      base::as_byte_span(std::string_view(certificate_pem)),
      net::X509Certificate::FORMAT_PEM_CERT_SEQUENCE);
  if (list.empty())
    return false;
  std::vector<bssl::UniquePtr<CRYPTO_BUFFER>> intermediates;
  for (size_t i = 1; i < list.size(); ++i)
    intermediates.push_back(bssl::UpRef(list[i]->cert_buffer()));
  auto leaf = net::X509Certificate::CreateFromBuffer(bssl::UpRef(list[0]->cert_buffer()),
                                                     std::move(intermediates));
  if (!leaf)
    return false;
  *problem = "invalid private key";
  bssl::UniquePtr<BIO> bio(BIO_new_mem_buf(key_pem, strlen(key_pem)));
  bssl::UniquePtr<EVP_PKEY> key(
      bio ? PEM_read_bio_PrivateKey(bio.get(), nullptr, nullptr, nullptr) : nullptr);
  crypto::OpenSSLErrStackTracer clear_errors(FROM_HERE);
  if (!key)
    return false;
  *problem = "the private key is not the certificate's";
  std::string_view spki;
  if (!net::asn1::ExtractSPKIFromDERCert(
          net::x509_util::CryptoBufferAsStringPiece(leaf->cert_buffer()), &spki))
    return false;
  CBS cbs;
  CBS_init(&cbs, reinterpret_cast<const uint8_t*>(spki.data()), spki.size());
  bssl::UniquePtr<EVP_PKEY> public_key(EVP_parse_public_key(&cbs));
  if (!public_key || EVP_PKEY_cmp(public_key.get(), key.get()) != 1)
    return false;
  decision->certificate = std::move(leaf);
  decision->key = net::WrapOpenSSLPrivateKey(std::move(key));
  return !!decision->key;
}
}  // namespace

extern "C" {
void zebkit_view_on_client_certificate(zebkit_view* view, zebkit_client_certificate_func function, void* data) {
  zebkit::Requests::Get().Install(view, function, data);
}
void zebkit_view_on_client_certificate_result(zebkit_view* view, zebkit_client_certificate_result_func function, void* data) {
  zebkit::Requests::Get().Observe(view, function, data);
}
bool zebkit_view_set_client_certificate_timeout(zebkit_view* view, uint32_t milliseconds) {
  return zebkit::Requests::Get().SetTimeout(view, milliseconds);
}
char* zebkit_client_certificate_request_info(zebkit_client_certificate_request* request) {
  return zebkit::Requests::Get().Info(reinterpret_cast<uintptr_t>(request));
}
bool zebkit_client_certificate_respond(zebkit_client_certificate_request* request,
                                       zebkit_client_certificate_action action,
                                       const char* certificate_pem, const char* key_pem, char** error) {
  auto fail = [&](const char* message) { if (error) *error = strdup(message); return false; };
  auto& requests = zebkit::Requests::Get();
  if (!requests.Ready()) return fail("client certificate replies need the engine thread");
  const auto id = reinterpret_cast<uintptr_t>(request);
  if (!requests.Find(id)) return fail("the request is no longer pending");
  zebkit::Traits::Decision decision;
  decision.action = action;
  if (action == ZEBKIT_CLIENT_CERTIFICATE_PROVIDE) {
    if (!certificate_pem || !key_pem) return fail("a certificate needs its private key");
    const char* problem = nullptr;
    if (!ParseIdentity(certificate_pem, key_pem, &decision, &problem)) return fail(problem);
  } else if (action != ZEBKIT_CLIENT_CERTIFICATE_NONE && action != ZEBKIT_CLIENT_CERTIFICATE_CANCEL) {
    return fail("unknown action");
  } else if (certificate_pem || key_pem) {
    return fail("only PROVIDE takes a certificate");
  }
  requests.Settle(id, std::move(decision), "answered");
  return true;
}
}  // extern "C"
