// External protocols, file choosers, media, keys, context menus, fullscreen
// (page_hooks.cc); downloads (download_hooks.cc); client certificates
// (client_certificate.cc). docs/ROADMAP_HOOKS.md.

#ifndef ZEBKIT_ENGINE_PAGE_HOOKS_H_
#define ZEBKIT_ENGINE_PAGE_HOOKS_H_

#include <memory>
#include <optional>
#include <vector>

#include "base/functional/callback_forward.h"
#include "base/memory/scoped_refptr.h"
#include "content/public/browser/frame_tree_node_id.h"
#include "services/network/public/mojom/web_sandbox_flags.mojom-shared.h"
#include "third_party/blink/public/mojom/choosers/file_chooser.mojom-forward.h"
#include "url/origin.h"
#include "zebkit/engine/view.h"

class GURL;
namespace content {
class ClientCertificateDelegate;
class FileSelectListener;
class RenderFrameHost;
class WebContents;
}  // namespace content
namespace net {
class ClientCertIdentity;
class SSLCertRequestInfo;
using ClientCertIdentityList = std::vector<std::unique_ptr<ClientCertIdentity>>;
}

namespace zebkit {

void InstallPageHooks();
void StopPageHooks();
void CancelPageHookRequests(zebkit_view* view);
// Ends what @frame's document was asked; nullptr: what documents gone were.
void PageHooksFrameGone(zebkit_view* view, content::RenderFrameHost* frame);
void TellExternalProtocol(const GURL& url, content::WebContents* contents,
                          content::FrameTreeNodeId frame, bool main_frame, bool fenced,
                          network::mojom::WebSandboxFlags sandbox, bool user_gesture,
                          const std::optional<url::Origin>& initiator,
                          content::RenderFrameHost* initiator_document);
bool HandleFileChooserHook(content::RenderFrameHost* frame,
                           scoped_refptr<content::FileSelectListener> listener,
                           const blink::mojom::FileChooserParams& params);

void InstallDownloadHooks();
void StopDownloadHooks();
void CancelDownloadRequests(zebkit_view* view);

void InstallClientCertificateHooks();
void StopClientCertificateHooks();
void CancelClientCertificateRequests(zebkit_view* view);
// @identities: the operating system's (clientCertificateStores), else none.
base::OnceClosure HandleClientCertificate(
    content::WebContents* contents, const net::SSLCertRequestInfo& info,
    net::ClientCertIdentityList identities,
    std::unique_ptr<content::ClientCertificateDelegate> delegate);

}  // namespace zebkit

#endif  // ZEBKIT_ENGINE_PAGE_HOOKS_H_
