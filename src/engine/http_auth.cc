#include "zebkit/engine/http_auth.h"
#include "zebkit/engine/view.h"

#include <string.h>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>
#include "base/compiler_specific.h"
#include "base/functional/bind.h"
#include "base/json/json_writer.h"
#include "base/no_destructor.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/single_thread_task_runner.h"
#include "base/time/time.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/web_contents.h"
#include "url/gurl.h"

namespace zebkit {
namespace {
class HttpAuthDelegate;
struct Pending {
  base::WeakPtr<HttpAuthDelegate> delegate;
  base::WeakPtr<zebkit_view> view;
  base::DictValue metadata;
  base::TimeTicks deadline;
};
struct Requests {
  bool active = false;
  uintptr_t next = 1;
  std::map<uintptr_t, Pending> pending;
};
Requests& Store() { static base::NoDestructor<Requests> store; return *store; }
bool Ready() {
  return content::BrowserThread::CurrentlyOn(content::BrowserThread::UI) && Store().active;
}
void Finish(uintptr_t id, const std::optional<net::AuthCredentials>& credentials,
            const char* reason, bool reply = true);

class HttpAuthDelegate : public content::LoginDelegate {
 public:
  explicit HttpAuthDelegate(LoginAuthRequiredCallback callback) : callback_(std::move(callback)) {}
  ~HttpAuthDelegate() override {
    // Destruction means the network request was cancelled. Chromium explicitly
    // requires dropping, rather than invoking, its auth callback in this case.
    if (id_) Finish(id_, std::nullopt, "cancelled", false);
  }
  base::WeakPtr<HttpAuthDelegate> GetWeakPtr() { return weak_factory_.GetWeakPtr(); }
  void SetId(uintptr_t id) { id_ = id; }
  void Reply(const std::optional<net::AuthCredentials>& credentials) {
    id_ = 0;
    auto callback = std::move(callback_);
    // The callback may synchronously destroy this delegate.
    if (callback) std::move(callback).Run(credentials);
  }
  void Cancel() { Reply(std::nullopt); }
 private:
  uintptr_t id_ = 0;
  LoginAuthRequiredCallback callback_;
  base::WeakPtrFactory<HttpAuthDelegate> weak_factory_{this};
};

void Report(base::WeakPtr<zebkit_view> view, base::DictValue metadata) {
  if (!Store().active || !view || !view->on_http_auth_result_) return;
  base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(FROM_HERE,
      base::BindOnce([](base::WeakPtr<zebkit_view> view, uint64_t generation, std::string json) {
        if (!Store().active || !view || view->http_auth_result_generation_ != generation ||
            !view->on_http_auth_result_) return;
        auto function = view->on_http_auth_result_; void* data = view->on_http_auth_result_data_;
        function(view.get(), json.c_str(), data);
      }, view, view->http_auth_result_generation_, base::WriteJson(metadata).value_or("{}")));
}
void Finish(uintptr_t id, const std::optional<net::AuthCredentials>& credentials,
            const char* reason, bool reply) {
  auto found = Store().pending.find(id);
  if (found == Store().pending.end()) return;
  auto pending = std::move(found->second);
  Store().pending.erase(found);
  pending.metadata.Set("state", credentials ? "provided" : "cancelled");
  pending.metadata.Set("reason", reason);
  pending.metadata.Set("provided", credentials.has_value());
  Report(pending.view, std::move(pending.metadata));
  if (reply && pending.delegate) pending.delegate->Reply(credentials);
}
void Check(uintptr_t id) {
  auto found = Store().pending.find(id);
  if (found == Store().pending.end()) return;
  if (!found->second.view || !found->second.view->web_contents() || !found->second.delegate)
    Finish(id, std::nullopt, "cancelled");
  else if (base::TimeTicks::Now() >= found->second.deadline)
    Finish(id, std::nullopt, "timeout");
}
void Deliver(uintptr_t id) {
  Check(id);
  auto found = Store().pending.find(id);
  if (found == Store().pending.end()) return;
  auto view = found->second.view;
  zebkit_view* decider = view ? view->Decider(&zebkit_view::on_http_auth_) : nullptr;
  if (!decider || !decider->on_http_auth_) { Finish(id, std::nullopt, "handler-changed"); return; }
  const std::string json = base::WriteJson(found->second.metadata).value_or("{}");
  auto function = decider->on_http_auth_; void* data = decider->on_http_auth_data_;
  function(view.get(), reinterpret_cast<zebkit_http_auth_request*>(id), json.c_str(), data);
}
std::string MetadataText(std::string text, bool* truncated) {
  if (text.size() > 16384) { text.resize(16384); *truncated = true; }
  // A byte limit can cut a UTF-8 sequence; never hand malformed JSON to a host.
  return base::UTF16ToUTF8(base::UTF8ToUTF16(text));
}
}  // namespace

void InstallHttpAuthHooks() { Store().active = true; }
void StopHttpAuthHooks() {
  Store().active = false;
  while (!Store().pending.empty()) Finish(Store().pending.begin()->first, std::nullopt, "shutdown");
}
void CancelHttpAuthRequests(zebkit_view* view, const char* reason) {
  std::vector<uintptr_t> ids;
  for (const auto& [id, pending] : Store().pending)
    if (pending.view.get() == view) ids.push_back(id);
  for (auto id : ids) Finish(id, std::nullopt, reason);
}
std::unique_ptr<content::LoginDelegate> CreateHttpAuthDelegate(
    const net::AuthChallengeInfo& challenge, content::WebContents* contents,
    const GURL& url, bool primary_main_frame, bool navigation, bool first_attempt,
    content::LoginDelegate::LoginAuthRequiredCallback& callback) {
  auto* view = contents ? ViewOf(contents) : nullptr;
  zebkit_view* decider = view ? view->Decider(&zebkit_view::on_http_auth_) : nullptr;
  if (!Ready() || !view || (decider && !decider->on_http_auth_)) return nullptr;
  auto delegate = std::make_unique<HttpAuthDelegate>(std::move(callback));
  auto runner = base::SingleThreadTaskRunner::GetCurrentDefault();
  if (!decider || Store().pending.size() >= 1024 || Store().next == std::numeric_limits<uintptr_t>::max()) {
    runner->PostTask(FROM_HERE, base::BindOnce(&HttpAuthDelegate::Cancel, delegate->GetWeakPtr()));
    return delegate;
  }
  const uintptr_t id = Store().next++;
  delegate->SetId(id);
  Pending pending;
  pending.delegate = delegate->GetWeakPtr(); pending.view = view->GetWeakPtr();
  pending.deadline = base::TimeTicks::Now() + base::Milliseconds(view->http_auth_timeout_ms_);
  auto& metadata = pending.metadata;
  bool truncated = false;
  GURL::Replacements redact;
  redact.ClearUsername(); redact.ClearPassword();
  metadata.Set("id", base::NumberToString(id)); metadata.Set("state", "pending");
  metadata.Set("url", MetadataText(url.ReplaceComponents(redact).spec(), &truncated));
  metadata.Set("realm", MetadataText(challenge.realm, &truncated));
  metadata.Set("scheme", MetadataText(challenge.scheme, &truncated));
  metadata.Set("challenger", challenge.challenger.Serialize());
  metadata.Set("host", challenge.challenger.host());
  metadata.Set("port", static_cast<int>(challenge.challenger.port()));
  metadata.Set("proxy", challenge.is_proxy);
  metadata.Set("mainFrame", primary_main_frame); metadata.Set("navigation", navigation);
  metadata.Set("firstAttempt", first_attempt);
  metadata.Set("metadataTruncated", truncated);
  metadata.Set("timeoutMs", static_cast<int>(view->http_auth_timeout_ms_));
  metadata.Set("deadlineUnixMs", (base::Time::Now() + base::Milliseconds(view->http_auth_timeout_ms_)).InMillisecondsFSinceUnixEpoch());
  Store().pending.emplace(id, std::move(pending));
  runner->PostTask(FROM_HERE, base::BindOnce(&Deliver, id));
  runner->PostDelayedTask(FROM_HERE, base::BindOnce(&Check, id), base::Milliseconds(view->http_auth_timeout_ms_));
  return delegate;
}
}  // namespace zebkit

extern "C" {
void zebkit_view_on_http_auth(zebkit_view* view, zebkit_http_auth_func function, void* data) {
  using namespace zebkit;
  if (!Ready() || !view || !view->web_contents()) return;
  if (view->on_http_auth_ == function && view->on_http_auth_data_ == data) return;
  auto weak = view->GetWeakPtr();
  CancelHttpAuthRequests(view, "handler-changed");
  if (!weak) return;
  view->on_http_auth_ = function; view->on_http_auth_data_ = data;
}
void zebkit_view_on_http_auth_result(zebkit_view* view, zebkit_http_auth_result_func function, void* data) {
  if (!zebkit::Ready() || !view || !view->web_contents()) return;
  ++view->http_auth_result_generation_;
  view->on_http_auth_result_ = function; view->on_http_auth_result_data_ = data;
}
bool zebkit_view_set_http_auth_timeout(zebkit_view* view, uint32_t milliseconds) {
  if (!zebkit::Ready() || !view || !view->web_contents() || !milliseconds || milliseconds > 300000) return false;
  view->http_auth_timeout_ms_ = milliseconds; return true;
}
char* zebkit_http_auth_request_info(zebkit_http_auth_request* request) {
  using namespace zebkit;
  if (!Ready()) return nullptr;
  const uintptr_t id = reinterpret_cast<uintptr_t>(request);
  Check(id);
  auto found = Store().pending.find(id);
  return found == Store().pending.end() ? nullptr
      : strdup(base::WriteJson(found->second.metadata).value_or("{}").c_str());
}
bool zebkit_http_auth_respond(zebkit_http_auth_request* request, const char* username, const char* password) {
  using namespace zebkit;
  if (!Ready()) return false;
  const uintptr_t id = reinterpret_cast<uintptr_t>(request);
  Check(id);
  if (!Store().pending.contains(id)) return false;
  if ((!username) != (!password)) return false;
  std::optional<net::AuthCredentials> credentials;
  if (username) {
    // SAFETY: the public C ABI requires valid NUL-terminated strings. Limit
    // these scans so an oversized credential is rejected without copying it.
    const size_t user_size = UNSAFE_BUFFERS(strnlen(username, 65537));
    const size_t password_size = UNSAFE_BUFFERS(strnlen(password, 65537));
    if (user_size > 65536 || password_size > 65536 ||
        !base::IsStringUTF8(std::string_view(username, user_size)) ||
        !base::IsStringUTF8(std::string_view(password, password_size))) return false;
    credentials.emplace(base::UTF8ToUTF16(std::string_view(username, user_size)),
                        base::UTF8ToUTF16(std::string_view(password, password_size)));
  }
  Check(id);
  if (!Store().pending.contains(id)) return false;
  Finish(id, credentials, "answered");
  return true;
}
}  // extern "C"
