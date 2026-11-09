#include "zebkit/engine/share.h"

#include <string.h>

#include <utility>

#include "base/memory/weak_ptr.h"
#include "base/no_destructor.h"
#include "content/public/browser/document_service.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/web_contents.h"
#include "zebkit/engine/engine.h"
#include "zebkit/engine/hook_requests.h"
#include "zebkit/engine/include/zebkit/hooks.h"
#include "zebkit/engine/view.h"

namespace zebkit {

namespace {

struct ShareTraits {
  struct Decision {
    bool shared = false;
  };
  using Request = zebkit_share_request;
  static constexpr auto kHandler = &zebkit_view::on_share_;
  static constexpr auto kData = &zebkit_view::on_share_data_;
  static constexpr auto kResult = &zebkit_view::on_share_result_;
  static constexpr auto kResultData = &zebkit_view::on_share_result_data_;
  static constexpr auto kGeneration = &zebkit_view::share_generation_;
  static constexpr auto kTimeout = &zebkit_view::share_timeout_ms_;
  static Decision Refusal() { return {}; }
  static Decision Unanswered() { return {}; }
  static const char* State(const Decision& decision) { return decision.shared ? "shared" : "cancelled"; }
};
using ShareRequests = HookRequests<ShareTraits>;

class ShareService : public content::DocumentService<blink::mojom::ShareService> {
 public:
  ShareService(content::RenderFrameHost& frame, mojo::PendingReceiver<blink::mojom::ShareService> receiver)
      : DocumentService(frame, std::move(receiver)) {}

  void Share(const std::string& title, const std::string& text, const GURL& url,
             std::vector<blink::mojom::SharedFilePtr> files, ShareCallback callback) override {
    content::WebContents* contents = content::WebContents::FromRenderFrameHost(&render_frame_host());
    zebkit_view* view = contents ? ViewOf(contents) : nullptr;
    auto answer = [](ShareCallback callback, bool shared) {
      std::move(callback).Run(shared ? blink::mojom::ShareError::OK : blink::mojom::ShareError::CANCELED);
    };
    if (view && ShareRequests::HasDecider(view) && !ShareRequests::OrphanedPopup(view)) {
      base::DictValue metadata;
      metadata.Set("title", title);
      metadata.Set("text", text);
      metadata.Set("url", url.spec());
      base::ListValue names;
      for (const auto& file : files)
        names.Append(file->name.path().AsUTF8Unsafe());
      metadata.Set("files", std::move(names));
      metadata.Set("origin", origin().Serialize());
      ShareRequests::Reply reply = base::BindOnce(
          [](ShareCallback callback, ShareTraits::Decision decision) {
            std::move(callback).Run(decision.shared ? blink::mojom::ShareError::OK
                                                    : blink::mojom::ShareError::CANCELED);
          },
          std::move(callback));
      if (!ShareRequests::Get().Ask(view, std::move(metadata), reply) && reply)
        std::move(reply).Run(ShareTraits::Decision());
      return;
    }
    base::OnceCallback<void(bool)> done = base::BindOnce(answer, std::move(callback));
    if (contents && !HasNoDisplay(contents) && ShowSystemShareSheet(contents, title, text, url.spec(), done))
      return;
    std::move(done).Run(false);
  }
};

}  // namespace

void BindShareService(content::RenderFrameHost* frame, mojo::PendingReceiver<blink::mojom::ShareService> receiver) {
  new ShareService(*frame, std::move(receiver));
}

void InstallShareHooks() { ShareRequests::Get().Start(); }
void StopShareHooks() { ShareRequests::Get().Stop(); }
void CancelShareRequests(zebkit_view* view) { ShareRequests::Get().CancelAll(view, "cancelled"); }

#if !BUILDFLAG(IS_MAC)
bool ShowSystemShareSheet(content::WebContents*, const std::string&, const std::string&, const std::string&,
                          base::OnceCallback<void(bool)>&) {
  return false;
}
#endif

}  // namespace zebkit

extern "C" {
void zebkit_view_on_share(zebkit_view* view, zebkit_share_func function, void* data) {
  zebkit::ShareRequests::Get().Install(view, function, data);
}
void zebkit_view_on_share_result(zebkit_view* view, zebkit_share_result_func function, void* data) {
  zebkit::ShareRequests::Get().Observe(view, function, data);
}
bool zebkit_view_set_share_timeout(zebkit_view* view, uint32_t milliseconds) {
  return zebkit::ShareRequests::Get().SetTimeout(view, milliseconds);
}
char* zebkit_share_request_info(zebkit_share_request* request) {
  return zebkit::ShareRequests::Get().Info(reinterpret_cast<uintptr_t>(request));
}
bool zebkit_share_respond(zebkit_share_request* request, bool shared) {
  auto& requests = zebkit::ShareRequests::Get();
  if (!requests.Ready()) return false;
  const auto id = reinterpret_cast<uintptr_t>(request);
  if (!requests.Find(id)) return false;
  zebkit::ShareTraits::Decision decision;
  decision.shared = shared;
  requests.Settle(id, std::move(decision), "answered");
  return true;
}
}  // extern "C"
