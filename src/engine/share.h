// The page's navigator.share() (docs/SYSTEM_INTEGRATION.md, 12): the
// application's to answer (zebkit_view_on_share), or the system's share sheet
// where it has one (macOS); cancelled otherwise, as Linux has none.

#ifndef ZEBKIT_ENGINE_SHARE_H_
#define ZEBKIT_ENGINE_SHARE_H_

#include <string>

#include "base/functional/callback.h"
#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "third_party/blink/public/mojom/webshare/webshare.mojom.h"

struct zebkit_view;

namespace content {
class RenderFrameHost;
class WebContents;
}

namespace zebkit {

void BindShareService(content::RenderFrameHost* frame, mojo::PendingReceiver<blink::mojom::ShareService> receiver);
void InstallShareHooks();
void StopShareHooks();
void CancelShareRequests(zebkit_view* view);

// share_mac.mm; false elsewhere: the system's sheet, answered later with
// whether something was shared.
bool ShowSystemShareSheet(content::WebContents* contents, const std::string& title, const std::string& text,
                          const std::string& url, base::OnceCallback<void(bool shared)>& done);

}  // namespace zebkit

#endif  // ZEBKIT_ENGINE_SHARE_H_
