#include "zebkit/engine/certificate_hooks.h"
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
struct Pending {
  base::WeakPtr<zebkit_view> view;
  base::DictValue metadata;
  base::TimeTicks deadline;
  bool strict = false;
  bool main_frame = false;
  GURL url;
  base::OnceCallback<void(content::CertificateRequestResultType)> reply;
};
struct Requests {
  bool active = false;
  uintptr_t next = 1;
  std::map<uintptr_t, std::unique_ptr<Pending>> pending;
};
Requests& Store() { static base::NoDestructor<Requests> store; return *store; }
bool Ready() { return content::BrowserThread::CurrentlyOn(content::BrowserThread::UI) && Store().active; }
Pending* Find(uintptr_t id) {
  auto it = Store().pending.find(id);
  return it == Store().pending.end() ? nullptr : it->second.get();
}
void Report(base::WeakPtr<zebkit_view> view, base::DictValue event) {
  if (!Store().active || !view || !view->on_certificate_result_) return;
  base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(FROM_HERE, base::BindOnce(
      [](base::WeakPtr<zebkit_view> target, uint64_t generation, std::string json) {
        if (Store().active && target && target->certificate_result_generation_ == generation && target->on_certificate_result_) {
          auto function = target->on_certificate_result_; void* data = target->on_certificate_result_data_;
          function(target.get(), json.c_str(), data);
        }
      }, view, view->certificate_result_generation_, base::WriteJson(event).value_or("{}")));
}
void Finish(uintptr_t id, bool allow, const char* reason) {
  auto it = Store().pending.find(id);
  if (it == Store().pending.end()) return;
  auto request = std::move(it->second); Store().pending.erase(it);
  auto event = request->metadata.Clone();
  event.Set("state", allow ? "continued" : "denied"); event.Set("reason", reason);
  Report(request->view, std::move(event));
  std::move(request->reply).Run(allow ? content::CERTIFICATE_REQUEST_RESULT_TYPE_CONTINUE
                                    : content::CERTIFICATE_REQUEST_RESULT_TYPE_DENY);
}
void Check(uintptr_t id) {
  auto* request = Find(id);
  if (!request) return;
  if (!request->view || !request->view->web_contents()) Finish(id, false, "cancelled");
  else if (base::TimeTicks::Now() >= request->deadline) Finish(id, false, "timeout");
}
void Expire(zebkit_view* view) {
  std::vector<uintptr_t> ids;
  for (const auto& [id, request] : Store().pending) if (request->view.get() == view) ids.push_back(id);
  for (auto id : ids) Check(id);
}
void Dispatch(uintptr_t id) {
  Check(id);
  auto* request = Find(id);
  if (!request) return;
  auto weak = request->view;
  zebkit_view* decider = weak ? weak->Decider(&zebkit_view::on_certificate_) : nullptr;
  if (!decider || !decider->on_certificate_) { Finish(id, false, "handler-changed"); return; }
  const std::string json = base::WriteJson(request->metadata).value_or("{}");
  auto function = decider->on_certificate_; void* data = decider->on_certificate_data_;
  function(weak.get(), reinterpret_cast<zebkit_certificate_request*>(id), json.c_str(), data);
}
}  // namespace
void InstallCertificateHooks() { Store().active = true; }
void StopCertificateHooks() {
  Store().active = false;
  while (!Store().pending.empty()) Finish(Store().pending.begin()->first, false, "cancelled");
}
void CancelCertificateRequests(zebkit_view* view, const char* reason) {
  std::vector<uintptr_t> ids;
  for (const auto& [id, request] : Store().pending) if (request->view.get() == view) ids.push_back(id);
  for (auto id : ids) Finish(id, false, reason);
}
void FinishCertificateNavigation(zebkit_view* view, const GURL& url) {
  std::vector<uintptr_t> ids;
  for (const auto& [id, request] : Store().pending)
    if (request->view.get() == view && request->main_frame && request->url == url) ids.push_back(id);
  for (auto id : ids) Finish(id, false, "cancelled");
}
void HandleCertificateError(content::WebContents* contents, int error,
    const net::SSLInfo& info, const GURL& url, bool main_frame, bool strict,
    base::OnceCallback<void(content::CertificateRequestResultType)> reply) {
  auto* view = contents ? ViewOf(contents) : nullptr;
  zebkit_view* decider = view ? view->Decider(&zebkit_view::on_certificate_) : nullptr;
  if (!Ready() || !decider || !decider->on_certificate_ || Store().pending.size() >= 1024 ||
      Store().next == std::numeric_limits<uintptr_t>::max()) {
    std::move(reply).Run(content::CERTIFICATE_REQUEST_RESULT_TYPE_DENY); return;
  }
  const uintptr_t id = Store().next++;
  auto request = std::make_unique<Pending>();
  request->view = view->GetWeakPtr(); request->strict = strict; request->reply = std::move(reply);
  request->main_frame = main_frame; request->url = url;
  request->deadline = base::TimeTicks::Now() + base::Milliseconds(view->certificate_timeout_ms_);
  auto& metadata = request->metadata;
  metadata.Set("id", base::NumberToString(id)); metadata.Set("state", "pending");
  metadata.Set("url", url.spec()); metadata.Set("mainFrame", main_frame);
  metadata.Set("error", error); metadata.Set("errorName", net::ErrorToShortString(error));
  metadata.Set("strictEnforcement", strict); metadata.Set("canIgnore", !strict);
  metadata.Set("timeoutMs", static_cast<int>(view->certificate_timeout_ms_));
  metadata.Set("deadlineUnixMs", (base::Time::Now() + base::Milliseconds(view->certificate_timeout_ms_)).InMillisecondsFSinceUnixEpoch());
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
  Store().pending.emplace(id, std::move(request));
  auto runner = base::SingleThreadTaskRunner::GetCurrentDefault();
  runner->PostTask(FROM_HERE, base::BindOnce(&Dispatch, id));
  runner->PostDelayedTask(FROM_HERE, base::BindOnce(&Check, id), base::Milliseconds(view->certificate_timeout_ms_));
}
}  // namespace zebkit
extern "C" {
void zebkit_view_on_certificate(zebkit_view* view, zebkit_certificate_func function, void* data) {
  if (!zebkit::Ready() || !view || !view->web_contents()) return;
  if (view->on_certificate_ == function && view->on_certificate_data_ == data) return;
  auto weak = view->GetWeakPtr();
  zebkit::CancelCertificateRequests(view, "handler-changed");
  if (weak) { weak->on_certificate_ = function; weak->on_certificate_data_ = data; }
}
void zebkit_view_on_certificate_result(zebkit_view* view, zebkit_certificate_result_func function, void* data) {
  if (!zebkit::Ready() || !view || !view->web_contents()) return;
  ++view->certificate_result_generation_;
  view->on_certificate_result_ = function; view->on_certificate_result_data_ = data;
}
bool zebkit_view_set_certificate_timeout(zebkit_view* view, uint32_t milliseconds) {
  if (!zebkit::Ready() || !view || !view->web_contents() || !milliseconds || milliseconds > 300000) return false;
  auto weak = view->GetWeakPtr(); zebkit::Expire(view);
  if (!weak) return false;
  weak->certificate_timeout_ms_ = milliseconds; return true;
}
char* zebkit_certificate_request_info(zebkit_certificate_request* request) {
  if (!zebkit::Ready()) return nullptr;
  const auto id = reinterpret_cast<uintptr_t>(request); zebkit::Check(id);
  auto* pending = zebkit::Find(id);
  return pending ? strdup(base::WriteJson(pending->metadata).value_or("{}").c_str()) : nullptr;
}
bool zebkit_certificate_respond(zebkit_certificate_request* request, bool allow) {
  if (!zebkit::Ready()) return false;
  const auto id = reinterpret_cast<uintptr_t>(request); zebkit::Check(id);
  auto* pending = zebkit::Find(id);
  if (!pending || (allow && pending->strict)) return false;
  zebkit::Finish(id, allow, "answered"); return true;
}
}
