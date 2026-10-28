// zebkit's automation (<zebkit/automation.h>, docs/AUTOMATION.md): what
// the rest of the engine calls.

#ifndef ZEBKIT_ENGINE_AUTOMATION_H_
#define ZEBKIT_ENGINE_AUTOMATION_H_

#include <memory>

#include "v8/include/v8-forward.h"

#include "content/public/browser/login_delegate.h"
#include "mojo/public/cpp/bindings/pending_associated_receiver.h"
#include "zebkit/engine/automation.mojom-forward.h"

namespace net {
class AuthChallengeInfo;
}  // namespace net

namespace content {
class BrowserContext;
class RenderFrame;
class RenderFrameHost;
}  // namespace content

namespace zebkit {

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
    bool first_attempt,
    content::LoginDelegate::LoginAuthRequiredCallback callback);

// The browser's end of @frame's page calls (Playwright's exposeBinding),
// bound by the frame's associated interfaces.
void BindAutomationHost(
    content::RenderFrameHost* frame,
    mojo::PendingAssociatedReceiver<mojom::AutomationHost> receiver);

}  // namespace zebkit

#endif  // ZEBKIT_ENGINE_AUTOMATION_H_
