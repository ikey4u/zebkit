#ifdef UNSAFE_BUFFERS_BUILD
#pragma allow_unsafe_buffers
#endif
#include "zebkit/engine/navigation_hooks.h"
#include "zebkit/engine/engine.h"
#include "zebkit/engine/view.h"

#include <stdlib.h>
#include <string.h>
#include <limits>
#include <map>
#include <optional>
#include <utility>
#include <vector>
#include "base/functional/bind.h"
#include "base/json/json_writer.h"
#include "base/no_destructor.h"
#include "base/strings/string_number_conversions.h"
#include "base/task/single_thread_task_runner.h"
#include "base/time/time.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/commit_deferring_condition.h"
#include "content/public/browser/navigation_handle.h"
#include "content/public/browser/navigation_throttle.h"
#include "content/public/browser/page_navigator.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/web_contents.h"
#include "net/http/http_response_headers.h"
#include "third_party/blink/public/mojom/loader/referrer.mojom.h"
#include "ui/base/page_transition_types.h"
#include "ui/base/window_open_disposition.h"
#include "url/gurl.h"

namespace zebkit {
namespace {
struct Pending {
  base::OnceCallback<void(zebkit_navigation_action, std::optional<GURL>)> resolve;
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
bool Ready() { return content::BrowserThread::CurrentlyOn(content::BrowserThread::UI) && Store().active; }
const char* ActionName(zebkit_navigation_action action) {
  return action == ZEBKIT_NAVIGATION_CONTINUE ? "continue" : action == ZEBKIT_NAVIGATION_CANCEL ? "cancel" : "redirect";
}
void Report(base::WeakPtr<zebkit_view> view, base::DictValue event) {
  if (!Store().active || !view || !view->on_navigation_result_) return;
  auto json = base::WriteJson(event).value_or("{}");
  base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(FROM_HERE,
      base::BindOnce([](base::WeakPtr<zebkit_view> view, std::string json) {
        if (Store().active && view && view->on_navigation_result_)
          view->on_navigation_result_(view.get(), json.c_str(), view->on_navigation_result_data_);
      }, view, std::move(json)));
}
void Finish(uintptr_t id, zebkit_navigation_action action, const char* reason,
            std::optional<GURL> replacement = std::nullopt);
void Deliver(uintptr_t id);
void Timeout(uintptr_t id);
// One view's page cannot use up another view's requests.
bool Saturated(zebkit_view* view) {
  if (Store().next == std::numeric_limits<uintptr_t>::max()) return true;
  size_t count = 0;
  for (const auto& [id, request] : Store().pending) count += request.view.get() == view;
  return count >= 1024;
}
zebkit_navigation_action Unanswered(zebkit_view* view) {
  zebkit_view* decider = view ? view->Decider(&zebkit_view::on_navigation_) : nullptr;
  return !decider || decider->navigation_fail_closed_ ? ZEBKIT_NAVIGATION_CANCEL : ZEBKIT_NAVIGATION_CONTINUE;
}

uintptr_t QueueRequest(content::NavigationHandle* handle,
                       base::WeakPtr<zebkit_view> view, const char* phase,
                       base::OnceCallback<void(zebkit_navigation_action, std::optional<GURL>)> resolve,
                       const char* activation_type = nullptr) {
    const auto id = Store().next++;
    Pending request;
    request.view = view;
    request.deadline = base::TimeTicks::Now() + base::Milliseconds(view->navigation_timeout_ms_);
    auto& event = request.metadata;
    event.Set("id", base::NumberToString(id));
    event.Set("navigationId", base::NumberToString(handle->GetNavigationId()));
    event.Set("frameId", "f" + base::NumberToString(handle->GetFrameTreeNodeId().value()));
    event.Set("phase", phase);
    event.Set("state", "pending");
    event.Set("url", handle->GetURL().spec());
    event.Set("method", handle->GetRequestMethod());
    event.Set("mainFrame", handle->IsInMainFrame());
    event.Set("userGesture", handle->HasUserGesture());
    event.Set("rendererInitiated", handle->IsRendererInitiated());
    event.Set("sameDocument", handle->IsSameDocument());
    // Prerender conditions run before navigation startup initializes the
    // sanitized referrer. GetReferrer() dereferences it unconditionally.
    event.Set("referrer", activation_type ? "" : handle->GetReferrer().url.spec());
    event.Set("referrerAvailable", !activation_type);
    event.Set("netError", static_cast<int>(handle->GetNetErrorCode()));
    event.Set("timeoutMs", static_cast<int>(view->navigation_timeout_ms_));
    event.Set("deadlineUnixMs", static_cast<double>((base::Time::Now() - base::Time::UnixEpoch()).InMilliseconds()) + view->navigation_timeout_ms_);
    base::ListValue redirects;
    for (const auto& url : handle->GetRedirectChain()) redirects.Append(url.spec());
    event.Set("redirectChain", std::move(redirects));
    base::ListValue headers;
    bool truncated = false;
    int status = 0;
    // A start/commit event has no response headers yet.
    if (strcmp(phase, "start") && strcmp(phase, "commit")) {
      if (const auto* response = handle->GetResponseHeaders()) {
        status = response->response_code();
        size_t iter = 0, bytes = 0;
        std::string name, value;
        while (response->EnumerateHeaderLines(&iter, &name, &value)) {
          if (headers.size() >= 256 || bytes + name.size() + value.size() > 65536) { truncated = true; break; }
          bytes += name.size() + value.size();
          base::DictValue pair;
          pair.Set("name", name); pair.Set("value", value);
          headers.Append(std::move(pair));
        }
      }
    }
    event.Set("responseStatus", status);
    event.Set("responseHeaders", std::move(headers));
    event.Set("headersTruncated", truncated);
    request.resolve = std::move(resolve);
    if (activation_type) event.Set("activationType", activation_type);
    Store().pending.emplace(id, std::move(request));
    auto runner = base::SingleThreadTaskRunner::GetCurrentDefault();
    runner->PostTask(FROM_HERE, base::BindOnce(&Deliver, id));
    runner->PostDelayedTask(FROM_HERE, base::BindOnce(&Timeout, id),
                           base::Milliseconds(view->navigation_timeout_ms_));
    return id;
}
void Abandon(uintptr_t id) {
    auto it = Store().pending.find(id);
    if (it == Store().pending.end()) return;
    auto request = std::move(it->second);
    Store().pending.erase(it);
    request.metadata.Set("state", "settled");
    request.metadata.Set("action", "cancel");
    request.metadata.Set("reason", "cancelled");
    Report(request.view, std::move(request.metadata));
}

class PolicyThrottle : public content::NavigationThrottle {
 public:
  PolicyThrottle(content::NavigationThrottleRegistry& registry, zebkit_view* view)
      : NavigationThrottle(registry), view_(view->GetWeakPtr()) {}
  ~PolicyThrottle() override { Abandon(pending_); }
  ThrottleCheckResult WillStartRequest() override { return Ask("start"); }
  ThrottleCheckResult WillRedirectRequest() override { return Ask("redirect"); }
  ThrottleCheckResult WillProcessResponse() override { return Ask("response"); }
  ThrottleCheckResult WillFailRequest() override { return Ask("failure"); }
  ThrottleCheckResult WillCommitWithoutUrlLoader() override { return Ask("commit"); }
  const char* GetNameForLogging() override { return "ZebkitNavigationPolicy"; }
  void Resolve(zebkit_navigation_action action, std::optional<GURL> replacement) {
    pending_ = 0;
    if (action == ZEBKIT_NAVIGATION_REDIRECT && replacement && view_) {
      auto frame_id = navigation_handle()->GetFrameTreeNodeId();
      auto params = content::OpenURLParams::CreateBrowserInitiated(
          *replacement, WindowOpenDisposition::CURRENT_TAB,
          navigation_handle()->IsInMainFrame() ? ui::PAGE_TRANSITION_AUTO_TOPLEVEL : ui::PAGE_TRANSITION_AUTO_SUBFRAME,
          content::Referrer(), false, frame_id);
      // Cancellation can destroy this throttle. A replacement runs afterwards
      // and is discarded if its original frame no longer exists.
      base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(FROM_HERE,
          base::BindOnce([](base::WeakPtr<zebkit_view> view, content::FrameTreeNodeId frame_id,
                            content::OpenURLParams params) {
            if (!Store().active || !view || !view->web_contents()) return;
            bool found = false;
            view->web_contents()->ForEachRenderFrameHost([&](content::RenderFrameHost* frame) {
              if (frame->IsActive() && frame->GetFrameTreeNodeId() == frame_id) found = true;
            });
            if (found) view->web_contents()->OpenURL(params, {});
          }, view_, frame_id, std::move(params)));
    }
    // Both calls can synchronously destroy this object. Do not access it afterwards.
    if (action == ZEBKIT_NAVIGATION_CONTINUE) Resume();
    else CancelDeferredNavigation(CANCEL_AND_IGNORE);
  }
 private:
  ThrottleCheckResult Ask(const char* phase) {
    if (!Ready() || !view_ || !view_->web_contents()) return PROCEED;
    zebkit_view* decider = view_->Decider(&zebkit_view::on_navigation_);
    if (!decider) return CANCEL_AND_IGNORE;
    if (!decider->on_navigation_) return PROCEED;
    // With an installed policy, resource exhaustion must not bypass a decision.
    if (Saturated(view_.get())) return CANCEL_AND_IGNORE;
    pending_ = QueueRequest(navigation_handle(), view_, phase,
        base::BindOnce(&PolicyThrottle::Resolve, weak_factory_.GetWeakPtr()));
    return DEFER;
  }
  uintptr_t pending_ = 0;
  base::WeakPtr<zebkit_view> view_;
  base::WeakPtrFactory<PolicyThrottle> weak_factory_{this};
};

class ActivationPolicy : public content::CommitDeferringCondition {
 public:
  ActivationPolicy(content::NavigationHandle& handle, zebkit_view* view, bool prerender)
      : CommitDeferringCondition(handle), view_(view->GetWeakPtr()), prerender_(prerender) {}
  ~ActivationPolicy() override { Abandon(pending_); }
  const char* TraceEventName() const override { return "ZebkitActivationPolicy"; }
  Result WillCommitNavigation(base::OnceClosure resume) override {
    if (!Ready() || !view_ || !view_->web_contents())
      return Result::kProceed;
    zebkit_view* decider = view_->Decider(&zebkit_view::on_navigation_);
    if (decider && !decider->on_navigation_)
      return Result::kProceed;
    // With an installed policy, resource exhaustion must not bypass a decision.
    if (!decider || Saturated(view_.get())) {
      view_->web_contents()->Stop();
      return Result::kCancelled;
    }
    resume_ = std::move(resume);
    pending_ = QueueRequest(&GetNavigationHandle(), view_, "activation",
        base::BindOnce(&ActivationPolicy::Resolve, weak_factory_.GetWeakPtr()),
        prerender_ ? "prerender" : "back-forward-cache");
    return Result::kDefer;
  }
  void Resolve(zebkit_navigation_action action, std::optional<GURL> replacement) {
    pending_ = 0;
    if (action == ZEBKIT_NAVIGATION_CONTINUE) {
      std::move(resume_).Run();
      return;
    }
    auto view = view_;
    if (!view || !view->web_contents()) return;
    if (action == ZEBKIT_NAVIGATION_REDIRECT && replacement) {
      // Queue replacement before cancellation, which can destroy this condition.
      base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(FROM_HERE,
          base::BindOnce([](base::WeakPtr<zebkit_view> view, GURL url) {
            if (!Store().active || !view || !view->web_contents()) return;
            auto params = content::OpenURLParams::CreateBrowserInitiated(
                url, WindowOpenDisposition::CURRENT_TAB,
                ui::PAGE_TRANSITION_AUTO_TOPLEVEL, content::Referrer(), false);
            view->web_contents()->OpenURL(params, {});
          }, view, *replacement));
    }
    view->web_contents()->Stop();
  }
 private:
  uintptr_t pending_ = 0;
  base::WeakPtr<zebkit_view> view_;
  const bool prerender_;
  base::OnceClosure resume_;
  base::WeakPtrFactory<ActivationPolicy> weak_factory_{this};
};

void Finish(uintptr_t id, zebkit_navigation_action action, const char* reason, std::optional<GURL> replacement) {
  auto it = Store().pending.find(id);
  if (it == Store().pending.end()) return;
  auto request = std::move(it->second);
  Store().pending.erase(it);
  request.metadata.Set("state", "settled");
  request.metadata.Set("action", ActionName(action));
  request.metadata.Set("reason", reason);
  if (replacement) request.metadata.Set("replacementUrl", replacement->spec());
  Report(request.view, std::move(request.metadata));
  if (request.resolve && !request.resolve.IsCancelled())
    std::move(request.resolve).Run(action, std::move(replacement));
}
void Timeout(uintptr_t id) {
  auto it = Store().pending.find(id);
  if (it == Store().pending.end()) return;
  if (base::TimeTicks::Now() < it->second.deadline) return;
  Finish(id, Unanswered(it->second.view.get()), "timeout");
}
void Deliver(uintptr_t id) {
  auto it = Store().pending.find(id);
  if (it == Store().pending.end()) return;
  if (!it->second.view || !it->second.view->web_contents() || it->second.resolve.IsCancelled()) {
    Finish(id, ZEBKIT_NAVIGATION_CANCEL, "cancelled"); return;
  }
  if (base::TimeTicks::Now() >= it->second.deadline) { Timeout(id); return; }
  auto view = it->second.view;
  zebkit_view* decider = view->Decider(&zebkit_view::on_navigation_);
  // As HookRequests says it: a popup whose opener has gone, or no function.
  if (!decider) { Finish(id, ZEBKIT_NAVIGATION_CANCEL, "opener-closed"); return; }
  if (!decider->on_navigation_) { Finish(id, Unanswered(view.get()), "handler-changed"); return; }
  const auto json = base::WriteJson(it->second.metadata).value_or("{}");
  auto function = decider->on_navigation_; void* data = decider->on_navigation_data_;
  function(view.get(), reinterpret_cast<zebkit_navigation_request*>(id), json.c_str(), data);
}
void Expire(zebkit_view* view) {
  std::vector<uintptr_t> ids;
  const auto now = base::TimeTicks::Now();
  for (const auto& [id, request] : Store().pending)
    if (request.view.get() == view && now >= request.deadline) ids.push_back(id);
  for (auto id : ids) Timeout(id);
}
}  // namespace

void InstallNavigationHooks() { Store().active = true; }
void StopNavigationHooks() {
  Store().active = false;
  std::vector<uintptr_t> ids;
  for (const auto& [id, request] : Store().pending) ids.push_back(id);
  for (auto id : ids) Finish(id, ZEBKIT_NAVIGATION_CANCEL, "shutdown");
}
void AddNavigationPolicyThrottle(content::NavigationThrottleRegistry& registry) {
  if (auto* view = ViewOf(registry.GetNavigationHandle().GetWebContents()))
    registry.AddThrottle(std::make_unique<PolicyThrottle>(registry, view));
}
std::unique_ptr<content::CommitDeferringCondition> CreateNavigationActivationPolicy(
    content::NavigationHandle* handle, bool prerender) {
  if (!prerender && !handle->IsServedFromBackForwardCache()) return nullptr;
  auto* view = ViewOf(handle->GetWebContents());
  if (!view) return nullptr;
  return std::make_unique<ActivationPolicy>(*handle, view, prerender);
}
void CancelNavigationRequests(zebkit_view* view, const char* reason) {
  std::vector<uintptr_t> ids;
  for (const auto& [id, request] : Store().pending)
    if (request.view.get() == view) ids.push_back(id);
  for (auto id : ids) Finish(id, ZEBKIT_NAVIGATION_CANCEL, reason);
}
}  // namespace zebkit

extern "C" {
void zebkit_view_on_navigation(zebkit_view* view, zebkit_navigation_func function, void* data) {
  using namespace zebkit;
  if (!Ready() || !zebkit::LiveView(view) || !view->web_contents()) return;
  if (view->on_navigation_ == function && view->on_navigation_data_ == data) return;
  auto weak = view->GetWeakPtr();
  Expire(view);
  if (!weak) return;
  view->on_navigation_ = function; view->on_navigation_data_ = data;
  std::vector<uintptr_t> ids;
  for (const auto& [id, request] : Store().pending) if (request.view.get() == view) ids.push_back(id);
  // What an unanswered navigation does: let through, or not when fail-closed.
  for (auto id : ids) Finish(id, Unanswered(view), "handler-changed");
}
void zebkit_view_on_navigation_result(zebkit_view* view, zebkit_navigation_result_func function, void* data) {
  if (!zebkit::Ready() || !zebkit::LiveView(view) || !view->web_contents()) return;
  view->on_navigation_result_ = function; view->on_navigation_result_data_ = data;
}
bool zebkit_view_set_navigation_timeout(zebkit_view* view, uint32_t milliseconds) {
  if (!zebkit::Ready() || !zebkit::LiveView(view) || !view->web_contents() || !milliseconds || milliseconds > 300000) return false;
  view->navigation_timeout_ms_ = milliseconds; return true;
}
bool zebkit_view_set_navigation_fail_closed(zebkit_view* view, bool fail_closed) {
  if (!zebkit::Ready() || !zebkit::LiveView(view) || !view->web_contents()) return false;
  view->navigation_fail_closed_ = fail_closed; return true;
}
char* zebkit_navigation_request_info(zebkit_navigation_request* request) {
  using namespace zebkit;
  if (!Ready()) return nullptr;
  const auto id = reinterpret_cast<uintptr_t>(request);
  auto it = Store().pending.find(id);
  if (it == Store().pending.end()) return nullptr;
  if (base::TimeTicks::Now() >= it->second.deadline) { Timeout(id); return nullptr; }
  return strdup(base::WriteJson(it->second.metadata).value_or("{}").c_str());
}
bool zebkit_navigation_respond(zebkit_navigation_request* request, zebkit_navigation_action action,
                                const char* replacement_url, char** error) {
  using namespace zebkit;
  auto fail = [&](const char* message) { if (error) *error = strdup(message); return false; };
  if (!Ready()) return fail("navigation replies need the engine thread");
  const auto id = reinterpret_cast<uintptr_t>(request);
  auto it = Store().pending.find(id);
  if (it == Store().pending.end()) return fail("navigation request is no longer pending");
  if (base::TimeTicks::Now() >= it->second.deadline) { Timeout(id); return fail("navigation request timed out"); }
  if (action < ZEBKIT_NAVIGATION_CONTINUE || action > ZEBKIT_NAVIGATION_REDIRECT)
    return fail("unknown navigation action");
  std::optional<GURL> replacement;
  if (action == ZEBKIT_NAVIGATION_REDIRECT) {
    if (!replacement_url || !*replacement_url || strlen(replacement_url) > 16384)
      return fail("redirect needs a replacement URL of at most 16384 bytes");
    replacement = GURL(replacement_url);
    if (!replacement->is_valid()) return fail("replacement URL is invalid");
  } else if (replacement_url) return fail("only redirect accepts a replacement URL");
  Finish(id, action, "answered", std::move(replacement));
  return true;
}
}  // extern "C"
