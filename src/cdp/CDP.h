/*
 * Chrome DevTools Protocol endpoint for zebkit.
 *
 * WebKit does not speak CDP. It has its own remote debugging protocol, the
 * Web Inspector Protocol, which covers much of the same ground with
 * different message shapes and without the browser-level parts (targets,
 * input, navigation). This directory is a bridge compiled into the engine
 * library: it serves the HTTP and WebSocket endpoints that CDP clients
 * expect, answers what it can natively through the WebKit API, and
 * translates the rest to and from the Web Inspector Protocol.
 *
 * docs/CDP.md lists what is covered.
 *
 * This header is what WebKit's own sources see of the endpoint: the calls
 * the patches in patches/ add to them.
 */

#pragma once

#if USE(GLIB)

typedef struct _WebKitWebView WebKitWebView;

namespace Zebkit::CDP {

// Called by WebKitWebView when a view has been constructed and when it is
// about to be destroyed. Every view in the process is a CDP page target
// while the server is listening.
void webViewCreated(WebKitWebView*);
void webViewDestroyed(WebKitWebView*);

} // namespace Zebkit::CDP

#elif PLATFORM(COCOA)

#include <wtf/Forward.h>
#include <wtf/Function.h>

OBJC_CLASS WKWebView;

namespace API {
class PageConfiguration;
}

namespace WebCore {
class ResourceError;
}

namespace WebKit {
class AuthenticationChallengeProxy;
class WebOpenPanelResultListenerProxy;
class WebPageProxy;
}

namespace Zebkit::CDP {

// Called by WKWebView when a view has been set up and when it is about to
// go. Every view in the process is a CDP page target while the server is
// listening.
void webViewCreated(WKWebView *);
void webViewDestroyed(WKWebView *);

// A WKWebView reports to the delegates its application gave it, and has one
// of each. The endpoint is told from where the delegates are called.

// From the navigation client: the main frame's loads.
void loadStarted(WebKit::WebPageProxy&);
void loadCommitted(WebKit::WebPageProxy&);
void loadFinished(WebKit::WebPageProxy&);
void loadFailed(WebKit::WebPageProxy&, const WebCore::ResourceError&);
void processTerminated(WebKit::WebPageProxy&);
// True if the endpoint answered the challenge: a client asked for
// certificate errors to be ignored, and this is one.
bool answerChallenge(WebKit::WebPageProxy&, WebKit::AuthenticationChallengeProxy&);

// From the UI client. Where the endpoint takes something, the delegate is
// not asked.
enum class Dialog : uint8_t { Alert, Confirm, Prompt, BeforeUnload };
// A client of this page answers its dialogs; of any page, for the second.
bool takesDialogs(WebKit::WebPageProxy&);
bool anyPageTakesDialogs();
// Only if takesDialogs().
void runDialog(WebKit::WebPageProxy&, Dialog, const String& message, const String& defaultPrompt, Function<void(bool accept, const String& promptText)>&&);
bool chooseFiles(WebKit::WebPageProxy&, WebKit::WebOpenPanelResultListenerProxy&);
bool pageClosed(WebKit::WebPageProxy&);
// A page opened a window and the application made no view for it. Null if
// the endpoint makes none either.
RefPtr<WebKit::WebPageProxy> createPage(WebKit::WebPageProxy& opener, API::PageConfiguration&);

} // namespace Zebkit::CDP

#endif
