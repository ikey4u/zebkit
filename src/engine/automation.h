// zebkit's automation (<zebkit/automation.h>, docs/AUTOMATION.md): what
// the rest of the engine calls.

#ifndef ZEBKIT_ENGINE_AUTOMATION_H_
#define ZEBKIT_ENGINE_AUTOMATION_H_

#include <stdint.h>

#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "url/gurl.h"
#include "v8/include/v8-forward.h"

#include "content/public/browser/login_delegate.h"
#include "mojo/public/cpp/bindings/pending_associated_receiver.h"
#include "zebkit/engine/automation.mojom-forward.h"

namespace net {
class AuthChallengeInfo;
class SSLCertRequestInfo;
}  // namespace net

namespace content {
class BrowserContext;
class ClientCertificateDelegate;
class RenderFrame;
class RenderFrameHost;
class WebContents;
}  // namespace content

namespace zebkit {

// A Step's handle at or below this is the frame's node whose DOM node id
// (DevTools' backendNodeId) is kDomNodeHandleBase - handle.
inline constexpr int32_t kDomNodeHandleBase = -(1 << 30);

// The renderer's agent for @render_frame (automation_renderer.cc); it
// deletes itself with the frame.
void InstallAutomationAgent(content::RenderFrame* render_frame);

// A worker's context made, on its thread (ContentRendererClient's
// DidInitializeWorkerContextOnWorkerThread()): Playwright's Worker.
void AutomationWorkerStarted(v8::Local<v8::Context> context);

// What automation takes from the rest of the engine, a page's dialogs and
// file choosers (automation.cc); once, when the browser starts.
void InstallAutomationHooks();

// HTTP authentication of @profile's pages, answered with the credentials
// automation was given (BrowserContext.setHTTPCredentials); nullptr when it
// has none for the challenger.
std::unique_ptr<content::LoginDelegate> CreateAutomationLoginDelegate(
    const net::AuthChallengeInfo& challenge,
    content::BrowserContext* profile,
    content::WebContents* contents,
    bool main_frame_navigation,
    const GURL& url,
    bool first_attempt,
    content::LoginDelegate::LoginAuthRequiredCallback callback);

// The proxy Browser.newContext gave @profile, as Chrome's DevTools takes it
// (Target.createBrowserContext's proxyServer, proxyBypassList); nullopt
// for none.
std::optional<std::pair<std::string, std::string>> AutomationProxyFor(
    content::BrowserContext* profile);

// A TLS client certificate request of @profile's, answered with the
// certificate Browser.newContext's clientCertificates gives the server's
// origin: true when it was (@delegate taken).
bool AutomationSelectClientCertificate(
    content::BrowserContext* profile,
    const net::SSLCertRequestInfo& info,
    std::unique_ptr<content::ClientCertificateDelegate>* delegate);

// Whether automation drives @contents' input (a page of Playwright's): its
// mouse events carry a drag the page starts (drag.cc).
bool AutomationDrivesInput(content::WebContents* contents);

// The view's automation events go to nobody from now on: the view is closing,
// and the page's automation, which outlives it a little, still emits events
// (zebkit_automation_on_event's function and data are the view's).
void ForgetAutomationEvents(content::WebContents* contents);

// The browser's end of @frame's page calls (Playwright's exposeBinding),
// bound by the frame's associated interfaces.
void BindAutomationHost(
    content::RenderFrameHost* frame,
    mojo::PendingAssociatedReceiver<mojom::AutomationHost> receiver);

}  // namespace zebkit

#endif  // ZEBKIT_ENGINE_AUTOMATION_H_
