/*
 * The part of a platform layer (see CDPPlatform.h) that needs nothing of
 * the platform: what the endpoint is told from inside WebKit about loads,
 * dialogs and file choosers, and network sessions, cookies and the
 * back/forward cache, all of which are WebKit's own cross-platform classes
 * (WebPageProxy, WebsiteDataStore, WebCore::Cookie).
 *
 * It is for the platforms whose layer sits on those classes and not on an
 * API of the port. Windows is built from it, with CDPViewWin.cpp for what
 * does need the platform. macOS has the same code in CDPViewCocoa.mm, from
 * before this file existed, and is to be moved onto it.
 */

#include "config.h"
#include "CDPInternal.h"

#if PLATFORM(WIN)

#include "APIHTTPCookieStore.h"
#include "WebOpenPanelResultListenerProxy.h"
#include "WebPageProxy.h"
#include <wtf/NeverDestroyed.h>
#include <wtf/URL.h>
#include <wtf/WallTime.h>
#include <wtf/text/MakeString.h>

namespace Zebkit::CDP {

static RefPtr<PageTarget> pageFor(WebKit::WebPageProxy& proxy)
{
    auto view = viewForPage(proxy);
    return view ? Server::singleton().pageForView(view) : nullptr;
}

// MARK: Loads

void loadStarted(WebKit::WebPageProxy& proxy)
{
    if (RefPtr page = pageFor(proxy))
        page->loadChanged(LoadEvent::Started);
}

void loadCommitted(WebKit::WebPageProxy& proxy)
{
    if (RefPtr page = pageFor(proxy))
        page->loadChanged(LoadEvent::Committed);
}

void loadFinished(WebKit::WebPageProxy& proxy)
{
    if (RefPtr page = pageFor(proxy))
        page->loadChanged(LoadEvent::Finished);
}

void processTerminated(WebKit::WebPageProxy& proxy)
{
    if (RefPtr page = pageFor(proxy))
        page->processTerminated();
}

void PageTarget::setCertificateErrorsIgnored(bool ignore)
{
    m_native.ignoresCertificateErrors = ignore;
}

void PageTarget::restoreCertificateChecks()
{
    m_native.ignoresCertificateErrors = false;
}

// MARK: Dialogs and file choosers

bool takesDialogs(WebKit::WebPageProxy& proxy)
{
    RefPtr page = pageFor(proxy);
    return page && page->acceptsDialog();
}

bool anyPageTakesDialogs()
{
    return Server::singleton().anyPageAcceptsDialog();
}

bool Server::anyPageAcceptsDialog() const
{
    for (auto& page : m_pages) {
        if (page->acceptsDialog())
            return true;
    }
    return false;
}

void runDialog(WebKit::WebPageProxy& proxy, Dialog dialog, const String& message, const String& defaultPrompt, Function<void(bool, const String&)>&& answer)
{
    RefPtr page = pageFor(proxy);
    if (!page || !page->acceptsDialog()) {
        answer(false, { });
        return;
    }
    ASCIILiteral type = dialog == Dialog::Alert ? "alert"_s : dialog == Dialog::Confirm ? "confirm"_s : dialog == Dialog::Prompt ? "prompt"_s : "beforeunload"_s;
    page->scriptDialog(type, message, dialog == Dialog::Prompt ? makeString(""_s, defaultPrompt) : String(), WTF::move(answer));
}

void PageTarget::scriptDialog(ASCIILiteral type, const String& message, const String& defaultPrompt, Function<void(bool, const String&)>&& answer)
{
    // Holding the answer holds the dialog, and the script that opened it,
    // until a client answers with Page.handleJavaScriptDialog.
    m_native.dialog = WTF::move(answer);
    announceDialog(type, message, defaultPrompt);
}

bool PageTarget::hasOpenDialog() const
{
    return !!m_native.dialog;
}

void PageTarget::answerDialog(bool accept, const String& promptText)
{
    if (auto answer = std::exchange(m_native.dialog, nullptr))
        answer(accept, promptText);
}

void PageTarget::closeDialog()
{
    // The page cannot be left waiting: an unanswered dialog is declined.
    answerDialog(false, { });
}

bool chooseFiles(WebKit::WebPageProxy& proxy, WebKit::WebOpenPanelResultListenerProxy& listener)
{
    RefPtr page = pageFor(proxy);
    return page && page->runFileChooser(listener);
}

bool PageTarget::runFileChooser(WebKit::WebOpenPanelResultListenerProxy& listener)
{
    if (!std::exchange(m_expectingFileChooser, false))
        return false;
    listener.chooseFiles(std::exchange(m_pendingFiles, { }));
    return true;
}

// MARK: Network sessions

String Server::userAgent()
{
    if (!m_pages.isEmpty())
        return pageProxy(m_pages.first()->view()).userAgent();
    return WebKit::WebPageProxy::standardUserAgent();
}

PlatformNetworkSession networkSessionOf(PlatformView view)
{
    return &pageProxy(view).websiteDataStore();
}

PlatformNetworkSession defaultNetworkSession()
{
    return &WebKit::WebsiteDataStore::defaultDataStore();
}

PlatformNetworkSession createEphemeralNetworkSession()
{
    return WebKit::WebsiteDataStore::createNonPersistent();
}

// A page restored from the back/forward cache comes back with the script
// contexts it had, and WebKit's inspector does not announce them again.
// While a client is attached, history navigations load afresh. Views can
// share one set of preferences, so the pages that need this are counted
// and the preference gets its own value back when the last of them is done.
void PageTarget::setPageCacheSuspended(bool suspended)
{
    struct Suspension {
        unsigned count { 0 };
        bool wasEnabled { false };
    };
    static NeverDestroyed<HashMap<WebKit::WebPreferences*, Suspension>> suspensions;
    if (suspended == !!m_native.pageCachePreferences || (suspended && !m_view))
        return;
    if (suspended)
        m_native.pageCachePreferences = &pageProxy(m_view).preferences();
    RefPtr preferences = suspended ? m_native.pageCachePreferences : std::exchange(m_native.pageCachePreferences, nullptr);

    auto& suspension = suspensions.get().add(preferences.get(), Suspension { }).iterator->value;
    if (suspended) {
        if (!suspension.count++) {
            suspension.wasEnabled = preferences->usesBackForwardCache();
            preferences->setUsesBackForwardCache(false);
        }
        return;
    }
    if (suspension.count && !--suspension.count && suspension.wasEnabled)
        preferences->setUsesBackForwardCache(true);
    if (!suspension.count)
        suspensions.get().remove(preferences.get());
}

// MARK: Cookies

void allCookies(const PlatformNetworkSession& session, Function<void(CookieList&&)>&& completion)
{
    session->cookieStore().cookies([completion = WTF::move(completion)](Vector<WebCore::Cookie>&& cookies) mutable {
        CookieList list;
        for (auto& cookie : cookies)
            list.append(makeUniqueWithoutFastMallocCheck<WebCore::Cookie>(WTF::move(cookie)));
        completion(WTF::move(list));
    });
}

void addCookie(const PlatformNetworkSession& session, const PlatformCookie& cookie, Function<void()>&& completion)
{
    session->cookieStore().setCookies({ *cookie }, [completion = WTF::move(completion)]() mutable {
        completion();
    });
}

void deleteCookie(const PlatformNetworkSession& session, const PlatformCookie& cookie, Function<void()>&& completion)
{
    session->cookieStore().deleteCookie(*cookie, [completion = WTF::move(completion)]() mutable {
        completion();
    });
}

Ref<JSON::Object> cookieToCDP(const PlatformCookie& cookie)
{
    auto object = JSON::Object::create();
    object->setString("name"_s, cookie->name);
    object->setString("value"_s, cookie->value);
    object->setString("domain"_s, cookie->domain);
    object->setString("path"_s, cookie->path);
    bool isSession = cookie->session || !cookie->expires;
    // Kept in milliseconds, reported in seconds.
    object->setDouble("expires"_s, isSession ? -1 : std::floor(*cookie->expires / 1000));
    object->setInteger("size"_s, cookie->name.length() + cookie->value.length());
    object->setBoolean("httpOnly"_s, cookie->httpOnly);
    object->setBoolean("secure"_s, cookie->secure);
    object->setBoolean("session"_s, isSession);
    switch (cookie->sameSite) {
    case WebCore::Cookie::SameSitePolicy::Strict:
        object->setString("sameSite"_s, "Strict"_s);
        break;
    case WebCore::Cookie::SameSitePolicy::Lax:
        object->setString("sameSite"_s, "Lax"_s);
        break;
    case WebCore::Cookie::SameSitePolicy::None:
        if (cookie->secure)
            object->setString("sameSite"_s, "None"_s);
        break;
    }
    object->setString("priority"_s, "Medium"_s);
    object->setString("sourceScheme"_s, cookie->secure ? "Secure"_s : "NonSecure"_s);
    object->setInteger("sourcePort"_s, cookie->secure ? 443 : 80);
    return object;
}

PlatformCookie cookieFromCDP(JSON::Object& object, const String& fallbackURL)
{
    auto addressText = object.getString("url"_s);
    URL address { addressText.isEmpty() ? fallbackURL : addressText };

    auto cookie = makeUniqueWithoutFastMallocCheck<WebCore::Cookie>();
    cookie->name = object.getString("name"_s);
    cookie->value = object.getString("value"_s);
    cookie->domain = object.getString("domain"_s);
    if (cookie->domain.isEmpty() && address.isValid())
        cookie->domain = address.host().toString();
    if (cookie->name.isNull() || cookie->domain.isEmpty())
        return nullptr;
    if (cookie->value.isNull())
        cookie->value = emptyString();
    cookie->path = object.getString("path"_s);
    if (cookie->path.isEmpty())
        cookie->path = "/"_s;
    cookie->secure = object.getBoolean("secure"_s).value_or(address.protocolIs("https"_s));
    cookie->httpOnly = object.getBoolean("httpOnly"_s).value_or(false);
    auto sameSite = object.getString("sameSite"_s);
    if (sameSite == "Strict"_s)
        cookie->sameSite = WebCore::Cookie::SameSitePolicy::Strict;
    else if (sameSite == "Lax"_s)
        cookie->sameSite = WebCore::Cookie::SameSitePolicy::Lax;
    cookie->created = WallTime::now().secondsSinceEpoch().milliseconds();
    auto expires = object.getDouble("expires"_s);
    if (expires && *expires > 0)
        cookie->expires = *expires * 1000;
    cookie->session = !cookie->expires;
    return cookie;
}

// RFC 6265, 5.1.3. A cookie's domain may start with a dot, which changes nothing.
static bool domainMatches(const String& cookieDomain, const String& host)
{
    auto domain = cookieDomain.startsWith('.') ? cookieDomain.substring(1) : cookieDomain;
    if (equalIgnoringASCIICase(host, domain))
        return true;
    return host.endsWithIgnoringASCIICase(makeString('.', domain)) && !isIPAddress(host);
}

bool cookieAppliesToURL(const PlatformCookie& cookie, const String& url)
{
    URL address { url };
    if (!address.isValid() || address.host().isEmpty())
        return false;
    if (cookie->secure && !address.protocolIs("https"_s))
        return false;
    if (cookie->expires && *cookie->expires < WallTime::now().secondsSinceEpoch().milliseconds())
        return false;
    if (!domainMatches(cookie->domain, address.host().toString()))
        return false;
    // RFC 6265, 5.1.4: the cookie's path is the request's, or a directory above it.
    auto path = address.path().isEmpty() ? "/"_str : address.path().toString();
    auto& cookiePath = cookie->path;
    if (cookiePath.isEmpty() || path == cookiePath)
        return true;
    return path.startsWith(cookiePath) && (cookiePath.endsWith('/') || path[cookiePath.length()] == '/');
}

bool cookieMatches(const PlatformCookie& cookie, const String& name, const String& domain, const String& path, const String& url)
{
    if (name != cookie->name)
        return false;
    if (!domain.isEmpty() && !domainMatches(cookie->domain, domain))
        return false;
    if (!path.isEmpty() && path != cookie->path)
        return false;
    URL address { url };
    if (!url.isEmpty() && address.isValid() && !address.host().isEmpty() && !domainMatches(cookie->domain, address.host().toString()))
        return false;
    return true;
}

} // namespace Zebkit::CDP

#endif // PLATFORM(WIN)
