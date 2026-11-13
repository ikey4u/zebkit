// External protocols, file choosers, media, keys, context menus, fullscreen
// (page_hooks.cc); downloads (download_hooks.cc); client certificates
// (client_certificate.cc). docs/HOOKS.md.

#ifndef ZEBKIT_ENGINE_PAGE_HOOKS_H_
#define ZEBKIT_ENGINE_PAGE_HOOKS_H_

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "base/functional/callback_forward.h"
#include "base/memory/scoped_refptr.h"
#include "base/values.h"
#include "content/public/browser/frame_tree_node_id.h"
#include "services/network/public/mojom/web_sandbox_flags.mojom-shared.h"
#include "third_party/blink/public/mojom/choosers/file_chooser.mojom-forward.h"
#include "url/origin.h"
#include "zebkit/engine/view.h"

class GURL;
namespace content {
class EyeDropper;
class EyeDropperListener;
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
// The page's EyeDropper.open() to the application (zebkit_view_on_eye_dropper);
// null without a function, which cancels it.
std::unique_ptr<content::EyeDropper> OpenEyeDropperHook(content::RenderFrameHost* frame,
                                                       content::EyeDropperListener* listener);
// A page's device chooser to the application (zebkit_view_on_device_chooser):
// the request's id (0 when too many wait: @choice gets none); nullopt without
// a function, @choice unrun and the caller's.
// @choice gets the chosen device's id, or nothing; Update replaces the list
// while it waits, Abandon ends it when the chooser goes.
using DeviceChoice = base::OnceCallback<void(std::optional<std::string>)>;
std::optional<uintptr_t> AskDeviceChooser(content::RenderFrameHost* frame, const char* kind, base::Value filters,
                                          base::ListValue devices, DeviceChoice& choice);
void UpdateDeviceChooser(uintptr_t id, base::ListValue devices);
void AbandonDeviceChooser(uintptr_t id);
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
