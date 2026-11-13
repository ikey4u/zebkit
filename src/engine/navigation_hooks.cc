#ifdef UNSAFE_BUFFERS_BUILD
#pragma allow_unsafe_buffers
#endif
#include "zebkit/engine/navigation_hooks.h"
#include "zebkit/engine/engine.h"
#include "zebkit/engine/hook_requests.h"
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
struct Traits {
  struct Decision {
    zebkit_navigation_action action = ZEBKIT_NAVIGATION_CONTINUE;
    std::optional<GURL> replacement;
  };
  using Request = zebkit_navigation_request;
  static constexpr auto kHandler = &zebkit_view::on_navigation_;
  static constexpr auto kData = &zebkit_view::on_navigation_data_;
  static constexpr auto kResult = &zebkit_view::on_navigation_result_;
  static constexpr auto kResultData = &zebkit_view::on_navigation_result_data_;
  static constexpr auto kGeneration = &zebkit_view::navigation_result_generation_;
  static constexpr auto kTimeout = &zebkit_view::navigation_timeout_ms_;
  static Decision Refusal() { return {ZEBKIT_NAVIGATION_CANCEL, std::nullopt}; }
  // Let through, or not when the view that decides is fail-closed.
  static Decision Unanswered(zebkit_view* view) {
    zebkit_view* decider = view ? view->Decider(&zebkit_view::on_navigation_) : nullptr;
    return !decider || decider->navigation_fail_closed_ ? Refusal() : Decision();
  }
  static void Settling(base::DictValue& metadata, Decision& decision) {
    if (decision.replacement)
      metadata.Set("replacementUrl", decision.replacement->spec());
  }
  static const char* State(const Decision& decision) {
    return decision.action == ZEBKIT_NAVIGATION_CONTINUE ? "continued"
           : decision.action == ZEBKIT_NAVIGATION_CANCEL ? "cancelled"
                                                         : "redirected";
  }
};
using Requests = HookRequests<Traits>;

bool Ready() { return Requests::Get().Ready(); }

// The request of @handle's navigation at @phase, asked of the view that decides;
// 0 when too many of @view's wait, which must not let a navigation through.
uintptr_t QueueRequest(content::NavigationHandle* handle, zebkit_view* view, const char* phase,
                       Requests::Reply& reply, const char* activation_type = nullptr) {
    base::DictValue event;
    event.Set("navigationId", base::NumberToString(handle->GetNavigationId()));
    event.Set("frameId", "f" + base::NumberToString(handle->GetFrameTreeNodeId().value()));
    event.Set("phase", phase);
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
    if (activation_type) event.Set("activationType", activation_type);
    auto& requests = Requests::Get();
    return requests.Ask(view, std::move(event), reply) ? requests.last_id() : 0;
}

class PolicyThrottle : public content::NavigationThrottle {
 public:
  PolicyThrottle(content::NavigationThrottleRegistry& registry, zebkit_view* view)
      : NavigationThrottle(registry), view_(view->GetWeakPtr()) {}
  ~PolicyThrottle() override { Requests::Get().Abandon(pending_); }
  ThrottleCheckResult WillStartRequest() override { return Ask("start"); }
  ThrottleCheckResult WillRedirectRequest() override { return Ask("redirect"); }
  ThrottleCheckResult WillProcessResponse() override { return Ask("response"); }
  ThrottleCheckResult WillFailRequest() override { return Ask("failure"); }
  ThrottleCheckResult WillCommitWithoutUrlLoader() override { return Ask("commit"); }
  const char* GetNameForLogging() override { return "ZebkitNavigationPolicy"; }
  void Resolve(Traits::Decision decision) {
    pending_ = 0;
    const zebkit_navigation_action action = decision.action;
    const std::optional<GURL>& replacement = decision.replacement;
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
            if (!Ready() || !view || !view->web_contents()) return;
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
    Requests::Reply reply = base::BindOnce(&PolicyThrottle::Resolve, weak_factory_.GetWeakPtr());
    pending_ = QueueRequest(navigation_handle(), view_.get(), phase, reply);
    return pending_ ? DEFER : CANCEL_AND_IGNORE;
  }
  uintptr_t pending_ = 0;
  base::WeakPtr<zebkit_view> view_;
  base::WeakPtrFactory<PolicyThrottle> weak_factory_{this};
};

class ActivationPolicy : public content::CommitDeferringCondition {
 public:
  ActivationPolicy(content::NavigationHandle& handle, zebkit_view* view, bool prerender)
      : CommitDeferringCondition(handle), view_(view->GetWeakPtr()), prerender_(prerender) {}
  ~ActivationPolicy() override { Requests::Get().Abandon(pending_); }
  const char* TraceEventName() const override { return "ZebkitActivationPolicy"; }
  Result WillCommitNavigation(base::OnceClosure resume) override {
    if (!Ready() || !view_ || !view_->web_contents())
      return Result::kProceed;
    zebkit_view* decider = view_->Decider(&zebkit_view::on_navigation_);
    if (decider && !decider->on_navigation_)
      return Result::kProceed;
    // With an installed policy, resource exhaustion must not bypass a decision.
    Requests::Reply reply = base::BindOnce(&ActivationPolicy::Resolve, weak_factory_.GetWeakPtr());
    if (decider)
      pending_ = QueueRequest(&GetNavigationHandle(), view_.get(), "activation", reply,
                              prerender_ ? "prerender" : "back-forward-cache");
    if (!pending_) {
      view_->web_contents()->Stop();
      return Result::kCancelled;
    }
    resume_ = std::move(resume);
    return Result::kDefer;
  }
  void Resolve(Traits::Decision decision) {
    pending_ = 0;
    const zebkit_navigation_action action = decision.action;
    const std::optional<GURL>& replacement = decision.replacement;
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
            if (!Ready() || !view || !view->web_contents()) return;
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

}  // namespace

void InstallNavigationHooks() { Requests::Get().Start(); }
void StopNavigationHooks() { Requests::Get().Stop(); }
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
  Requests::Get().CancelAll(view, reason);
}
}  // namespace zebkit

extern "C" {
void zebkit_view_on_navigation(zebkit_view* view, zebkit_navigation_func function, void* data) {
  ZEBKIT_LIVE_VIEW_OR_RETURN(view);
  // What an unanswered navigation does: let through, or not when fail-closed.
  zebkit::Requests::Get().Install(view, function, data);
}
void zebkit_view_on_navigation_result(zebkit_view* view, zebkit_navigation_result_func function, void* data) {
  ZEBKIT_LIVE_VIEW_OR_RETURN(view);
  zebkit::Requests::Get().Observe(view, function, data);
}
bool zebkit_view_set_navigation_timeout(zebkit_view* view, uint32_t milliseconds) {
  ZEBKIT_LIVE_VIEW_OR_RETURN(view, false);
  return zebkit::Requests::Get().SetTimeout(view, milliseconds);
}
bool zebkit_view_set_navigation_fail_closed(zebkit_view* view, bool fail_closed) {
  if (!zebkit::Ready() || !zebkit::LiveView(view) || !view->web_contents()) return false;
  view->navigation_fail_closed_ = fail_closed; return true;
}
char* zebkit_navigation_request_info(zebkit_navigation_request* request) {
  return zebkit::Requests::Get().Info(reinterpret_cast<uintptr_t>(request));
}
bool zebkit_navigation_respond(zebkit_navigation_request* request, zebkit_navigation_action action,
                                const char* replacement_url, char** error) {
  using namespace zebkit;
  auto fail = [&](const char* message) { if (error) *error = strdup(message); return false; };
  if (!Ready()) return fail("navigation replies need the engine thread");
  const auto id = reinterpret_cast<uintptr_t>(request);
  auto& requests = Requests::Get();
  const bool was_pending = requests.Pending(id);
  if (!requests.Find(id))
    return fail(was_pending ? "navigation request timed out" : "navigation request is no longer pending");
  if (action < ZEBKIT_NAVIGATION_CONTINUE || action > ZEBKIT_NAVIGATION_REDIRECT)
    return fail("unknown navigation action");
  std::optional<GURL> replacement;
  if (action == ZEBKIT_NAVIGATION_REDIRECT) {
    if (!replacement_url || !*replacement_url || strlen(replacement_url) > 16384)
      return fail("redirect needs a replacement URL of at most 16384 bytes");
    replacement = GURL(replacement_url);
    if (!replacement->is_valid()) return fail("replacement URL is invalid");
  } else if (replacement_url) return fail("only redirect accepts a replacement URL");
  requests.Settle(id, Traits::Decision{action, std::move(replacement)}, "answered");
  return true;
}
}  // extern "C"
