/*
 * The platform layer for macOS (see CDPPlatform.h): everything about a page
 * that goes through the view and not through the inspector. The view is a
 * WKWebView. Navigation, input, window size, dialogs, file choosers, the
 * life of the view, cookies and the creation of views.
 *
 * A WKWebView tells what happens to it to the delegates of the application,
 * and has one of each, so the endpoint cannot take their place. It is told
 * from inside WebKit instead, where the delegates are called (the functions
 * declared in CDP.h, called by patches/2009). Title and address are
 * observed like any other property.
 *
 * Input is delivered as NSEvents to the view, the path a real mouse and
 * keyboard take, so pages see trusted events and default actions (focus,
 * text entry, link activation) happen.
 *
 * What is the same on every platform, such as the bookkeeping of a
 * navigation and the order of input answers, is in CDPView.cpp.
 */

#import "config.h"
#import "CDPInternal.h"

#import "APIHTTPCookieStore.h"
#import "APIPageConfiguration.h"
#import "AuthenticationChallengeProxy.h"
#import "AuthenticationDecisionListener.h"
#import "NativeWebKeyboardEvent.h"
#import "WKBackForwardList.h"
#import "WKBackForwardListItem.h"
#import "WKWebViewConfigurationInternal.h"
#import "WKWebViewInternal.h"
#import "WKWebViewPrivate.h"
#import "WKWebsiteDataStoreInternal.h"
#import "WebOpenPanelResultListenerProxy.h"
#import "WebPageProxy.h"
#import "WebProcessMessages.h"
#import "WebProcessPool.h"
#import <Carbon/Carbon.h>
#import <WebCore/Credential.h>
#import <WebCore/KeypressCommand.h>
#import <WebCore/ResourceError.h>
#import <wtf/CallbackAggregator.h>
#import <wtf/NeverDestroyed.h>
#import <wtf/URL.h>
#import <wtf/WallTime.h>
#import <wtf/text/MakeString.h>

using Zebkit::CDP::PageTarget;

// Tells a page of the changes to its view's title and address.
@interface ZebkitCDPViewObserver : NSObject
- (instancetype)initWithPage:(PageTarget&)page;
@end

@implementation ZebkitCDPViewObserver {
    WeakPtr<PageTarget> _page;
}

- (instancetype)initWithPage:(PageTarget&)page
{
    if ((self = [super init]))
        _page = page;
    return self;
}

- (void)observeValueForKeyPath:(NSString *)keyPath ofObject:(id)object change:(NSDictionary *)change context:(void *)context
{
    RefPtr page = _page.get();
    if (!page)
        return;
    page->infoChanged();
    if ([keyPath isEqualToString:@"URL"])
        page->addressChanged();
}

@end

// The window of a view the endpoint made. Nobody looks at it: it is there
// because a page lays out, paints and takes input only in a window, and it
// says it is the key window so that the page has the focus whichever
// window the user is typing into.
@interface ZebkitCDPWindow : NSWindow
@end

@implementation ZebkitCDPWindow

- (BOOL)isKeyWindow
{
    return YES;
}

- (BOOL)canBecomeKeyWindow
{
    return YES;
}

@end

@interface NSApplication (ZebkitCDPCurrentEvent)
- (void)_setCurrentEvent:(NSEvent *)event;
@end

namespace Zebkit::CDP {

static constexpr int defaultViewWidth = 1280;
static constexpr int defaultViewHeight = 720;

static HashMap<WKWebView *, RetainPtr<NSWindow>>& ownedWindows()
{
    static NeverDestroyed<HashMap<WKWebView *, RetainPtr<NSWindow>>> windows;
    return windows;
}

// Lets go of a view the endpoint made. The view goes with its window.
static void destroyOwnedView(WKWebView *view)
{
    if (auto window = ownedWindows().take(view)) {
        [window orderOut:nil];
        [window setContentView:nil];
    }
}

static RefPtr<PageTarget> pageFor(WebKit::WebPageProxy& proxy)
{
    auto view = proxy.cocoaView();
    return view ? Server::singleton().pageForView(view.get()) : nullptr;
}

// MARK: The view's notifications

void PageTarget::connectViewSignals()
{
    m_native.observer = adoptNS([[ZebkitCDPViewObserver alloc] initWithPage:*this]);
    for (NSString *key in @[ @"URL", @"title" ])
        [m_view addObserver:m_native.observer.get() forKeyPath:key options:0 context:nullptr];
}

void PageTarget::releaseView()
{
    if (auto observer = std::exchange(m_native.observer, nullptr)) {
        for (NSString *key in @[ @"URL", @"title" ])
            [m_view removeObserver:observer.get() forKeyPath:key];
    }
    m_native.dialog = nullptr;
    ownedWindows().remove(m_view);
}

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

void loadFailed(WebKit::WebPageProxy& proxy, const WebCore::ResourceError& error)
{
    RefPtr page = pageFor(proxy);
    if (!page)
        return;
    int code = error.errorCode();
    bool isURLError = error.domain() == String(NSURLErrorDomain);
    if (error.isCancellation() || (isURLError && code == NSURLErrorCancelled)) {
        page->loadCancelled();
        return;
    }
    if (logLevel())
        WTFLogAlways("cdp: load failed: %s %d: %s", error.domain().utf8().data(), code, error.localizedDescription().utf8().data());

    ASCIILiteral text = "net::ERR_FAILED"_s;
    if (!isURLError) {
        // WebKit's own errors: a policy decision stopped the load.
        text = "net::ERR_ABORTED"_s;
    } else if (code == NSURLErrorCannotFindHost || code == NSURLErrorDNSLookupFailed)
        text = "net::ERR_NAME_NOT_RESOLVED"_s;
    else if (code == NSURLErrorCannotConnectToHost)
        text = "net::ERR_CONNECTION_REFUSED"_s;
    else if (code == NSURLErrorTimedOut)
        text = "net::ERR_TIMED_OUT"_s;
    else if (code == NSURLErrorNotConnectedToInternet)
        text = "net::ERR_ADDRESS_UNREACHABLE"_s;
    else if (code == NSURLErrorNetworkConnectionLost)
        text = "net::ERR_CONNECTION_CLOSED"_s;
    else if (code == NSURLErrorSecureConnectionFailed)
        text = "net::ERR_SSL_PROTOCOL_ERROR"_s;
    else if (code <= NSURLErrorServerCertificateHasBadDate && code >= NSURLErrorClientCertificateRequired)
        text = "net::ERR_CERT_AUTHORITY_INVALID"_s;
    else if (code == NSURLErrorFileDoesNotExist)
        text = "net::ERR_FILE_NOT_FOUND"_s;
    else if (code == NSURLErrorUnsupportedURL)
        text = "net::ERR_UNKNOWN_URL_SCHEME"_s;
    // The load is over as well as failed.
    page->loadFailed(text);
    page->loadChanged(LoadEvent::Finished);
}

void processTerminated(WebKit::WebPageProxy& proxy)
{
    if (RefPtr page = pageFor(proxy))
        page->processTerminated();
}

bool answerChallenge(WebKit::WebPageProxy& proxy, WebKit::AuthenticationChallengeProxy& challenge)
{
    RefPtr page = pageFor(proxy);
    if (!page || !page->native().ignoresCertificateErrors)
        return false;
    RetainPtr space = challenge.core().protectionSpace().nsSpace();
    if (![[space authenticationMethod] isEqualToString:NSURLAuthenticationMethodServerTrust] || ![space serverTrust])
        return false;
    challenge.listener().completeChallenge(WebKit::AuthenticationChallengeDisposition::UseCredential,
        WebCore::Credential([NSURLCredential credentialForTrust:[space serverTrust]]));
    return true;
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

// MARK: The life of a view

void webViewCreated(WKWebView *view)
{
    // A view gets the client that calls its UI delegate, which is where the
    // endpoint is told of dialogs and new windows, only when a delegate is
    // set. Setting none gives it that client all the same.
    [view setUIDelegate:nil];

    auto& server = Server::singleton();
    server.startFromEnvironmentOnce();
    // Views are tracked even while nothing listens, so that an application
    // can start the endpoint after it has created its views.
    server.addView(view);
}

void webViewDestroyed(WKWebView *view)
{
    Server::singleton().removeView(view);
}

bool pageClosed(WebKit::WebPageProxy& proxy)
{
    RefPtr page = pageFor(proxy);
    if (!page || !std::exchange(page->ownedByServer, false))
        return false;
    // Not from inside what the view is telling.
    runAfter(0, [view = RetainPtr { page->view() }] {
        destroyOwnedView(view.get());
    });
    return true;
}

void PageTarget::close(bool force)
{
    if (!m_view)
        return;
    // A view this endpoint owns can simply be destroyed. One that belongs
    // to the application can only be asked, which runs beforeunload.
    if (!force || !std::exchange(ownedByServer, false)) {
        [m_view _tryClose];
        return;
    }
    runAfter(0, [view = RetainPtr { m_view }] {
        destroyOwnedView(view.get());
    });
}

// The configuration a page that opens a window hands over for the new view.
static WKWebViewConfiguration *openedPageConfiguration;

RefPtr<WebKit::WebPageProxy> createPage(WebKit::WebPageProxy& opener, API::PageConfiguration& configuration)
{
    // Only while a client is connected: with nobody to see or close them,
    // such views would pile up for as long as the process lives.
    auto& server = Server::singleton();
    auto openerView = opener.cocoaView();
    if (!openerView || !server.isListening() || !server.hasConnections())
        return nullptr;
    SetForScope scope(openedPageConfiguration, WebKit::wrapper(configuration));
    WKWebView *view = server.createView({ }, openerView.get());
    return view ? view->_page.get() : nullptr;
}

String Server::userAgent()
{
    if (!m_pages.isEmpty())
        return pageProxy(m_pages.first()->view()).userAgent();
    return WebKit::WebPageProxy::standardUserAgent();
}

PlatformView Server::createView(const String& browserContextID, PlatformView related)
{
    SetForScope contextScope(m_creatingContextID, related && pageForView(related) ? pageForView(related)->browserContextID : browserContextID);
    SetForScope openerScope(m_creatingOpenerID, related && pageForView(related) ? pageForView(related)->targetID() : String());

    RetainPtr<WKWebViewConfiguration> configuration;
    if (related)
        configuration = openedPageConfiguration;
    else {
        // Take everything else from a view the application made, so that
        // pages opened over CDP behave like the application's own.
        for (auto& page : m_pages) {
            if (page->browserContextID.isEmpty()) {
                configuration = [page->view() configuration];
                break;
            }
        }
        if (!configuration)
            configuration = adoptNS([[WKWebViewConfiguration alloc] init]);
        if (auto networkSession = networkSessionFor(browserContextID))
            [configuration setWebsiteDataStore:WebKit::wrapper(*networkSession)];
    }
    if (!configuration)
        return nullptr;

    if (createViewFunc) {
        if (auto* view = createViewFunc(related, configuration.get(), createViewData))
            return view;
    }

    // Off every screen, yet in the window list: what is not there is not
    // painted, and its timers are slowed down.
    auto frame = NSMakeRect(0, 0, defaultViewWidth, defaultViewHeight);
    auto window = adoptNS([[ZebkitCDPWindow alloc] initWithContentRect:NSOffsetRect(frame, -20000, -20000) styleMask:NSWindowStyleMaskBorderless backing:NSBackingStoreBuffered defer:NO]);
    [window setReleasedWhenClosed:NO];
    [window setExcludedFromWindowsMenu:YES];
    auto view = adoptNS([[WKWebView alloc] initWithFrame:frame configuration:configuration.get()]);
    if (!view)
        return nullptr;
    [view _setWindowOcclusionDetectionEnabled:NO];
    [window setContentView:view.get()];
    [window orderBack:nil];
    [window makeFirstResponder:view.get()];
    ownedWindows().set(view.get(), WTF::move(window));

    if (RefPtr page = pageForView(view.get()))
        page->ownedByServer = true;
    // The window holds the view from here on.
    return view.autorelease();
}

// MARK: Navigation

void PageTarget::pageGetNavigationHistory(Session&, JSON::Object&, Reply&& reply)
{
    WKBackForwardList *list = [m_view backForwardList];
    auto entries = JSON::Array::create();
    int index = 0;
    auto add = [&](WKBackForwardListItem *item) {
        auto entry = JSON::Object::create();
        entry->setInteger("id"_s, index++);
        entry->setString("url"_s, String(item.URL.absoluteString));
        entry->setString("userTypedURL"_s, String(item.initialURL.absoluteString));
        entry->setString("title"_s, item.title ? String(item.title) : emptyString());
        entry->setString("transitionType"_s, "typed"_s);
        entries->pushObject(WTF::move(entry));
    };
    for (WKBackForwardListItem *item in list.backList)
        add(item);
    int currentIndex = index;
    if (list.currentItem)
        add(list.currentItem);
    else
        currentIndex = -1;
    for (WKBackForwardListItem *item in list.forwardList)
        add(item);

    auto result = JSON::Object::create();
    result->setInteger("currentIndex"_s, currentIndex);
    result->setArray("entries"_s, WTF::move(entries));
    reply(WTF::move(result), { });
}

void PageTarget::pageNavigateToHistoryEntry(Session&, JSON::Object& params, Reply&& reply)
{
    WKBackForwardList *list = [m_view backForwardList];
    int entryID = params.getInteger("entryId"_s).value_or(-1);
    // Ids are positions in the list getNavigationHistory returned; the API
    // counts from the current item instead.
    WKBackForwardListItem *item = entryID < 0 ? nil : [list itemAtIndex:entryID - static_cast<int>(list.backList.count)];
    if (!item) {
        replyInvalid(reply, "No entry with passed id"_s);
        return;
    }
    [m_view goToBackForwardListItem:item];
    replyOK(reply);
}

// MARK: Window

void PageTarget::focusView()
{
    NSWindow *window = [m_view window];
    if (window && window.firstResponder != m_view)
        [window makeFirstResponder:m_view];
}

void PageTarget::pageBringToFront(Session&, JSON::Object&, Reply&& reply)
{
    focusView();
    replyOK(reply);
}

// A view that is all there is in its window is resized with the window.
static void resizeView(WKWebView *view, int width, int height)
{
    NSWindow *window = view.window;
    if (window && window.contentView == view)
        [window setContentSize:NSMakeSize(width, height)];
    else
        [view setFrameSize:NSMakeSize(width, height)];
}

void PageTarget::emulationSetDeviceMetricsOverride(Session&, JSON::Object& params, Reply&& reply)
{
    int width = params.getInteger("width"_s).value_or(0);
    int height = params.getInteger("height"_s).value_or(0);
    // How many pixels of the screen one of the page's is. A Mac's screen has
    // two as a rule, and a picture of the page then twice the size asked for.
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
    resizeView(m_view, width, height);
    waitForViewportWidth(width, 40, WTF::move(reply));
}

void PageTarget::restoreViewSize()
{
    int width = std::exchange(m_originalWidth, 0);
    int height = std::exchange(m_originalHeight, 0);
    if (width > 0 && height > 0 && m_view)
        resizeView(m_view, width, height);
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
static NSEventModifierFlags modifiersFromCDP(int modifiers)
{
    NSEventModifierFlags result = 0;
    if (modifiers & 1)
        result |= NSEventModifierFlagOption;
    if (modifiers & 2)
        result |= NSEventModifierFlagControl;
    if (modifiers & 4)
        result |= NSEventModifierFlagCommand;
    if (modifiers & 8)
        result |= NSEventModifierFlagShift;
    return result;
}

// Hands an event to the view as the application's event loop would have,
// with the event current while the view handles it.
static void deliver(NSEvent *event, NOESCAPE const Function<void()>& toView)
{
    bool canSetCurrent = [NSApp respondsToSelector:@selector(_setCurrentEvent:)];
    if (canSetCurrent)
        [NSApp _setCurrentEvent:event];
    toView();
    if (canSetCurrent)
        [NSApp _setCurrentEvent:nil];
}

void PageTarget::inputDispatchMouseEvent(Session&, JSON::Object& params, Reply&& reply)
{
    NSWindow *window = [m_view window];
    if (!window) {
        replyError(reply, "The view cannot receive input"_s);
        return;
    }
    focusView();

    auto type = params.getString("type"_s);
    double x = params.getDouble("x"_s).value_or(0);
    double y = params.getDouble("y"_s).value_or(0);
    auto buttonName = params.getString("button"_s);
    // Left 1, right 2, middle 4, as CDP's "buttons" has them.
    unsigned button = buttonName == "left"_s ? 1 : buttonName == "right"_s ? 2 : buttonName == "middle"_s ? 4 : 0;
    auto modifiers = modifiersFromCDP(params.getInteger("modifiers"_s).value_or(0));
    // The view's coordinates start at its top left corner, like the page's.
    NSPoint location = [m_view convertPoint:NSMakePoint(x, y) toView:nil];
    NSTimeInterval time = NSProcessInfo.processInfo.systemUptime;
    WKWebView *view = m_view;

    auto mouseEvent = [&](NSEventType eventType, NSInteger clickCount, float pressure) {
        return [NSEvent mouseEventWithType:eventType location:location modifierFlags:modifiers timestamp:time
            windowNumber:window.windowNumber context:nil eventNumber:0 clickCount:clickCount pressure:pressure];
    };

    if (type == "mouseMoved"_s) {
        if (m_pressedButtons & 1) {
            NSEvent *event = mouseEvent(NSEventTypeLeftMouseDragged, 0, 1);
            deliver(event, [&] { [view mouseDragged:event]; });
        } else if (m_pressedButtons & 2) {
            NSEvent *event = mouseEvent(NSEventTypeRightMouseDragged, 0, 1);
            deliver(event, [&] { [view rightMouseDragged:event]; });
        } else if (m_pressedButtons & 4) {
            NSEvent *event = mouseEvent(NSEventTypeOtherMouseDragged, 0, 1);
            deliver(event, [&] { [view otherMouseDragged:event]; });
        } else {
            NSEvent *event = mouseEvent(NSEventTypeMouseMoved, 0, 0);
            deliver(event, [&] { [view mouseMoved:event]; });
        }
    } else if (type == "mousePressed"_s && button) {
        m_pressedButtons |= button;
        int clickCount = std::max(1, params.getInteger("clickCount"_s).value_or(1));
        if (button == 1) {
            NSEvent *event = mouseEvent(NSEventTypeLeftMouseDown, clickCount, 1);
            deliver(event, [&] { [view mouseDown:event]; });
        } else if (button == 2) {
            NSEvent *event = mouseEvent(NSEventTypeRightMouseDown, clickCount, 1);
            deliver(event, [&] { [view rightMouseDown:event]; });
        } else {
            NSEvent *event = mouseEvent(NSEventTypeOtherMouseDown, clickCount, 1);
            deliver(event, [&] { [view otherMouseDown:event]; });
        }
    } else if (type == "mouseReleased"_s && button) {
        m_pressedButtons &= ~button;
        int clickCount = std::max(1, params.getInteger("clickCount"_s).value_or(1));
        if (button == 1) {
            NSEvent *event = mouseEvent(NSEventTypeLeftMouseUp, clickCount, 0);
            deliver(event, [&] { [view mouseUp:event]; });
        } else if (button == 2) {
            NSEvent *event = mouseEvent(NSEventTypeRightMouseUp, clickCount, 0);
            deliver(event, [&] { [view rightMouseUp:event]; });
        } else {
            NSEvent *event = mouseEvent(NSEventTypeOtherMouseUp, clickCount, 0);
            deliver(event, [&] { [view otherMouseUp:event]; });
        }
    } else if (type == "mouseWheel"_s) {
        // CDP gives the distance to scroll in CSS pixels, down and right
        // positive; the platform counts the other way. An event made this
        // way has no window, and its position is then taken to be on the
        // screen, measured from the top of the first one.
        auto wheel = adoptCF(CGEventCreateScrollWheelEvent2(nullptr, kCGScrollEventUnitPixel, 2,
            static_cast<int32_t>(-params.getDouble("deltaY"_s).value_or(0)), static_cast<int32_t>(-params.getDouble("deltaX"_s).value_or(0)), 0));
        NSPoint onScreen = [window convertPointToScreen:location];
        CGEventSetLocation(wheel.get(), CGPointMake(onScreen.x, NSMaxY(NSScreen.screens.firstObject.frame) - onScreen.y));
        CGEventSetFlags(wheel.get(), static_cast<CGEventFlags>(modifiers));
        NSEvent *event = [NSEvent eventWithCGEvent:wheel.get()];
        deliver(event, [&] { [view scrollWheel:event]; });
    } else {
        replyInvalid(reply, makeString("Unexpected event type '"_s, type, '\''));
        return;
    }
    m_lastMouseX = x;
    m_lastMouseY = y;
    whenInputProcessed(WTF::move(reply));
}

// The keys of a keyboard by the name the DOM gives their position ("code"),
// with the number macOS has for the same key.
static constexpr std::pair<ASCIILiteral, unsigned short> keyPositions[] = {
    { "KeyA"_s, kVK_ANSI_A }, { "KeyB"_s, kVK_ANSI_B }, { "KeyC"_s, kVK_ANSI_C }, { "KeyD"_s, kVK_ANSI_D }, { "KeyE"_s, kVK_ANSI_E },
    { "KeyF"_s, kVK_ANSI_F }, { "KeyG"_s, kVK_ANSI_G }, { "KeyH"_s, kVK_ANSI_H }, { "KeyI"_s, kVK_ANSI_I }, { "KeyJ"_s, kVK_ANSI_J },
    { "KeyK"_s, kVK_ANSI_K }, { "KeyL"_s, kVK_ANSI_L }, { "KeyM"_s, kVK_ANSI_M }, { "KeyN"_s, kVK_ANSI_N }, { "KeyO"_s, kVK_ANSI_O },
    { "KeyP"_s, kVK_ANSI_P }, { "KeyQ"_s, kVK_ANSI_Q }, { "KeyR"_s, kVK_ANSI_R }, { "KeyS"_s, kVK_ANSI_S }, { "KeyT"_s, kVK_ANSI_T },
    { "KeyU"_s, kVK_ANSI_U }, { "KeyV"_s, kVK_ANSI_V }, { "KeyW"_s, kVK_ANSI_W }, { "KeyX"_s, kVK_ANSI_X }, { "KeyY"_s, kVK_ANSI_Y },
    { "KeyZ"_s, kVK_ANSI_Z },
    { "Digit0"_s, kVK_ANSI_0 }, { "Digit1"_s, kVK_ANSI_1 }, { "Digit2"_s, kVK_ANSI_2 }, { "Digit3"_s, kVK_ANSI_3 }, { "Digit4"_s, kVK_ANSI_4 },
    { "Digit5"_s, kVK_ANSI_5 }, { "Digit6"_s, kVK_ANSI_6 }, { "Digit7"_s, kVK_ANSI_7 }, { "Digit8"_s, kVK_ANSI_8 }, { "Digit9"_s, kVK_ANSI_9 },
    { "Minus"_s, kVK_ANSI_Minus }, { "Equal"_s, kVK_ANSI_Equal }, { "BracketLeft"_s, kVK_ANSI_LeftBracket }, { "BracketRight"_s, kVK_ANSI_RightBracket },
    { "Backslash"_s, kVK_ANSI_Backslash }, { "Semicolon"_s, kVK_ANSI_Semicolon }, { "Quote"_s, kVK_ANSI_Quote }, { "Backquote"_s, kVK_ANSI_Grave },
    { "Comma"_s, kVK_ANSI_Comma }, { "Period"_s, kVK_ANSI_Period }, { "Slash"_s, kVK_ANSI_Slash }, { "Space"_s, kVK_Space },
    { "Enter"_s, kVK_Return }, { "Tab"_s, kVK_Tab }, { "Backspace"_s, kVK_Delete }, { "Delete"_s, kVK_ForwardDelete }, { "Escape"_s, kVK_Escape },
    { "ArrowLeft"_s, kVK_LeftArrow }, { "ArrowRight"_s, kVK_RightArrow }, { "ArrowUp"_s, kVK_UpArrow }, { "ArrowDown"_s, kVK_DownArrow },
    { "Home"_s, kVK_Home }, { "End"_s, kVK_End }, { "PageUp"_s, kVK_PageUp }, { "PageDown"_s, kVK_PageDown }, { "Insert"_s, kVK_Help },
    { "ShiftLeft"_s, kVK_Shift }, { "ShiftRight"_s, kVK_RightShift }, { "ControlLeft"_s, kVK_Control }, { "ControlRight"_s, kVK_RightControl },
    { "AltLeft"_s, kVK_Option }, { "AltRight"_s, kVK_RightOption }, { "MetaLeft"_s, kVK_Command }, { "MetaRight"_s, kVK_RightCommand },
    { "CapsLock"_s, kVK_CapsLock }, { "ContextMenu"_s, 0x6e },
    { "F1"_s, kVK_F1 }, { "F2"_s, kVK_F2 }, { "F3"_s, kVK_F3 }, { "F4"_s, kVK_F4 }, { "F5"_s, kVK_F5 }, { "F6"_s, kVK_F6 },
    { "F7"_s, kVK_F7 }, { "F8"_s, kVK_F8 }, { "F9"_s, kVK_F9 }, { "F10"_s, kVK_F10 }, { "F11"_s, kVK_F11 }, { "F12"_s, kVK_F12 },
    { "Numpad0"_s, kVK_ANSI_Keypad0 }, { "Numpad1"_s, kVK_ANSI_Keypad1 }, { "Numpad2"_s, kVK_ANSI_Keypad2 }, { "Numpad3"_s, kVK_ANSI_Keypad3 },
    { "Numpad4"_s, kVK_ANSI_Keypad4 }, { "Numpad5"_s, kVK_ANSI_Keypad5 }, { "Numpad6"_s, kVK_ANSI_Keypad6 }, { "Numpad7"_s, kVK_ANSI_Keypad7 },
    { "Numpad8"_s, kVK_ANSI_Keypad8 }, { "Numpad9"_s, kVK_ANSI_Keypad9 }, { "NumpadAdd"_s, kVK_ANSI_KeypadPlus }, { "NumpadSubtract"_s, kVK_ANSI_KeypadMinus },
    { "NumpadMultiply"_s, kVK_ANSI_KeypadMultiply }, { "NumpadDivide"_s, kVK_ANSI_KeypadDivide }, { "NumpadDecimal"_s, kVK_ANSI_KeypadDecimal },
    { "NumpadEnter"_s, kVK_ANSI_KeypadEnter }, { "NumLock"_s, kVK_ANSI_KeypadClear },
};

// The keys that have a name and no text ("key"), with their position, and
// with the character AppKit reports for them; 0 for a modifier, which
// reports a change of flags and no character.
struct NamedKey {
    ASCIILiteral name;
    ASCIILiteral code;
    char16_t character;
    NSEventModifierFlags flag;
};
static constexpr NamedKey namedKeys[] = {
    { "Enter"_s, "Enter"_s, '\r', 0 }, { "Tab"_s, "Tab"_s, '\t', 0 }, { "Backspace"_s, "Backspace"_s, 0x7f, 0 }, { "Escape"_s, "Escape"_s, 0x1b, 0 },
    { "Delete"_s, "Delete"_s, NSDeleteFunctionKey, 0 }, { "Insert"_s, "Insert"_s, NSInsertFunctionKey, 0 },
    { "ArrowLeft"_s, "ArrowLeft"_s, NSLeftArrowFunctionKey, 0 }, { "ArrowUp"_s, "ArrowUp"_s, NSUpArrowFunctionKey, 0 },
    { "ArrowRight"_s, "ArrowRight"_s, NSRightArrowFunctionKey, 0 }, { "ArrowDown"_s, "ArrowDown"_s, NSDownArrowFunctionKey, 0 },
    { "Home"_s, "Home"_s, NSHomeFunctionKey, 0 }, { "End"_s, "End"_s, NSEndFunctionKey, 0 },
    { "PageUp"_s, "PageUp"_s, NSPageUpFunctionKey, 0 }, { "PageDown"_s, "PageDown"_s, NSPageDownFunctionKey, 0 },
    { "ContextMenu"_s, "ContextMenu"_s, NSMenuFunctionKey, 0 }, { "Pause"_s, { }, NSPauseFunctionKey, 0 },
    { "ScrollLock"_s, { }, NSScrollLockFunctionKey, 0 }, { "PrintScreen"_s, { }, NSPrintScreenFunctionKey, 0 }, { "NumLock"_s, "NumLock"_s, NSClearLineFunctionKey, 0 },
    { "F1"_s, "F1"_s, NSF1FunctionKey, 0 }, { "F2"_s, "F2"_s, NSF2FunctionKey, 0 }, { "F3"_s, "F3"_s, NSF3FunctionKey, 0 }, { "F4"_s, "F4"_s, NSF4FunctionKey, 0 },
    { "F5"_s, "F5"_s, NSF5FunctionKey, 0 }, { "F6"_s, "F6"_s, NSF6FunctionKey, 0 }, { "F7"_s, "F7"_s, NSF7FunctionKey, 0 }, { "F8"_s, "F8"_s, NSF8FunctionKey, 0 },
    { "F9"_s, "F9"_s, NSF9FunctionKey, 0 }, { "F10"_s, "F10"_s, NSF10FunctionKey, 0 }, { "F11"_s, "F11"_s, NSF11FunctionKey, 0 }, { "F12"_s, "F12"_s, NSF12FunctionKey, 0 },
    { "Shift"_s, "ShiftLeft"_s, 0, NSEventModifierFlagShift }, { "Control"_s, "ControlLeft"_s, 0, NSEventModifierFlagControl },
    { "Alt"_s, "AltLeft"_s, 0, NSEventModifierFlagOption }, { "Meta"_s, "MetaLeft"_s, 0, NSEventModifierFlagCommand },
    { "CapsLock"_s, "CapsLock"_s, 0, NSEventModifierFlagCapsLock },
};

// Where a character is on a US keyboard, for a client that names none.
static ASCIILiteral codeForCharacter(char32_t character)
{
    // Each row of characters is on the keys of the list after it.
    static constexpr std::array rows { "abcdefghijklmnopqrstuvwxyz"_s, "ABCDEFGHIJKLMNOPQRSTUVWXYZ"_s, "0123456789"_s, ")!@#$%^&*("_s };
    static constexpr std::pair<ASCIILiteral, ASCIILiteral> others[] = {
        { "-_"_s, "Minus"_s }, { "=+"_s, "Equal"_s }, { "[{"_s, "BracketLeft"_s }, { "]}"_s, "BracketRight"_s }, { "\\|"_s, "Backslash"_s },
        { ";:"_s, "Semicolon"_s }, { "'\""_s, "Quote"_s }, { "`~"_s, "Backquote"_s }, { ",<"_s, "Comma"_s }, { ".>"_s, "Period"_s },
        { "/?"_s, "Slash"_s }, { " "_s, "Space"_s },
    };
    static constexpr std::array letters {
        "KeyA"_s, "KeyB"_s, "KeyC"_s, "KeyD"_s, "KeyE"_s, "KeyF"_s, "KeyG"_s, "KeyH"_s, "KeyI"_s, "KeyJ"_s, "KeyK"_s, "KeyL"_s, "KeyM"_s,
        "KeyN"_s, "KeyO"_s, "KeyP"_s, "KeyQ"_s, "KeyR"_s, "KeyS"_s, "KeyT"_s, "KeyU"_s, "KeyV"_s, "KeyW"_s, "KeyX"_s, "KeyY"_s, "KeyZ"_s,
    };
    static constexpr std::array digits {
        "Digit0"_s, "Digit1"_s, "Digit2"_s, "Digit3"_s, "Digit4"_s, "Digit5"_s, "Digit6"_s, "Digit7"_s, "Digit8"_s, "Digit9"_s,
    };
    if (character > 0x7e)
        return { };
    for (size_t row = 0; row < rows.size(); ++row) {
        auto characters = rows[row].span();
        for (size_t index = 0; index < characters.size(); ++index) {
            if (static_cast<char32_t>(characters[index]) == character)
                return row < 2 ? letters[index] : digits[index];
        }
    }
    for (auto& [characters, code] : others) {
        for (auto candidate : characters.span()) {
            if (static_cast<char32_t>(candidate) == character)
                return code;
        }
    }
    return { };
}

static std::optional<unsigned short> keyCodeForCode(const String& code)
{
    for (auto& [name, keyCode] : keyPositions) {
        if (code == name)
            return keyCode;
    }
    return std::nullopt;
}

void PageTarget::inputInsertText(Session&, JSON::Object& params, Reply&& reply)
{
    focusView();
    pageProxy(m_view).executeEditCommand("InsertText"_s, params.getString("text"_s));
    whenInputProcessed(WTF::move(reply));
}

void PageTarget::inputDispatchKeyEvent(Session& session, JSON::Object& params, Reply&& reply)
{
    NSWindow *window = [m_view window];
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
    NSTimeInterval time = NSProcessInfo.processInfo.systemUptime;
    WKWebView *view = m_view;

    // The platform event carries the characters the key gives, from which
    // WebKit works out the key name and the text to insert, and the
    // position of the key, which becomes the "code".
    const NamedKey* named = nullptr;
    for (auto& candidate : namedKeys) {
        if (key == candidate.name)
            named = &candidate;
    }
    String characters;
    if (named) {
        if (named->character)
            characters = singleElementSpan(named->character);
        if (code.isEmpty())
            code = named->code;
    } else {
        char32_t character = singleCodePoint(text);
        if (character < 0x20 || character == 0x7f)
            character = singleCodePoint(key);
        if (character < 0x20 || character == 0x7f) {
            replyError(reply, makeString("Unknown key: \""_s, key, '"'));
            return;
        }
        characters = singleCodePoint(text) == character ? text : key;
        if (code.isEmpty())
            code = codeForCharacter(character);
    }
    unsigned short keyCode = keyCodeForCode(code).value_or(0);

    if (named && named->flag) {
        // A modifier going down or up is a change of flags and nothing else.
        auto flags = isDown ? (modifiers | named->flag) : (modifiers & ~named->flag);
        NSEvent *event = [NSEvent keyEventWithType:NSEventTypeFlagsChanged location:NSZeroPoint modifierFlags:flags timestamp:time
            windowNumber:window.windowNumber context:nil characters:@"" charactersIgnoringModifiers:@"" isARepeat:NO keyCode:keyCode];
        deliver(event, [&] { [view flagsChanged:event]; });
        whenInputProcessed(WTF::move(reply));
        return;
    }

    // Arrow keys and their like say so in the flags as well.
    if (named && named->character >= 0xf700)
        modifiers |= NSEventModifierFlagFunction;
    RetainPtr nsCharacters = characters.createNSString();
    NSEvent *event = [NSEvent keyEventWithType:isDown ? NSEventTypeKeyDown : NSEventTypeKeyUp location:NSZeroPoint modifierFlags:modifiers timestamp:time
        windowNumber:window.windowNumber context:nil characters:nsCharacters.get() charactersIgnoringModifiers:nsCharacters.get()
        isARepeat:params.getBoolean("autoRepeat"_s).value_or(false) keyCode:keyCode];

    // What a key does to the text besides typing (Select All, a move to the
    // start of the line) the system works out from the user's key bindings
    // and the application's menu, which a synthesized key does not reach.
    // A client that knows says so with the key, as it does to Chrome, and
    // the page is then given the key with exactly those commands.
    Vector<WebCore::KeypressCommand> commands;
    if (auto names = params.getArray("commands"_s); names && isDown) {
        for (auto& value : *names) {
            // Chrome's names are its own editor's ("SelectAll"); the system's
            // are the methods a text view has for the same ("selectAll:").
            auto name = value->asString();
            if (name.isEmpty() || !isASCIIAlpha(name[0]))
                continue;
            name = makeString(toASCIILower(name[0]), name.substring(1), name.endsWith(':') ? ""_s : ":"_s);
            commands.append(WebCore::KeypressCommand { name });
        }
    }
    if (!commands.isEmpty())
        pageProxy(m_view).handleKeyboardEvent(WebKit::NativeWebKeyboardEvent(event, false, false, commands));
    else {
        deliver(event, [&] {
            if (isDown)
                [view keyDown:event];
            else
                [view keyUp:event];
        });
    }
    whenInputProcessed(WTF::move(reply));
}

// MARK: What the portable code asks of a view

WebKit::WebPageProxy& pageProxy(PlatformView view)
{
    return *view->_page;
}

String viewTitle(PlatformView view)
{
    return view.title ? String(view.title) : emptyString();
}

String viewURI(PlatformView view)
{
    return view.URL ? String(view.URL.absoluteString) : emptyString();
}

void loadURI(PlatformView view, const String& url)
{
    [view loadRequest:[NSURLRequest requestWithURL:URL { url }.createNSURL().get()]];
}

void stopLoading(PlatformView view)
{
    [view stopLoading];
}

void terminateWebProcess(PlatformView view)
{
    [view _killWebContentProcess];
}

void reloadView(PlatformView view, bool bypassCache)
{
    // Through the API, which also works when the page's process has died.
    if (bypassCache)
        [view reloadFromOrigin];
    else
        [view reload];
}

std::pair<int, int> viewSize(PlatformView view)
{
    auto size = view.bounds.size;
    return { static_cast<int>(size.width), static_cast<int>(size.height) };
}

String engineVersion()
{
    NSString *version = [[NSBundle bundleForClass:[WKWebView class]] objectForInfoDictionaryKey:(__bridge NSString *)kCFBundleVersionKey];
    return version ? String(version) : "0"_str;
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

// On this platform each page's process keeps the cookies of its site, and
// is told of a change a moment after it was made. A client that changes a
// cookie and then looks at document.cookie must not see the old one, so the
// processes are made to forget what they keep, and ask again, before the
// change is reported done.
static void forgetCachedCookies(WebKit::WebsiteDataStore& store, Function<void()>&& completion)
{
    auto aggregator = CallbackAggregator::create(WTF::move(completion));
    for (auto& pool : store.processPools()) {
        pool->forEachProcessForSession(store.sessionID(), [&](auto& process) {
            if (process.canSendMessage())
                process.sendWithAsyncReply(Messages::WebProcess::DeleteAllCookies(), [aggregator] { });
        });
    }
}

void addCookie(const PlatformNetworkSession& session, const PlatformCookie& cookie, Function<void()>&& completion)
{
    session->cookieStore().setCookies({ *cookie }, [session, completion = WTF::move(completion)]() mutable {
        forgetCachedCookies(*session, WTF::move(completion));
    });
}

void deleteCookie(const PlatformNetworkSession& session, const PlatformCookie& cookie, Function<void()>&& completion)
{
    session->cookieStore().deleteCookie(*cookie, [session, completion = WTF::move(completion)]() mutable {
        forgetCachedCookies(*session, WTF::move(completion));
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
