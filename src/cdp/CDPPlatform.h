// The line between the endpoint's portable code and what a platform has to
// supply.
//
// Portable (CDPServer.cpp, CDPPage.cpp, CDPDomains.cpp): the protocol.
// Sessions, targets, the translation between CDP and WebKit's inspector
// protocol, HTTP discovery and the routing of sockets. It uses WTF and
// WebKit's cross-platform classes, and what is declared here.
//
// Per platform: the web view API, the network listener, input, windows,
// cookies, dialogs. On Linux that is WebKit's GLib API, libsoup and WPE,
// in CDPViewGLib.cpp and CDPTransportGLib.cpp. On macOS it is WKWebView,
// AppKit and BSD sockets, in CDPViewCocoa.mm and CDPTransportCocoa.mm. On
// Windows it is the view of WebKit's C API, window messages and Winsock,
// in CDPViewWin.cpp and CDPTransportWin.cpp, with what needs none of those
// in CDPViewProxy.cpp. A port to another platform writes its own versions
// of those files, and adds its types below.
//
// What a platform supplies, all of it:
//
//  1. The types in the #if block below.
//  2. The free functions declared in this file.
//  3. Listener::create(), and a Socket for each WebSocket it accepts,
//     handed to Server::acceptSocket(); HTTP requests go to
//     Server::handleHTTP().
//  4. Server::createView() and Server::userAgent().
//  5. The members of PageTarget marked "platform" in CDPInternal.h: the
//     ones that load, navigate, close, resize, send input, answer dialogs
//     and choose files, and the hook-up of the view's notifications, which
//     call back into the portable members (loadChanged and the like).
//  6. A call to Server::addView() and removeView() for every view of the
//     process (webViewCreated/webViewDestroyed in CDP.h, on Linux), and
//     the public C API in zebkit-cdp.h.
//
// scripts/cdp.sh fails if the portable files use a platform API directly.

#pragma once

#include <wtf/Forward.h>
#include <wtf/Function.h>
#include <wtf/JSONValues.h>
#include <wtf/RefCounted.h>
#include <wtf/Vector.h>
#include <wtf/text/WTFString.h>

#if USE(GLIB)
#include <WebCore/GUniquePtrSoup.h>
#include <glib-object.h>
#include <libsoup/soup.h>
#include <wtf/glib/GRefPtr.h>
#include <wtf/glib/GUniquePtr.h>

typedef struct _WebKitWebView WebKitWebView;
typedef struct _WebKitNetworkSession WebKitNetworkSession;
typedef struct _WebKitScriptDialog WebKitScriptDialog;
#elif PLATFORM(COCOA)
#include "WebPreferences.h"
#include "WebsiteDataStore.h"
#include <WebCore/Cookie.h>
#include <wtf/RetainPtr.h>

OBJC_CLASS NSObject;
OBJC_CLASS WKWebView;
OBJC_CLASS WKWebViewConfiguration;
#elif PLATFORM(WIN)
#include "WebPreferences.h"
#include "WebsiteDataStore.h"
#include <WebCore/Cookie.h>

namespace WebKit {
class WebView;
}
#else
#error "The CDP endpoint has no platform layer for this platform yet (see CDPPlatform.h)"
#endif

namespace WebKit {
class WebPageProxy;
}

namespace Zebkit::CDP {

#if USE(GLIB)
// A view, by identity. Null once the view is gone.
using PlatformView = WebKitWebView*;
// What holds a browsing context's cookies and cache. Owning.
using PlatformNetworkSession = GRefPtr<WebKitNetworkSession>;
using PlatformCookie = GUniquePtr<SoupCookie>;
// What the platform members of PageTarget keep between calls.
struct PlatformPageState {
    Vector<gulong> signalHandlers;
    GRefPtr<GObject> pageCacheSettings;
    WebKitScriptDialog* dialog { nullptr };
    std::optional<int> originalTLSPolicy;
};
// After a failed load the engine shows a page of its own that says so.
static constexpr bool engineShowsErrorPages = true;
#elif PLATFORM(COCOA)
// A view, by identity, and not retained: the view says when it goes.
using PlatformView = WKWebView *;
using PlatformNetworkSession = RefPtr<WebKit::WebsiteDataStore>;
using PlatformCookie = std::unique_ptr<WebCore::Cookie>;
struct PlatformPageState {
    // Watches the view's title and address.
    RetainPtr<NSObject> observer;
    // Answers the dialog the page is waiting in. Null when none is open.
    Function<void(bool accept, const String& promptText)> dialog;
    bool ignoresCertificateErrors { false };
    // A client set the device scale factor; what it was, 0 for the screen's.
    bool overridesScale { false };
    float originalScale { 0 };
    // Set while the back/forward cache is off for this page's sake.
    RefPtr<WebKit::WebPreferences> pageCachePreferences;
};
// A failed load leaves the view as it was; an application shows what it likes.
static constexpr bool engineShowsErrorPages = false;
#elif PLATFORM(WIN)
// A view, by identity, and not referenced: the view says when it goes.
using PlatformView = WebKit::WebView*;
using PlatformNetworkSession = RefPtr<WebKit::WebsiteDataStore>;
using PlatformCookie = std::unique_ptr<WebCore::Cookie>;
class ViewObserver;
struct PlatformPageState {
    PlatformPageState();
    ~PlatformPageState();
    // Watches the page's title and address.
    RefPtr<ViewObserver> observer;
    // Answers the dialog the page is waiting in. Null when none is open.
    Function<void(bool accept, const String& promptText)> dialog;
    bool ignoresCertificateErrors { false };
    // A client set the device scale factor; what it was, 0 for the screen's.
    bool overridesScale { false };
    float originalScale { 0 };
    // Set while the back/forward cache is off for this page's sake.
    RefPtr<WebKit::WebPreferences> pageCachePreferences;
};
// A failed load leaves the view as it was; an application shows what it likes.
static constexpr bool engineShowsErrorPages = false;
#endif

using CookieList = Vector<PlatformCookie>;

// MARK: Views

WebKit::WebPageProxy& pageProxy(PlatformView);
#if PLATFORM(WIN)
// The view a page is shown in; null for a page no view of this process has.
PlatformView viewForPage(WebKit::WebPageProxy&);
#endif
String viewTitle(PlatformView);
String viewURI(PlatformView);
void loadURI(PlatformView, const String&);
void reloadView(PlatformView, bool bypassCache);
void stopLoading(PlatformView);
// Kills the process the page runs in, as a crash would.
void terminateWebProcess(PlatformView);
// In CSS pixels; 0x0 when the view has no surface.
std::pair<int, int> viewSize(PlatformView);
// The engine's version, as in "2.54.1".
String engineVersion();

// MARK: Network sessions and cookies

PlatformNetworkSession networkSessionOf(PlatformView);
PlatformNetworkSession defaultNetworkSession();
// A session that shares nothing with any other and keeps nothing on disk.
PlatformNetworkSession createEphemeralNetworkSession();

void allCookies(const PlatformNetworkSession&, Function<void(CookieList&&)>&&);
void addCookie(const PlatformNetworkSession&, const PlatformCookie&, Function<void()>&&);
void deleteCookie(const PlatformNetworkSession&, const PlatformCookie&, Function<void()>&&);
Ref<JSON::Object> cookieToCDP(const PlatformCookie&);
// Null if the fields do not make a cookie. A cookie without a domain gets
// that of fallbackURL.
PlatformCookie cookieFromCDP(JSON::Object&, const String& fallbackURL);
// Whether a request for the address would carry the cookie.
bool cookieAppliesToURL(const PlatformCookie&, const String& url);
// Network.deleteCookies' rule: the name, and whichever of the rest is given.
bool cookieMatches(const PlatformCookie&, const String& name, const String& domain, const String& path, const String& url);

// MARK: Transport

// One accepted WebSocket. The platform makes it; the portable code sets
// the two callbacks and then only sends and closes.
class Socket : public RefCounted<Socket> {
public:
    virtual ~Socket() = default;
    virtual bool isOpen() const = 0;
    virtual void send(const String& text) = 0;
    // A refusal says why; an ordinary close says nothing.
    virtual void close(ASCIILiteral refusal = { }) = 0;

    // A text message arrived.
    Function<void(const String&)> onMessage;
    // The other side went, or close() finished. Called at most once.
    Function<void()> onClosed;
};

struct HTTPRequest {
    String method;
    String path;
    // What follows "?", still escaped.
    String query;
    // Null when the request did not have the header.
    String host;
    String origin;
};

struct HTTPResponse {
    unsigned status;
    String body;
    ASCIILiteral type;
};

// The listening socket. Destroying it stops listening.
class Listener {
public:
    // An empty address is the loopback interface. Null, with a reason, if
    // the address is not a numeric one or the port cannot be had.
    static std::unique_ptr<Listener> create(const String& address, unsigned port, String& error);
    virtual ~Listener() = default;

    unsigned port { 0 };
    // As it goes into a URL: IPv6 in brackets.
    String host;
    bool isLoopback { true };
};

} // namespace Zebkit::CDP
