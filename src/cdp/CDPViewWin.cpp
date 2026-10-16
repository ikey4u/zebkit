/*
 * The platform layer for Windows (see CDPPlatform.h): what of a page goes
 * through the view and needs the platform. The view is WebKit::WebView,
 * the object behind the C API's WKViewRef, which is a child window.
 * Navigation, input, window size, the life of the view and the creation of
 * views. What needs no platform is in CDPViewProxy.cpp.
 *
 * A page tells what happens to it to the clients of the application
 * (WKPageSetPageNavigationClient, WKPageSetPageUIClient), and has one of
 * each, so the endpoint cannot take their place. It is told from inside
 * WebKit instead, where the clients are called (the functions declared in
 * CDP.h, called by patches/2011). Title and address are observed.
 *
 * Input is made as the events WebKit makes from window messages, with what
 * the client said in them, and given to the page as the view gives its
 * own: pages see trusted events, and default actions (focus, text entry,
 * link activation) happen. It is not sent as window messages, which carry
 * less than CDP says: no click count, no key name, and modifiers only as
 * the state of the real keyboard.
 *
 * What is the same on every platform, such as the bookkeeping of a
 * navigation and the order of input answers, is in CDPView.cpp.
 */

#include "config.h"
#include "CDPInternal.h"

// Linux builds every .cpp file of this directory.
#if PLATFORM(WIN)

#include "APIArray.h"
#include "APINavigation.h"
#include "APIPageConfiguration.h"
#include "AuthenticationChallengeProxy.h"
#include "AuthenticationDecisionListener.h"
#include "NativeWebKeyboardEvent.h"
#include "NativeWebMouseEvent.h"
#include "NativeWebWheelEvent.h"
#include "PageLoadState.h"
#include "WKAPICast.h"
#include "WKPage.h"
#include "WebBackForwardList.h"
#include "WebBackForwardListItem.h"
#include "WebPageProxy.h"
#include "WebProcessProxy.h"
#include "WebView.h"
#include <WebCore/Credential.h>
#include <WebCore/ResourceError.h>
#include <WebCore/ResourceRequest.h>
// The header of curl wants the socket types declared first.
#include <winsock2.h>
#include <curl/curl.h>
#include <wtf/HexNumber.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/URL.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringToIntegerConversion.h>

namespace Zebkit::CDP {

static constexpr int defaultViewWidth = 1280;
static constexpr int defaultViewHeight = 720;

// Tells a page of the changes to its view's title and address.
class ViewObserver final : public RefCounted<ViewObserver>, public WebKit::PageLoadState::Observer {
public:
    static Ref<ViewObserver> create(PageTarget& page) { return adoptRef(*new ViewObserver(page)); }

    void ref() const final { RefCounted::ref(); }
    void deref() const final { RefCounted::deref(); }

private:
    explicit ViewObserver(PageTarget& page)
        : m_page(page)
    {
    }

    void didChangeTitle() final
    {
        if (RefPtr page = m_page.get())
            page->infoChanged();
    }

    void didChangeActiveURL() final
    {
        RefPtr page = m_page.get();
        if (!page)
            return;
        page->infoChanged();
        page->addressChanged();
    }

    void willChangeIsLoading() final { }
    void didChangeIsLoading() final { }
    void willChangeTitle() final { }
    void willChangeActiveURL() final { }
    void willChangeHasOnlySecureContent() final { }
    void didChangeHasOnlySecureContent() final { }
    void willChangeEstimatedProgress() final { }
    void didChangeEstimatedProgress() final { }
    void willChangeCanGoBack() final { }
    void didChangeCanGoBack() final { }
    void willChangeCanGoForward() final { }
    void didChangeCanGoForward() final { }
    void willChangeNetworkRequestsInProgress() final { }
    void didChangeNetworkRequestsInProgress() final { }
    void willChangeCertificateInfo() final { }
    void didChangeCertificateInfo() final { }
    void willChangeWebProcessIsResponsive() final { }
    void didChangeWebProcessIsResponsive() final { }
    void didSwapWebProcesses() final { }

    WeakPtr<PageTarget> m_page;
};

PlatformPageState::PlatformPageState() = default;
PlatformPageState::~PlatformPageState() = default;

// The views of the process, by their pages.
static HashMap<WebKit::WebPageProxy*, WebKit::WebView*>& viewsByPage()
{
    static NeverDestroyed<HashMap<WebKit::WebPageProxy*, WebKit::WebView*>> views;
    return views;
}

PlatformView viewForPage(WebKit::WebPageProxy& proxy)
{
    return viewsByPage().get(&proxy);
}

// A view the endpoint made, and the window it is in. Nobody looks at the
// window: it is there because a page lays out, paints and takes input
// only in one, and it is off every screen.
struct OwnedView {
    RefPtr<WebKit::WebView> view;
    HWND window { nullptr };
};

static HashMap<WebKit::WebView*, OwnedView>& ownedViews()
{
    static NeverDestroyed<HashMap<WebKit::WebView*, OwnedView>> views;
    return views;
}

// Lets go of a view the endpoint made. The view's window goes with the one
// it is in, and the view with the last reference to it.
static void destroyOwnedView(WebKit::WebView* view)
{
    auto owned = ownedViews().take(view);
    if (owned.window)
        ::DestroyWindow(owned.window);
}

// MARK: The view's notifications

void PageTarget::connectViewSignals()
{
    m_native.observer = ViewObserver::create(*this);
    pageProxy(m_view).pageLoadState().addObserver(*m_native.observer);
}

void PageTarget::releaseView()
{
    if (RefPtr observer = std::exchange(m_native.observer, nullptr))
        pageProxy(m_view).pageLoadState().removeObserver(*observer);
    m_native.dialog = nullptr;
}

void loadFailed(WebKit::WebPageProxy& proxy, const WebCore::ResourceError& error)
{
    RefPtr page = Server::singleton().pageForView(viewForPage(proxy));
    if (!page)
        return;
    if (error.isCancellation()) {
        page->loadCancelled();
        return;
    }
    int code = error.errorCode();
    if (logLevel())
        WTFLogAlways("cdp: load failed: %s %d: %s", error.domain().utf8().data(), code, error.localizedDescription().utf8().data());

    // The network library is curl, and the codes are its own.
    ASCIILiteral text = "net::ERR_FAILED"_s;
    if (error.domain() != "CurlErrorDomain"_s) {
        // WebKit's own errors: a policy decision stopped the load.
        text = "net::ERR_ABORTED"_s;
    } else if (code == CURLE_COULDNT_RESOLVE_HOST || code == CURLE_COULDNT_RESOLVE_PROXY)
        text = "net::ERR_NAME_NOT_RESOLVED"_s;
    else if (code == CURLE_COULDNT_CONNECT)
        text = "net::ERR_CONNECTION_REFUSED"_s;
    else if (code == CURLE_OPERATION_TIMEDOUT)
        text = "net::ERR_TIMED_OUT"_s;
    else if (code == CURLE_RECV_ERROR || code == CURLE_SEND_ERROR || code == CURLE_GOT_NOTHING || code == CURLE_PARTIAL_FILE)
        text = "net::ERR_CONNECTION_CLOSED"_s;
    else if (code == CURLE_SSL_CONNECT_ERROR)
        text = "net::ERR_SSL_PROTOCOL_ERROR"_s;
    else if (code == CURLE_PEER_FAILED_VERIFICATION || code == CURLE_SSL_CACERT_BADFILE || code == CURLE_SSL_ISSUER_ERROR)
        text = "net::ERR_CERT_AUTHORITY_INVALID"_s;
    else if (code == CURLE_FILE_COULDNT_READ_FILE)
        text = "net::ERR_FILE_NOT_FOUND"_s;
    else if (code == CURLE_UNSUPPORTED_PROTOCOL)
        text = "net::ERR_UNKNOWN_URL_SCHEME"_s;
    // The load is over as well as failed.
    page->loadFailed(text);
    page->loadChanged(LoadEvent::Finished);
}

bool answerChallenge(WebKit::WebPageProxy& proxy, WebKit::AuthenticationChallengeProxy& challenge)
{
    RefPtr page = Server::singleton().pageForView(viewForPage(proxy));
    if (!page || !page->native().ignoresCertificateErrors)
        return false;
    if (challenge.core().protectionSpace().authenticationScheme() != WebCore::ProtectionSpace::AuthenticationScheme::ServerTrustEvaluationRequested)
        return false;
    // Any credential says the certificate is taken as it is.
    challenge.listener().completeChallenge(WebKit::AuthenticationChallengeDisposition::UseCredential,
        WebCore::Credential("accept server trust"_s, emptyString(), WebCore::CredentialPersistence::ForSession));
    return true;
}

// MARK: The life of a view

void webViewCreated(WebKit::WebView* view)
{
    RefPtr proxy = view->page();
    if (!proxy)
        return;
    viewsByPage().set(proxy.get(), view);

    // A page gets the clients that call the application's, which is where
    // the endpoint is told of loads, dialogs and new windows, only when the
    // application sets some. Clients with nothing in them give it those all
    // the same; the application's own replace them when it sets them.
    WKPageNavigationClientV0 navigationClient { };
    WKPageSetPageNavigationClient(WebKit::toAPI(proxy.get()), &navigationClient.base);
    WKPageUIClientV0 uiClient { };
    WKPageSetPageUIClient(WebKit::toAPI(proxy.get()), &uiClient.base);

    auto& server = Server::singleton();
    server.startFromEnvironmentOnce();
    // Views are tracked even while nothing listens, so that an application
    // can start the endpoint after it has created its views.
    server.addView(view);
}

void webViewDestroyed(WebKit::WebView* view)
{
    Server::singleton().removeView(view);
    viewsByPage().removeIf([view](auto& entry) {
        return entry.value == view;
    });
}

bool pageClosed(WebKit::WebPageProxy& proxy)
{
    RefPtr page = Server::singleton().pageForView(viewForPage(proxy));
    if (!page || !std::exchange(page->ownedByServer, false))
        return false;
    // Not from inside what the page is telling.
    runAfter(0, [view = RefPtr { page->view() }] {
        destroyOwnedView(view.get());
    });
    return true;
}

void PageTarget::close(bool force)
{
    if (!m_view)
        return;
    // A view this endpoint owns can simply be destroyed. One that belongs
    // to the application can only be asked, which runs beforeunload; the
    // page's UI client is told to close it when the page agrees.
    if (!force || !std::exchange(ownedByServer, false)) {
        Ref proxy = pageProxy(m_view);
        if (proxy->tryClose())
            proxy->closePage();
        return;
    }
    runAfter(0, [view = RefPtr { m_view }] {
        destroyOwnedView(view.get());
    });
}

// The configuration a page that opens a window hands over for the new view.
static API::PageConfiguration* openedPageConfiguration;

RefPtr<WebKit::WebPageProxy> createPage(WebKit::WebPageProxy& opener, API::PageConfiguration& configuration)
{
    // Only while a client is connected: with nobody to see or close them,
    // such views would pile up for as long as the process lives.
    auto& server = Server::singleton();
    auto openerView = viewForPage(opener);
    if (!openerView || !server.isListening() || !server.hasConnections())
        return nullptr;
    SetForScope scope(openedPageConfiguration, &configuration);
    auto view = server.createView({ }, openerView);
    return view ? view->page() : nullptr;
}

static HWND createOffscreenWindow(int width, int height)
{
    static const wchar_t className[] = L"ZebkitCDPWindow";
    static bool registered;
    if (!std::exchange(registered, true)) {
        WNDCLASSEXW windowClass { };
        windowClass.cbSize = sizeof(windowClass);
        windowClass.hInstance = ::GetModuleHandleW(nullptr);
        windowClass.lpszClassName = className;
        windowClass.lpfnWndProc = ::DefWindowProcW;
        ::RegisterClassExW(&windowClass);
    }
    // Off every screen, yet shown: what is hidden is not painted, and its
    // timers are slowed down. Not in the taskbar, and never the window the
    // user's typing goes to.
    HWND window = ::CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, className, L"", WS_POPUP, -20000, -20000, width, height,
        nullptr, nullptr, ::GetModuleHandleW(nullptr), nullptr);
    if (window)
        ::ShowWindow(window, SW_SHOWNOACTIVATE);
    return window;
}

PlatformView Server::createView(const String& browserContextID, PlatformView related)
{
    SetForScope contextScope(m_creatingContextID, related && pageForView(related) ? pageForView(related)->browserContextID : browserContextID);
    SetForScope openerScope(m_creatingOpenerID, related && pageForView(related) ? pageForView(related)->targetID() : String());

    RefPtr<API::PageConfiguration> configuration;
    if (related)
        configuration = openedPageConfiguration;
    else {
        // Take everything else from a view the application made, so that
        // pages opened over CDP behave like the application's own.
        for (auto& page : m_pages) {
            if (page->browserContextID.isEmpty()) {
                configuration = pageProxy(page->view()).configuration().copy();
                break;
            }
        }
        if (!configuration)
            configuration = API::PageConfiguration::create();
        if (auto networkSession = networkSessionFor(browserContextID))
            configuration->setWebsiteDataStore(networkSession.copyRef());
    }
    if (!configuration)
        return nullptr;

    if (createViewFunc) {
        if (auto view = createViewFunc(WebKit::toAPI(related), WebKit::toAPI(configuration.get()), createViewData))
            return WebKit::toImpl(view);
    }

    HWND window = createOffscreenWindow(defaultViewWidth, defaultViewHeight);
    if (!window)
        return nullptr;
    RECT frame { 0, 0, defaultViewWidth, defaultViewHeight };
    Ref view = WebKit::WebView::create(frame, *configuration, window);
    ownedViews().set(view.ptr(), OwnedView { view.copyRef(), window });

    if (RefPtr page = pageForView(view.ptr()))
        page->ownedByServer = true;
    // The table holds the view from here on.
    return view.ptr();
}

// MARK: Navigation

static Vector<Ref<WebKit::WebBackForwardListItem>> historyItems(WebKit::WebBackForwardList& list, int& currentIndex)
{
    Vector<Ref<WebKit::WebBackForwardListItem>> items;
    for (auto item : list.backList()->elementsOfType<WebKit::WebBackForwardListItem>())
        items.append(item.releaseNonNull());
    currentIndex = -1;
    if (RefPtr current = list.currentItem()) {
        currentIndex = items.size();
        items.append(current.releaseNonNull());
    }
    for (auto item : list.forwardList()->elementsOfType<WebKit::WebBackForwardListItem>())
        items.append(item.releaseNonNull());
    return items;
}

void PageTarget::pageGetNavigationHistory(Session&, JSON::Object&, Reply&& reply)
{
    int currentIndex;
    auto items = historyItems(pageProxy(m_view).backForwardList(), currentIndex);
    auto entries = JSON::Array::create();
    int index = 0;
    for (auto& item : items) {
        auto entry = JSON::Object::create();
        entry->setInteger("id"_s, index++);
        entry->setString("url"_s, item->url());
        entry->setString("userTypedURL"_s, item->originalURL());
        entry->setString("title"_s, item->title().isNull() ? emptyString() : item->title());
        entry->setString("transitionType"_s, "typed"_s);
        entries->pushObject(WTF::move(entry));
    }

    auto result = JSON::Object::create();
    result->setInteger("currentIndex"_s, currentIndex);
    result->setArray("entries"_s, WTF::move(entries));
    reply(WTF::move(result), { });
}

void PageTarget::pageNavigateToHistoryEntry(Session&, JSON::Object& params, Reply&& reply)
{
    // Ids are positions in the list getNavigationHistory returned.
    int currentIndex;
    auto items = historyItems(pageProxy(m_view).backForwardList(), currentIndex);
    int entryID = params.getInteger("entryId"_s).value_or(-1);
    if (entryID < 0 || static_cast<size_t>(entryID) >= items.size()) {
        replyInvalid(reply, "No entry with passed id"_s);
        return;
    }
    pageProxy(m_view).goToBackForwardItem(items[entryID]);
    replyOK(reply);
}

// MARK: Window

void PageTarget::focusView()
{
    HWND window = m_view->window();
    if (window && ::GetFocus() != window)
        ::SetFocus(window);
}

void PageTarget::pageBringToFront(Session&, JSON::Object&, Reply&& reply)
{
    focusView();
    replyOK(reply);
}

// Sizes are the page's, in CSS pixels; windows are measured in the
// screen's. A view that is all there is in its window is resized with it.
static void resizeView(WebKit::WebView& view, int width, int height)
{
    float scale = view.page() ? view.page()->intrinsicDeviceScaleFactor() : 1;
    int pixelWidth = std::lround(width * scale);
    int pixelHeight = std::lround(height * scale);
    auto owned = ownedViews().find(&view);
    if (owned != ownedViews().end())
        ::SetWindowPos(owned->value.window, nullptr, 0, 0, pixelWidth, pixelHeight, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    ::SetWindowPos(view.window(), nullptr, 0, 0, pixelWidth, pixelHeight, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

void PageTarget::emulationSetDeviceMetricsOverride(Session&, JSON::Object& params, Reply&& reply)
{
    int width = params.getInteger("width"_s).value_or(0);
    int height = params.getInteger("height"_s).value_or(0);
    // How many pixels of the screen one of the page's is.
    if (double scale = params.getDouble("deviceScaleFactor"_s).value_or(0); scale > 0) {
        auto& proxy = pageProxy(m_view);
        if (!m_native.overridesScale) {
            m_native.overridesScale = true;
            m_native.originalScale = proxy.customDeviceScaleFactor().value_or(0);
        }
        proxy.setCustomDeviceScaleFactor(scale, [] { });
    }
    if (width <= 0 || height <= 0) {
        replyOK(reply);
        return;
    }
    // The override is a real resize of the view. The page learns of it a
    // little later; answer once it has, so that what the client does next
    // sees the new size.
    if (!m_originalWidth)
        std::tie(m_originalWidth, m_originalHeight) = viewSize(m_view);
    resizeView(*m_view, width, height);
    waitForViewportWidth(width, 40, WTF::move(reply));
}

void PageTarget::restoreViewSize()
{
    int width = std::exchange(m_originalWidth, 0);
    int height = std::exchange(m_originalHeight, 0);
    if (width > 0 && height > 0 && m_view)
        resizeView(*m_view, width, height);
    if (std::exchange(m_native.overridesScale, false) && m_view)
        pageProxy(m_view).setCustomDeviceScaleFactor(m_native.originalScale, [] { });
}

void PageTarget::emulationClearDeviceMetricsOverride(Session&, JSON::Object&, Reply&& reply)
{
    int width = m_originalWidth;
    restoreViewSize();
    if (width > 0)
        waitForViewportWidth(width, 40, WTF::move(reply));
    else
        replyOK(reply);
}

// MARK: Input

// CDP: Alt 1, Ctrl 2, Meta 4, Shift 8.
static OptionSet<WebKit::WebEventModifier> modifiersFromCDP(int modifiers)
{
    OptionSet<WebKit::WebEventModifier> result;
    if (modifiers & 1)
        result.add(WebKit::WebEventModifier::AltKey);
    if (modifiers & 2)
        result.add(WebKit::WebEventModifier::ControlKey);
    if (modifiers & 4)
        result.add(WebKit::WebEventModifier::MetaKey);
    if (modifiers & 8)
        result.add(WebKit::WebEventModifier::ShiftKey);
    return result;
}

// Where a point of the page is on the screen, in the page's pixels.
static WebCore::IntPoint globalPosition(HWND window, float scale, double x, double y)
{
    POINT point { static_cast<LONG>(std::lround(x * scale)), static_cast<LONG>(std::lround(y * scale)) };
    ::ClientToScreen(window, &point);
    return { static_cast<int>(std::lround(point.x / scale)), static_cast<int>(std::lround(point.y / scale)) };
}

void PageTarget::inputDispatchMouseEvent(Session&, JSON::Object& params, Reply&& reply)
{
    HWND window = m_view->window();
    if (!window) {
        replyError(reply, "The view cannot receive input"_s);
        return;
    }
    focusView();

    auto type = params.getString("type"_s);
    double x = params.getDouble("x"_s).value_or(0);
    double y = params.getDouble("y"_s).value_or(0);
    auto buttonName = params.getString("button"_s);
    // Left 1, right 2, middle 4, as CDP's "buttons" has them, and WebKit's.
    unsigned button = buttonName == "left"_s ? 1 : buttonName == "right"_s ? 2 : buttonName == "middle"_s ? 4 : 0;
    auto modifiers = modifiersFromCDP(params.getInteger("modifiers"_s).value_or(0));
    auto& proxy = pageProxy(m_view);
    float scale = proxy.intrinsicDeviceScaleFactor();
    WebCore::IntPoint position { static_cast<int>(std::floor(x)), static_cast<int>(std::floor(y)) };
    auto global = globalPosition(window, scale, x, y);
    LPARAM where = MAKELPARAM(std::lround(x * scale), std::lround(y * scale));

    auto webButton = [](unsigned bit) {
        return bit == 1 ? WebKit::WebMouseEventButton::Left : bit == 2 ? WebKit::WebMouseEventButton::Right
            : bit == 4 ? WebKit::WebMouseEventButton::Middle : WebKit::WebMouseEventButton::None;
    };
    // The event the view makes of a window message, and in it what the
    // client said in place of what a message can say.
    auto send = [&](WebKit::WebEventType eventType, unsigned eventButton, int clickCount) {
        WebKit::NativeWebMouseEvent event(window, WM_MOUSEMOVE, 0, where, false, scale);
        static_cast<WebKit::WebMouseEvent&>(event) = WebKit::WebMouseEvent({ eventType, modifiers, MonotonicTime::now() }, webButton(eventButton),
            static_cast<unsigned short>(m_pressedButtons), position, global, 0, 0, 0, clickCount, 0, WebKit::WebEventInputSource::UserDriven);
        proxy.handleMouseEvent(event);
    };

    if (type == "mouseMoved"_s) {
        // A move with a button down is a drag with that button.
        unsigned held = (m_pressedButtons & 1) ? 1 : (m_pressedButtons & 2) ? 2 : (m_pressedButtons & 4) ? 4 : 0;
        send(WebKit::WebEventType::MouseMove, held, 0);
    } else if (type == "mousePressed"_s && button) {
        m_pressedButtons |= button;
        send(WebKit::WebEventType::MouseDown, button, std::max(1, params.getInteger("clickCount"_s).value_or(1)));
    } else if (type == "mouseReleased"_s && button) {
        m_pressedButtons &= ~button;
        send(WebKit::WebEventType::MouseUp, button, std::max(1, params.getInteger("clickCount"_s).value_or(1)));
    } else if (type == "mouseWheel"_s) {
        // CDP gives the distance to scroll in CSS pixels, down and right
        // positive; WebKit counts the other way. The distance is exact,
        // as from a touchpad, and is scrolled at once, as on the other
        // platforms; a wheel's notches would be animated. A notch is three
        // lines of forty pixels, which is what the ticks count.
        WebCore::FloatSize delta(-params.getDouble("deltaX"_s).value_or(0), -params.getDouble("deltaY"_s).value_or(0));
        POINT onScreen { static_cast<LONG>(std::lround(x * scale)), static_cast<LONG>(std::lround(y * scale)) };
        ::ClientToScreen(window, &onScreen);
        WebKit::NativeWebWheelEvent event(window, WM_MOUSEWHEEL, 0, MAKELPARAM(onScreen.x, onScreen.y), scale);
        static_cast<WebKit::WebWheelEvent&>(event) = WebKit::WebWheelEvent({ WebKit::WebEventType::Wheel, modifiers, MonotonicTime::now() }, position, global,
            delta, WebCore::FloatSize(delta.width() / 120, delta.height() / 120), WebKit::WebWheelEvent::Granularity::ScrollByPixelWheelEvent,
            WebKit::WebWheelEvent::Phase::None, WebKit::WebWheelEvent::Phase::None, true);
        proxy.handleNativeWheelEvent(event);
    } else {
        replyInvalid(reply, makeString("Unexpected event type '"_s, type, '\''));
        return;
    }
    m_lastMouseX = x;
    m_lastMouseY = y;
    whenInputProcessed(WTF::move(reply));
}

// The keys of a keyboard by the name the DOM gives their position ("code"),
// with the number Windows has for the same key (its virtual-key code),
// which is also what a page reads as keyCode.
static constexpr std::pair<ASCIILiteral, int> keyPositions[] = {
    { "Minus"_s, VK_OEM_MINUS }, { "Equal"_s, VK_OEM_PLUS }, { "BracketLeft"_s, VK_OEM_4 }, { "BracketRight"_s, VK_OEM_6 },
    { "Backslash"_s, VK_OEM_5 }, { "Semicolon"_s, VK_OEM_1 }, { "Quote"_s, VK_OEM_7 }, { "Backquote"_s, VK_OEM_3 },
    { "Comma"_s, VK_OEM_COMMA }, { "Period"_s, VK_OEM_PERIOD }, { "Slash"_s, VK_OEM_2 }, { "Space"_s, VK_SPACE },
    { "Enter"_s, VK_RETURN }, { "Tab"_s, VK_TAB }, { "Backspace"_s, VK_BACK }, { "Delete"_s, VK_DELETE }, { "Escape"_s, VK_ESCAPE },
    { "ArrowLeft"_s, VK_LEFT }, { "ArrowRight"_s, VK_RIGHT }, { "ArrowUp"_s, VK_UP }, { "ArrowDown"_s, VK_DOWN },
    { "Home"_s, VK_HOME }, { "End"_s, VK_END }, { "PageUp"_s, VK_PRIOR }, { "PageDown"_s, VK_NEXT }, { "Insert"_s, VK_INSERT },
    { "ShiftLeft"_s, VK_SHIFT }, { "ShiftRight"_s, VK_SHIFT }, { "ControlLeft"_s, VK_CONTROL }, { "ControlRight"_s, VK_CONTROL },
    { "AltLeft"_s, VK_MENU }, { "AltRight"_s, VK_MENU }, { "MetaLeft"_s, VK_LWIN }, { "MetaRight"_s, VK_RWIN },
    { "CapsLock"_s, VK_CAPITAL }, { "ContextMenu"_s, VK_APPS }, { "Pause"_s, VK_PAUSE }, { "ScrollLock"_s, VK_SCROLL },
    { "PrintScreen"_s, VK_SNAPSHOT }, { "NumLock"_s, VK_NUMLOCK },
    { "NumpadAdd"_s, VK_ADD }, { "NumpadSubtract"_s, VK_SUBTRACT }, { "NumpadMultiply"_s, VK_MULTIPLY }, { "NumpadDivide"_s, VK_DIVIDE },
    { "NumpadDecimal"_s, VK_DECIMAL }, { "NumpadEnter"_s, VK_RETURN },
};

// The keys that have a name and no text ("key"), with their position.
static constexpr std::pair<ASCIILiteral, ASCIILiteral> namedKeys[] = {
    { "Enter"_s, "Enter"_s }, { "Tab"_s, "Tab"_s }, { "Backspace"_s, "Backspace"_s }, { "Escape"_s, "Escape"_s },
    { "Delete"_s, "Delete"_s }, { "Insert"_s, "Insert"_s },
    { "ArrowLeft"_s, "ArrowLeft"_s }, { "ArrowUp"_s, "ArrowUp"_s }, { "ArrowRight"_s, "ArrowRight"_s }, { "ArrowDown"_s, "ArrowDown"_s },
    { "Home"_s, "Home"_s }, { "End"_s, "End"_s }, { "PageUp"_s, "PageUp"_s }, { "PageDown"_s, "PageDown"_s },
    { "ContextMenu"_s, "ContextMenu"_s }, { "Pause"_s, "Pause"_s }, { "ScrollLock"_s, "ScrollLock"_s },
    { "PrintScreen"_s, "PrintScreen"_s }, { "NumLock"_s, "NumLock"_s },
    { "F1"_s, "F1"_s }, { "F2"_s, "F2"_s }, { "F3"_s, "F3"_s }, { "F4"_s, "F4"_s }, { "F5"_s, "F5"_s }, { "F6"_s, "F6"_s },
    { "F7"_s, "F7"_s }, { "F8"_s, "F8"_s }, { "F9"_s, "F9"_s }, { "F10"_s, "F10"_s }, { "F11"_s, "F11"_s }, { "F12"_s, "F12"_s },
    { "Shift"_s, "ShiftLeft"_s }, { "Control"_s, "ControlLeft"_s }, { "Alt"_s, "AltLeft"_s }, { "Meta"_s, "MetaLeft"_s },
    { "CapsLock"_s, "CapsLock"_s },
};

// Where a character is on a US keyboard, for a client that names none.
static String codeForCharacter(char32_t character)
{
    static constexpr auto shiftedDigits = ")!@#$%^&*("_s;
    static constexpr std::pair<ASCIILiteral, ASCIILiteral> others[] = {
        { "-_"_s, "Minus"_s }, { "=+"_s, "Equal"_s }, { "[{"_s, "BracketLeft"_s }, { "]}"_s, "BracketRight"_s }, { "\\|"_s, "Backslash"_s },
        { ";:"_s, "Semicolon"_s }, { "'\""_s, "Quote"_s }, { "`~"_s, "Backquote"_s }, { ",<"_s, "Comma"_s }, { ".>"_s, "Period"_s },
        { "/?"_s, "Slash"_s }, { " "_s, "Space"_s },
    };
    if (character > 0x7e)
        return { };
    if (isASCIIAlpha(character))
        return makeString("Key"_s, static_cast<char>(toASCIIUpper(character)));
    if (isASCIIDigit(character))
        return makeString("Digit"_s, static_cast<char>(character));
    auto digits = shiftedDigits.span();
    for (size_t index = 0; index < digits.size(); ++index) {
        if (static_cast<char32_t>(digits[index]) == character)
            return makeString("Digit"_s, static_cast<char>('0' + index));
    }
    for (auto& [characters, code] : others) {
        for (auto candidate : characters.span()) {
            if (static_cast<char32_t>(candidate) == character)
                return code;
        }
    }
    return { };
}

static int virtualKeyForCode(const String& code)
{
    if (code.length() == 4 && code.startsWith("Key"_s) && isASCIIUpper(code[3]))
        return code[3];
    if (code.length() == 6 && code.startsWith("Digit"_s) && isASCIIDigit(code[5]))
        return code[5];
    if (code.length() == 7 && code.startsWith("Numpad"_s) && isASCIIDigit(code[6]))
        return VK_NUMPAD0 + (code[6] - '0');
    if (code.length() >= 2 && code.length() <= 3 && code[0] == 'F') {
        if (auto number = parseInteger<int>(StringView(code).substring(1)); number && *number >= 1 && *number <= 24)
            return VK_F1 + *number - 1;
    }
    for (auto& [name, key] : keyPositions) {
        if (code == name)
            return key;
    }
    return 0;
}

// What old pages read as keyIdentifier.
static String keyIdentifierFor(const String& key, int virtualKey)
{
    static constexpr std::pair<ASCIILiteral, ASCIILiteral> names[] = {
        { "ArrowLeft"_s, "Left"_s }, { "ArrowRight"_s, "Right"_s }, { "ArrowUp"_s, "Up"_s }, { "ArrowDown"_s, "Down"_s },
        { "Escape"_s, "U+001B"_s }, { "Backspace"_s, "U+0008"_s }, { "Tab"_s, "U+0009"_s }, { "Delete"_s, "U+007F"_s },
    };
    for (auto& [name, identifier] : names) {
        if (key == name)
            return identifier;
    }
    if (key.length() > 1)
        return key;
    return makeString("U+"_s, hex(static_cast<unsigned>(toASCIIUpper(virtualKey)), 4));
}

void PageTarget::inputInsertText(Session&, JSON::Object& params, Reply&& reply)
{
    focusView();
    pageProxy(m_view).executeEditCommand("InsertText"_s, params.getString("text"_s));
    whenInputProcessed(WTF::move(reply));
}

void PageTarget::inputDispatchKeyEvent(Session& session, JSON::Object& params, Reply&& reply)
{
    HWND window = m_view->window();
    if (!window) {
        replyError(reply, "The view cannot receive input"_s);
        return;
    }
    focusView();

    auto type = params.getString("type"_s);
    auto key = params.getString("key"_s);
    auto text = params.getString("text"_s);
    auto code = params.getString("code"_s);
    if (type != "keyDown"_s && type != "keyUp"_s && type != "rawKeyDown"_s && type != "char"_s) {
        replyInvalid(reply, makeString("Unexpected event type '"_s, type, '\''));
        return;
    }
    if (type == "char"_s) {
        // Text with no key behind it, as from an input method.
        inputInsertText(session, params, WTF::move(reply));
        return;
    }
    bool isDown = type != "keyUp"_s;
    auto modifiers = modifiersFromCDP(params.getInteger("modifiers"_s).value_or(0));

    bool isNamed = false;
    for (auto& [name, position] : namedKeys) {
        if (key != name)
            continue;
        isNamed = true;
        if (code.isEmpty())
            code = position;
    }
    if (!isNamed) {
        char32_t character = singleCodePoint(text);
        if (character < 0x20 || character == 0x7f)
            character = singleCodePoint(key);
        if (character < 0x20 || character == 0x7f) {
            replyError(reply, makeString("Unknown key: \""_s, key, '"'));
            return;
        }
        if (code.isEmpty())
            code = codeForCharacter(character);
    }
    // A client that knows the key's number says it; one that does not is
    // given the number of the key's place on a US keyboard.
    int virtualKey = params.getInteger("windowsVirtualKeyCode"_s).value_or(0);
    if (!virtualKey)
        virtualKey = virtualKeyForCode(code);

    // What Windows sends for a key is a message that it went down and, if
    // the key types something, a second one with the character. The page
    // gets keydown from the first and keypress, and the text, from the
    // second. Here the two are made with what the client said.
    auto& proxy = pageProxy(m_view);
    auto send = [&](WebKit::WebEventType eventType, UINT message, const String& eventText, int keyCode) {
        WebKit::NativeWebKeyboardEvent event(window, message, keyCode, 0, { });
        static_cast<WebKit::WebKeyboardEvent&>(event) = WebKit::WebKeyboardEvent({ eventType, modifiers, MonotonicTime::now() }, eventText, eventText,
            key, code, keyIdentifierFor(key, virtualKey), eventType == WebKit::WebEventType::Char ? 0 : virtualKey, keyCode, 0,
            params.getBoolean("autoRepeat"_s).value_or(false), code.startsWith("Numpad"_s), false);
        proxy.handleKeyboardEvent(event);
    };
    if (!isDown)
        send(WebKit::WebEventType::KeyUp, WM_KEYUP, emptyString(), virtualKey);
    else {
        send(WebKit::WebEventType::RawKeyDown, WM_KEYDOWN, emptyString(), virtualKey);
        if (type == "keyDown"_s && !text.isEmpty())
            send(WebKit::WebEventType::Char, WM_CHAR, text, text[0]);
        // What a key does to the text besides typing, where the client
        // names it (as it does to Chrome, for the keys of a Mac).
        if (auto names = params.getArray("commands"_s)) {
            for (auto& value : *names) {
                if (auto name = value->asString(); !name.isEmpty())
                    proxy.executeEditCommand(name);
            }
        }
    }
    whenInputProcessed(WTF::move(reply));
}

// MARK: What the portable code asks of a view

WebKit::WebPageProxy& pageProxy(PlatformView view)
{
    return *view->page();
}

String viewTitle(PlatformView view)
{
    auto& title = pageProxy(view).pageLoadState().title();
    return title.isNull() ? emptyString() : title;
}

String viewURI(PlatformView view)
{
    return pageProxy(view).pageLoadState().activeURL().string();
}

void loadURI(PlatformView view, const String& url)
{
    pageProxy(view).loadRequest(WebCore::ResourceRequest { URL { url } });
}

void stopLoading(PlatformView view)
{
    pageProxy(view).stopLoading();
}

void terminateWebProcess(PlatformView view)
{
    Ref { pageProxy(view).legacyMainFrameProcess() }->terminate();
}

void reloadView(PlatformView view, bool bypassCache)
{
    OptionSet<WebCore::ReloadOption> options;
    if (bypassCache)
        options.add(WebCore::ReloadOption::FromOrigin);
    pageProxy(view).reload(options);
}

std::pair<int, int> viewSize(PlatformView view)
{
    auto size = view->viewSize();
    return { size.width(), size.height() };
}

String engineVersion()
{
#ifdef ZEBKIT_ENGINE_VERSION
    return String::fromUTF8(ZEBKIT_ENGINE_VERSION);
#else
    return "0"_s;
#endif
}

} // namespace Zebkit::CDP

#endif // PLATFORM(WIN)
