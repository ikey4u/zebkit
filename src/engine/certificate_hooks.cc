#include "zebkit/engine/certificate_hooks.h"
#include "zebkit/engine/engine.h"
#include "zebkit/engine/hook_requests.h"
#include "zebkit/engine/view.h"
#include <stdlib.h>
#include <string.h>
#include <limits>
#include <map>
#include <memory>
#include <vector>
#include "base/functional/bind.h"
#include "base/json/json_writer.h"
#include "base/no_destructor.h"
#include "base/strings/string_number_conversions.h"
#include "base/task/single_thread_task_runner.h"
#include "base/time/time.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/web_contents.h"
#include "net/base/net_errors.h"
#include "net/cert/x509_certificate.h"
#include "net/ssl/ssl_info.h"
#include "url/gurl.h"

namespace zebkit {
namespace {
struct Traits {
  struct Decision {
    bool allow = false;
  };
  using Request = zebkit_certificate_request;
  static constexpr auto kHandler = &zebkit_view::on_certificate_;
  static constexpr auto kData = &zebkit_view::on_certificate_data_;
  static constexpr auto kResult = &zebkit_view::on_certificate_result_;
  static constexpr auto kResultData = &zebkit_view::on_certificate_result_data_;
  static constexpr auto kGeneration = &zebkit_view::certificate_result_generation_;
  static constexpr auto kTimeout = &zebkit_view::certificate_timeout_ms_;
  static Decision Refusal() { return {}; }
  static Decision Unanswered() { return {}; }
  static const char* State(const Decision& decision) { return decision.allow ? "continued" : "denied"; }
};
using Requests = HookRequests<Traits>;
bool Ready() { return Requests::Get().Ready(); }
}  // namespace
void InstallCertificateHooks() { Requests::Get().Start(); }
void StopCertificateHooks() { Requests::Get().Stop(); }
void CancelCertificateRequests(zebkit_view* view, const char* reason) {
  Requests::Get().CancelAll(view, reason);
}
void FinishCertificateNavigation(zebkit_view* view, const GURL& url) {
  auto& requests = Requests::Get();
  const std::string spec = url.spec();
  for (uintptr_t id : requests.IdsOf(view)) {
    auto* entry = requests.Find(id);
    if (!entry || !entry->metadata.FindBool("mainFrame").value_or(false)) continue;
    const std::string* asked = entry->metadata.FindString("url");
    if (asked && *asked == spec) requests.Settle(id, Traits::Refusal(), "cancelled");
  }
}
void HandleCertificateError(content::WebContents* contents, int error,
    const net::SSLInfo& info, const GURL& url, bool main_frame, bool strict,
    base::OnceCallback<void(content::CertificateRequestResultType)> reply) {
  auto* view = contents ? ViewOf(contents) : nullptr;
  Requests::Reply answer = base::BindOnce(
      [](base::OnceCallback<void(content::CertificateRequestResultType)> reply, Traits::Decision decision) {
        std::move(reply).Run(decision.allow ? content::CERTIFICATE_REQUEST_RESULT_TYPE_CONTINUE
                                            : content::CERTIFICATE_REQUEST_RESULT_TYPE_DENY);
      },
      std::move(reply));
  if (!Ready() || !view || !Requests::HasDecider(view)) {
    std::move(answer).Run(Traits::Refusal());
    return;
  }
  base::DictValue metadata;
  metadata.Set("url", url.spec()); metadata.Set("mainFrame", main_frame);
  metadata.Set("error", error); metadata.Set("errorName", net::ErrorToShortString(error));
  metadata.Set("strictEnforcement", strict); metadata.Set("canIgnore", !strict);
  metadata.Set("certStatus", static_cast<int>(info.cert_status));
  metadata.Set("knownRoot", info.is_issued_by_known_root);
  base::ListValue chain;
  bool truncated = false;
  if (info.cert) {
    metadata.Set("subject", info.cert->subject().GetDisplayName());
    metadata.Set("issuer", info.cert->issuer().GetDisplayName());
    metadata.Set("validFromUnixMs", info.cert->valid_start().InMillisecondsFSinceUnixEpoch());
    metadata.Set("validUntilUnixMs", info.cert->valid_expiry().InMillisecondsFSinceUnixEpoch());
    std::vector<std::string> encoded; info.cert->GetPEMEncodedChain(&encoded);
    size_t bytes = 0;
    for (auto& pem : encoded) {
      if (chain.size() >= 16 || bytes + pem.size() > 1024 * 1024) { truncated = true; break; }
      bytes += pem.size(); chain.Append(std::move(pem));
    }
  }
  metadata.Set("certificateChain", std::move(chain)); metadata.Set("chainTruncated", truncated);
  // At most 1024 waiting for each view, so that one page cannot use up another's.
  if (!Requests::Get().Ask(view, std::move(metadata), answer))
    std::move(answer).Run(Traits::Refusal());
}
}  // namespace zebkit
extern "C" {
void zebkit_view_on_certificate(zebkit_view* view, zebkit_certificate_func function, void* data) {
  ZEBKIT_LIVE_VIEW_OR_RETURN(view);
  zebkit::Requests::Get().Install(view, function, data);
}
void zebkit_view_on_certificate_result(zebkit_view* view, zebkit_certificate_result_func function, void* data) {
  ZEBKIT_LIVE_VIEW_OR_RETURN(view);
  zebkit::Requests::Get().Observe(view, function, data);
}
bool zebkit_view_set_certificate_timeout(zebkit_view* view, uint32_t milliseconds) {
  ZEBKIT_LIVE_VIEW_OR_RETURN(view, false);
  return zebkit::Requests::Get().SetTimeout(view, milliseconds);
}
char* zebkit_certificate_request_info(zebkit_certificate_request* request) {
  return zebkit::Requests::Get().Info(reinterpret_cast<uintptr_t>(request));
}
bool zebkit_certificate_respond(zebkit_certificate_request* request, bool allow) {
  using namespace zebkit;
  if (!Ready()) return false;
  const auto id = reinterpret_cast<uintptr_t>(request);
  auto* pending = Requests::Get().Find(id);
  if (!pending || (allow && pending->metadata.FindBool("strictEnforcement").value_or(true))) return false;
  Requests::Get().Settle(id, Traits::Decision{allow}, "answered");
  return true;
}
}
