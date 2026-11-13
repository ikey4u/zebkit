#include "zebkit/engine/http_auth.h"
#include "zebkit/engine/engine.h"
#include "zebkit/engine/hook_requests.h"
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
struct Traits {
  struct Decision {
    std::optional<net::AuthCredentials> credentials;
  };
  using Request = zebkit_http_auth_request;
  static constexpr auto kHandler = &zebkit_view::on_http_auth_;
  static constexpr auto kData = &zebkit_view::on_http_auth_data_;
  static constexpr auto kResult = &zebkit_view::on_http_auth_result_;
  static constexpr auto kResultData = &zebkit_view::on_http_auth_result_data_;
  static constexpr auto kGeneration = &zebkit_view::http_auth_result_generation_;
  static constexpr auto kTimeout = &zebkit_view::http_auth_timeout_ms_;
  static Decision Refusal() { return {}; }
  static Decision Unanswered() { return {}; }
  static const char* State(const Decision& decision) { return decision.credentials ? "provided" : "cancelled"; }
};
using Requests = HookRequests<Traits>;
bool Ready() { return Requests::Get().Ready(); }

class HttpAuthDelegate : public content::LoginDelegate {
 public:
  explicit HttpAuthDelegate(LoginAuthRequiredCallback callback) : callback_(std::move(callback)) {}
  ~HttpAuthDelegate() override {
    // Destruction means the network request was cancelled. Chromium explicitly
    // requires dropping, rather than invoking, its auth callback in this case.
    Requests::Get().Abandon(id_);
  }
  base::WeakPtr<HttpAuthDelegate> GetWeakPtr() { return weak_factory_.GetWeakPtr(); }
  void SetId(uintptr_t id) { id_ = id; }
  void Answer(Traits::Decision decision) { Reply(decision.credentials); }
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

std::string MetadataText(std::string text, bool* truncated) {
  if (text.size() > 16384) { text.resize(16384); *truncated = true; }
  // A byte limit can cut a UTF-8 sequence; never hand malformed JSON to a host.
  return base::UTF16ToUTF8(base::UTF8ToUTF16(text));
}
}  // namespace

void InstallHttpAuthHooks() { Requests::Get().Start(); }
void StopHttpAuthHooks() { Requests::Get().Stop(); }
void CancelHttpAuthRequests(zebkit_view* view, const char* reason) {
  Requests::Get().CancelAll(view, reason);
}
std::unique_ptr<content::LoginDelegate> CreateHttpAuthDelegate(
    const net::AuthChallengeInfo& challenge, content::WebContents* contents,
    const GURL& url, bool primary_main_frame, bool navigation, bool first_attempt,
    content::LoginDelegate::LoginAuthRequiredCallback& callback) {
  auto* view = contents ? ViewOf(contents) : nullptr;
  zebkit_view* decider = view ? view->Decider(&zebkit_view::on_http_auth_) : nullptr;
  if (!Ready() || !view || (decider && !decider->on_http_auth_)) return nullptr;
  auto delegate = std::make_unique<HttpAuthDelegate>(std::move(callback));
  auto cancel = [&] {
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE, base::BindOnce(&HttpAuthDelegate::Cancel, delegate->GetWeakPtr()));
    return std::move(delegate);
  };
  if (!decider)
    return cancel();
  base::DictValue metadata;
  bool truncated = false;
  GURL::Replacements redact;
  redact.ClearUsername(); redact.ClearPassword();
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
  // At most 1024 waiting for each view, so that one page cannot use up another's.
  Requests::Reply reply = base::BindOnce(&HttpAuthDelegate::Answer, delegate->GetWeakPtr());
  auto& requests = Requests::Get();
  if (!requests.Ask(view, std::move(metadata), reply))
    return cancel();
  delegate->SetId(requests.last_id());
  return delegate;
}
}  // namespace zebkit

extern "C" {
void zebkit_view_on_http_auth(zebkit_view* view, zebkit_http_auth_func function, void* data) {
  ZEBKIT_LIVE_VIEW_OR_RETURN(view);
  zebkit::Requests::Get().Install(view, function, data);
}
void zebkit_view_on_http_auth_result(zebkit_view* view, zebkit_http_auth_result_func function, void* data) {
  ZEBKIT_LIVE_VIEW_OR_RETURN(view);
  zebkit::Requests::Get().Observe(view, function, data);
}
bool zebkit_view_set_http_auth_timeout(zebkit_view* view, uint32_t milliseconds) {
  ZEBKIT_LIVE_VIEW_OR_RETURN(view, false);
  return zebkit::Requests::Get().SetTimeout(view, milliseconds);
}
char* zebkit_http_auth_request_info(zebkit_http_auth_request* request) {
  return zebkit::Requests::Get().Info(reinterpret_cast<uintptr_t>(request));
}
bool zebkit_http_auth_respond(zebkit_http_auth_request* request, const char* username, const char* password) {
  using namespace zebkit;
  if (!Ready()) return false;
  const uintptr_t id = reinterpret_cast<uintptr_t>(request);
  auto& requests = Requests::Get();
  if (!requests.Find(id)) return false;
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
  if (!requests.Find(id)) return false;
  requests.Settle(id, Traits::Decision{std::move(credentials)}, "answered");
  return true;
}
}  // extern "C"
