/*
 * The parts of a page target that go through the WebKit API instead of the
 * inspector: navigation, input, window size, dialogs, file choosers and the
 * life of the view. WebKit's protocol has none of these.
 *
 * Input is delivered as platform events to the view, the same path a real
 * mouse and keyboard take, so pages see trusted events and default actions
 * (focus, text entry, link activation) happen.
 */

#include "config.h"
#include "CDPInternal.h"

#include "WebKitWebViewPrivate.h"
#include "WebPageProxy.h"
#include "WebsiteDataStore.h"
#include <wpe/webkit.h>
#include <wpe/wpe-platform.h>
#include <wtf/URL.h>
#include <wtf/text/CString.h>
#include <wtf/text/MakeString.h>
#include <wtf/unicode/CharacterNames.h>

namespace Zebkit::CDP {

// MARK: View signals

void PageTarget::connectViewSignals()
{
    auto connect = [&](const char* signal, GCallback callback, bool after = false) {
        m_signalHandlers.append(g_signal_connect_data(m_view, signal, callback, this, nullptr, after ? G_CONNECT_AFTER : static_cast<GConnectFlags>(0)));
    };
    connect("load-changed", G_CALLBACK(+[](WebKitWebView*, WebKitLoadEvent event, gpointer data) {
        static_cast<PageTarget*>(data)->loadChanged(event);
    }));
    connect("load-failed", G_CALLBACK(+[](WebKitWebView*, WebKitLoadEvent, const char* failingURI, GError* error, gpointer data) -> gboolean {
        return static_cast<PageTarget*>(data)->loadFailed(failingURI, error);
    }));
    connect("load-failed-with-tls-errors", G_CALLBACK(+[](WebKitWebView*, const char*, GTlsCertificate*, GTlsCertificateFlags, gpointer data) -> gboolean {
        static_cast<PageTarget*>(data)->finishNavigation({ }, "net::ERR_CERT_AUTHORITY_INVALID"_s);
        return FALSE;
    }));
    connect("notify::uri", G_CALLBACK(+[](GObject*, GParamSpec*, gpointer data) {
        static_cast<PageTarget*>(data)->infoChanged();
        static_cast<PageTarget*>(data)->addressChanged();
    }));
    connect("notify::title", G_CALLBACK(+[](GObject*, GParamSpec*, gpointer data) {
        static_cast<PageTarget*>(data)->infoChanged();
    }));
    connect("script-dialog", G_CALLBACK(+[](WebKitWebView*, WebKitScriptDialog* dialog, gpointer data) -> gboolean {
        return static_cast<PageTarget*>(data)->scriptDialog(dialog);
    }));
    connect("run-file-chooser", G_CALLBACK(+[](WebKitWebView*, WebKitFileChooserRequest* request, gpointer data) -> gboolean {
        return static_cast<PageTarget*>(data)->runFileChooser(G_OBJECT(request));
    }));
    connect("web-process-terminated", G_CALLBACK(+[](WebKitWebView*, WebKitWebProcessTerminationReason, gpointer data) {
        static_cast<PageTarget*>(data)->processTerminated();
    }));
    // After the application's own handlers: a page opened by script becomes
    // a view of this endpoint only when the application did not make one.
    connect("create", G_CALLBACK(+[](WebKitWebView* view, WebKitNavigationAction*, gpointer) -> WebKitWebView* {
        // Only while a client is connected: with nobody to see or close
        // them, such views would pile up for as long as the process lives.
        auto& server = Server::singleton();
        return server.isListening() && server.hasConnections() ? server.createView({ }, view) : nullptr;
    }), true);
    connect("close", G_CALLBACK(+[](WebKitWebView* view, gpointer data) {
        auto& page = *static_cast<PageTarget*>(data);
        if (!std::exchange(page.ownedByServer, false))
            return;
        // Not from inside the signal the view is emitting.
        runAfter(0, [view] {
            g_object_unref(view);
        });
    }));
}

void PageTarget::confirmNavigationCommitted(unsigned generation, unsigned attemptsLeft)
{
    auto params = JSON::Object::create();
    params->setString("objectGroup"_s, "zebkit-sync"_s);
    wip("Runtime.releaseObjectGroup"_s, WTF::move(params), [this, weakThis = WeakPtr { *this }, generation, attemptsLeft](JSON::Object*, const String& error) {
        if (!weakThis || generation != m_navigationGeneration || !m_pendingNavigation)
            return;
        if (!error.isNull()) {
            // Asked of a process the page was just leaving; ask the new one.
            if (attemptsLeft && !m_crashed && m_inspectorConnected)
                confirmNavigationCommitted(generation, attemptsLeft - 1);
            return;
        }
        if (!m_pendingErrorText.isNull()) {
            m_errorPageExpected = false;
            finishNavigation({ }, std::exchange(m_pendingErrorText, String()));
            return;
        }
        auto frame = m_frames.find(mainFrameID());
        finishNavigation(frame == m_frames.end() ? String() : uniqueID(frame->value.loaderID), { });
    });
}

void PageTarget::infoChanged()
{
    if (m_view)
        Server::singleton().targetInfoChanged(*this);
}

// The address of the main frame changed. If no new document came with it,
// the document changed its own address: a fragment, or the history API.
// WebKit's protocol has no event for that, so the document is asked. A new
// document has reported its address by the time it answers; one that is
// still on its way leaves the old one answering with the old address.
void PageTarget::addressChanged()
{
    if (!m_view || !m_inspectorConnected || m_crashed)
        return;
    evaluateValue("location.href"_s, [this, weakThis = WeakPtr { *this }](RefPtr<JSON::Value>&& value) {
        if (weakThis && value)
            navigatedWithinDocument(value->asString());
    });
}

void PageTarget::navigatedWithinDocument(const String& url)
{
    auto frame = m_frames.find(mainFrameID());
    if (frame == m_frames.end() || url.isEmpty() || frame->value.url == url)
        return;
    auto withoutFragment = [](const String& address) {
        auto hash = address.find('#');
        return hash == notFound ? address : address.left(hash);
    };
    bool isFragment = withoutFragment(frame->value.url) == withoutFragment(url);
    frame->value.url = url;
    forEachSession([&](Session& session) {
        if (!session.pageEnabled)
            return;
        auto event = JSON::Object::create();
        event->setString("frameId"_s, m_targetID);
        event->setString("url"_s, url);
        event->setString("navigationType"_s, isFragment ? "fragment"_s : "historyApi"_s);
        session.event("Page.navigatedWithinDocument"_s, WTF::move(event));
    });
}

void PageTarget::loadChanged(int loadEvent)
{

    if (loadEvent == WEBKIT_LOAD_STARTED && std::exchange(m_crashed, false)) {
        // The load starts a new process, which knows nothing of this client.
        m_reconnectAttempts = 0;
        reconnectInspector();
    }
    if (loadEvent == WEBKIT_LOAD_COMMITTED && m_pendingNavigation && m_inspectorConnected) {
        // The page reports the commit itself, with the loader's id, unless
        // it committed before its process had been told to report anything.
        // Once the page has answered and the navigation is still waiting,
        // that is what happened.
        confirmNavigationCommitted(m_navigationGeneration, 5);
    }
    if (loadEvent != WEBKIT_LOAD_STARTED && loadEvent != WEBKIT_LOAD_FINISHED)
        return;
    bool started = loadEvent == WEBKIT_LOAD_STARTED;
    forEachSession([&](Session& session) {
        if (!session.pageEnabled)
            return;
        auto event = JSON::Object::create();
        event->setString("frameId"_s, m_targetID);
        session.event(started ? "Page.frameStartedLoading"_s : "Page.frameStoppedLoading"_s, WTF::move(event));
    });
}

bool PageTarget::loadFailed(const char*, GError* error)
{
    // A load that was replaced by a newer one is not the newer one failing.
    if (g_error_matches(error, WEBKIT_NETWORK_ERROR, WEBKIT_NETWORK_ERROR_CANCELLED)) {
        m_errorPageExpected = false;
        return false;
    }
    m_errorPageExpected = true;

    if (logLevel())
        g_printerr("cdp: load failed: %s %d: %s\n", g_quark_to_string(error->domain), error->code, error->message);
    ASCIILiteral text = "net::ERR_FAILED"_s;
    if (error->domain == G_RESOLVER_ERROR)
        text = "net::ERR_NAME_NOT_RESOLVED"_s;
    else if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CONNECTION_REFUSED))
        text = "net::ERR_CONNECTION_REFUSED"_s;
    else if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT))
        text = "net::ERR_TIMED_OUT"_s;
    else if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_HOST_UNREACHABLE) || g_error_matches(error, G_IO_ERROR, G_IO_ERROR_NETWORK_UNREACHABLE))
        text = "net::ERR_ADDRESS_UNREACHABLE"_s;
    else if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CONNECTION_CLOSED) || g_error_matches(error, G_IO_ERROR, G_IO_ERROR_BROKEN_PIPE))
        text = "net::ERR_CONNECTION_CLOSED"_s;
    else if (error->domain == G_TLS_ERROR)
        text = "net::ERR_SSL_PROTOCOL_ERROR"_s;
    else if (error->domain == WEBKIT_POLICY_ERROR)
        text = "net::ERR_ABORTED"_s;
    else if (g_error_matches(error, WEBKIT_NETWORK_ERROR, WEBKIT_NETWORK_ERROR_FILE_DOES_NOT_EXIST))
        text = "net::ERR_FILE_NOT_FOUND"_s;
    else if (g_error_matches(error, WEBKIT_NETWORK_ERROR, WEBKIT_NETWORK_ERROR_UNKNOWN_PROTOCOL))
        text = "net::ERR_UNKNOWN_URL_SCHEME"_s;
    if (!m_pendingNavigation)
        return false;
    // WebKit now loads its error page. The client is told of the failure
    // once that page has committed (handleInnerEvent), so that the commit
    // is not taken for part of whatever the client does next. An
    // application may suppress the error page; do not wait for long.
    m_pendingErrorText = text;
    unsigned generation = ++m_navigationGeneration;
    runAfter(500, [this, weakThis = WeakPtr { *this }, generation] {
        if (weakThis && generation == m_navigationGeneration && !m_pendingErrorText.isNull())
            finishNavigation({ }, std::exchange(m_pendingErrorText, String()));
    });
    return false;
}

void PageTarget::processTerminated()
{
    // WebKit keeps the same inspector target for the page and gives it a new
    // process on the next load, so nothing here says the old one is gone.
    // Whatever was waiting for it will never be answered.
    m_crashed = true;
    m_queuedCommands.clear();
    failPendingCommands("Target crashed"_s);
    resetDocumentState();
    dismissDialog();
    forEachSession([](Session& session) {
        session.event("Inspector.targetCrashed"_s, JSON::Object::create());
    });
    finishNavigation({ }, "net::ERR_ABORTED"_s);
}

void PageTarget::pageCrash(Session&, JSON::Object&, Reply&& reply)
{
    replyOK(reply);
    webkit_web_view_terminate_web_process(m_view);
}

void PageTarget::dismissDialog()
{
    for (auto& session : m_sessions)
        session->sawDialog = false;
    if (auto* dialog = std::exchange(m_dialog, nullptr)) {
        webkit_script_dialog_close(dialog);
        webkit_script_dialog_unref(dialog);
    }
}

bool PageTarget::scriptDialog(WebKitScriptDialog* dialog)
{
    bool hasListener = false;
    for (auto& session : m_sessions)
        hasListener |= session->pageEnabled;
    if (!hasListener || m_dialog)
        return false;

    // Keeping a reference holds the dialog, and the script that opened it,
    // until a client answers with Page.handleJavaScriptDialog.
    m_dialog = webkit_script_dialog_ref(dialog);
    ASCIILiteral type = "alert"_s;
    switch (webkit_script_dialog_get_dialog_type(dialog)) {
    case WEBKIT_SCRIPT_DIALOG_ALERT:
        break;
    case WEBKIT_SCRIPT_DIALOG_CONFIRM:
        type = "confirm"_s;
        break;
    case WEBKIT_SCRIPT_DIALOG_PROMPT:
        type = "prompt"_s;
        break;
    case WEBKIT_SCRIPT_DIALOG_BEFORE_UNLOAD_CONFIRM:
        type = "beforeunload"_s;
        break;
    }
    forEachSession([&](Session& session) {
        if (!session.pageEnabled)
            return;
        session.sawDialog = true;
        auto event = JSON::Object::create();
        event->setString("url"_s, str(webkit_web_view_get_uri(m_view)));
        // WebKit does not say which frame opened it.
        event->setString("frameId"_s, m_targetID);
        event->setString("message"_s, str(webkit_script_dialog_get_message(dialog)));
        event->setString("type"_s, type);
        event->setBoolean("hasBrowserHandler"_s, false);
        if (webkit_script_dialog_get_dialog_type(dialog) == WEBKIT_SCRIPT_DIALOG_PROMPT)
            event->setString("defaultPrompt"_s, str(webkit_script_dialog_prompt_get_default_text(dialog)));
        session.event("Page.javascriptDialogOpening"_s, WTF::move(event));
    });
    return true;
}

void PageTarget::pageHandleJavaScriptDialog(Session&, JSON::Object& params, Reply&& reply)
{
    if (!m_dialog) {
        replyInvalid(reply, "No dialog is showing"_s);
        return;
    }
    auto* dialog = std::exchange(m_dialog, nullptr);
    bool accept = params.getBoolean("accept"_s).value_or(false);
    auto promptText = params.getString("promptText"_s);
    switch (webkit_script_dialog_get_dialog_type(dialog)) {
    case WEBKIT_SCRIPT_DIALOG_ALERT:
        break;
    case WEBKIT_SCRIPT_DIALOG_CONFIRM:
    case WEBKIT_SCRIPT_DIALOG_BEFORE_UNLOAD_CONFIRM:
        webkit_script_dialog_confirm_set_confirmed(dialog, accept);
        break;
    case WEBKIT_SCRIPT_DIALOG_PROMPT:
        if (accept) {
            if (promptText.isNull())
                promptText = str(webkit_script_dialog_prompt_get_default_text(dialog));
            webkit_script_dialog_prompt_set_text(dialog, promptText.utf8().data());
        }
        break;
    }
    webkit_script_dialog_close(dialog);
    webkit_script_dialog_unref(dialog);
    for (auto& session : m_sessions)
        session->sawDialog = false;

    forEachSession([&](Session& session) {
        if (!session.pageEnabled)
            return;
        auto event = JSON::Object::create();
        event->setString("frameId"_s, m_targetID);
        event->setBoolean("result"_s, accept);
        event->setString("userInput"_s, promptText.isNull() ? emptyString() : promptText);
        session.event("Page.javascriptDialogClosed"_s, WTF::move(event));
    });
    replyOK(reply);
}

bool PageTarget::runFileChooser(GObject* request)
{
    if (!std::exchange(m_expectingFileChooser, false))
        return false;
    Vector<CString> paths;
    for (auto& file : std::exchange(m_pendingFiles, { }))
        paths.append(file.utf8());
    Vector<const char*> pointers;
    for (auto& path : paths)
        pointers.append(path.data());
    pointers.append(nullptr);
    webkit_file_chooser_request_select_files(WEBKIT_FILE_CHOOSER_REQUEST(request), pointers.span().data());
    return true;
}

void PageTarget::close(bool force)
{
    if (!m_view)
        return;
    // A view this endpoint owns can simply be destroyed. One that belongs
    // to the application can only be asked, which runs beforeunload.
    if (!force || !std::exchange(ownedByServer, false)) {
        webkit_web_view_try_close(m_view);
        return;
    }
    runAfter(0, [view = m_view, weakThis = WeakPtr { *this }] {
        if (weakThis && weakThis->m_view == view)
            g_object_unref(view);
    });
}

void PageTarget::pageClose(Session&, JSON::Object&, Reply&& reply)
{
    replyOK(reply);
    close();
}

// MARK: Navigation

void PageTarget::finishNavigation(const String& loaderID, const String& errorText)
{
    if (!m_pendingNavigation)
        return;
    auto reply = std::exchange(m_pendingNavigation, nullptr);
    auto result = JSON::Object::create();
    result->setString("frameId"_s, m_targetID);
    if (!loaderID.isEmpty())
        result->setString("loaderId"_s, loaderID);
    if (!errorText.isNull())
        result->setString("errorText"_s, errorText);
    reply(WTF::move(result), { });
}

void PageTarget::pageNavigate(Session&, JSON::Object& params, Reply&& reply)
{
    auto url = params.getString("url"_s);
    if (auto frameID = params.getString("frameId"_s); !frameID.isEmpty() && frameID != m_targetID) {
        replyError(reply, "Only the main frame can be navigated"_s);
        return;
    }
    // What cannot be an address is refused, as in Chrome, and not loaded.
    if (url.isEmpty() || !URL { url }.isValid()) {
        replyError(reply, "Cannot navigate to invalid URL"_s);
        return;
    }
    finishNavigation({ }, "net::ERR_ABORTED"_s);
    pendingInitialURL = String();
    m_pendingErrorText = String();
    ++m_navigationGeneration;

    auto withoutFragment = [](const String& address) {
        auto hash = address.find('#');
        return hash == notFound ? address : address.left(hash);
    };
    auto current = str(webkit_web_view_get_uri(m_view));
    bool sameDocument = url.contains('#') && !current.isEmpty() && withoutFragment(url) == withoutFragment(current);
    if (sameDocument || url.startsWith("javascript:"_s)) {
        // Neither commits a new document, so there is nothing to wait for.
        webkit_web_view_load_uri(m_view, url.utf8().data());
        auto result = JSON::Object::create();
        result->setString("frameId"_s, m_targetID);
        reply(WTF::move(result), { });
        if (sameDocument)
            navigatedWithinDocument(url);
        return;
    }

    // Answered when the document commits (handleInnerEvent) or the load
    // fails (loadFailed), as Chrome does.
    m_pendingNavigation = WTF::move(reply);
    webkit_web_view_load_uri(m_view, url.utf8().data());
}

void PageTarget::pageStopLoading(Session&, JSON::Object&, Reply&& reply)
{
    webkit_web_view_stop_loading(m_view);
    // WebKit reports this like a load a newer one replaced, which is not
    // passed on; the navigation the client is waiting for is over all the same.
    ++m_navigationGeneration;
    m_pendingErrorText = String();
    finishNavigation({ }, "net::ERR_ABORTED"_s);
    replyOK(reply);
}

void PageTarget::pageGetNavigationHistory(Session&, JSON::Object&, Reply&& reply)
{
    auto* list = webkit_web_view_get_back_forward_list(m_view);
    auto entries = JSON::Array::create();
    int index = 0;
    auto add = [&](WebKitBackForwardListItem* item) {
        auto entry = JSON::Object::create();
        entry->setInteger("id"_s, index++);
        entry->setString("url"_s, str(webkit_back_forward_list_item_get_uri(item)));
        entry->setString("userTypedURL"_s, str(webkit_back_forward_list_item_get_original_uri(item)));
        entry->setString("title"_s, str(webkit_back_forward_list_item_get_title(item)));
        entry->setString("transitionType"_s, "typed"_s);
        entries->pushObject(WTF::move(entry));
    };
    // The back list comes nearest first.
    GList* back = g_list_reverse(webkit_back_forward_list_get_back_list(list));
    for (GList* item = back; item; item = item->next)
        add(WEBKIT_BACK_FORWARD_LIST_ITEM(item->data));
    g_list_free(back);
    int currentIndex = index;
    if (auto* current = webkit_back_forward_list_get_current_item(list))
        add(current);
    else
        currentIndex = -1;
    GList* forward = webkit_back_forward_list_get_forward_list(list);
    for (GList* item = forward; item; item = item->next)
        add(WEBKIT_BACK_FORWARD_LIST_ITEM(item->data));
    g_list_free(forward);

    auto result = JSON::Object::create();
    result->setInteger("currentIndex"_s, currentIndex);
    result->setArray("entries"_s, WTF::move(entries));
    reply(WTF::move(result), { });
}

void PageTarget::pageNavigateToHistoryEntry(Session&, JSON::Object& params, Reply&& reply)
{
    auto* list = webkit_web_view_get_back_forward_list(m_view);
    int backLength = g_list_length(webkit_back_forward_list_get_back_list(list));
    int entryID = params.getInteger("entryId"_s).value_or(-1);
    // Ids are positions in the list getNavigationHistory returned; the API
    // counts from the current item instead.
    auto* item = webkit_back_forward_list_get_nth_item(list, entryID - backLength);
    if (entryID < 0 || !item) {
        replyInvalid(reply, "No entry with passed id"_s);
        return;
    }
    webkit_web_view_go_to_back_forward_list_item(m_view, item);
    replyOK(reply);
}

// MARK: Window

void PageTarget::focusView()
{
    auto* wpeView = m_view ? webkit_web_view_get_wpe_view(m_view) : nullptr;
    if (wpeView && !wpe_view_get_has_focus(wpeView))
        wpe_view_focus_in(wpeView);
}

void PageTarget::pageBringToFront(Session&, JSON::Object&, Reply&& reply)
{
    focusView();
    replyOK(reply);
}

void PageTarget::emulationSetDeviceMetricsOverride(Session&, JSON::Object& params, Reply&& reply)
{
    int width = params.getInteger("width"_s).value_or(0);
    int height = params.getInteger("height"_s).value_or(0);
    auto* wpeView = webkit_web_view_get_wpe_view(m_view);
    auto* toplevel = wpeView ? wpe_view_get_toplevel(wpeView) : nullptr;
    if (width <= 0 || height <= 0 || !toplevel) {
        replyOK(reply);
        return;
    }
    // The override is a real resize of the view. The page learns of it a
    // little later; answer once it has, so that what the client does next
    // sees the new size.
    if (!m_originalWidth) {
        m_originalWidth = wpe_view_get_width(wpeView);
        m_originalHeight = wpe_view_get_height(wpeView);
    }
    wpe_toplevel_resize(toplevel, width, height);
    waitForViewportWidth(width, 40, WTF::move(reply));
}

void PageTarget::restoreViewSize()
{
    int width = std::exchange(m_originalWidth, 0);
    int height = std::exchange(m_originalHeight, 0);
    auto* wpeView = m_view ? webkit_web_view_get_wpe_view(m_view) : nullptr;
    auto* toplevel = wpeView ? wpe_view_get_toplevel(wpeView) : nullptr;
    if (width > 0 && height > 0 && toplevel)
        wpe_toplevel_resize(toplevel, width, height);
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

void PageTarget::waitForViewportWidth(int width, unsigned attemptsLeft, Reply&& reply)
{
    evaluateValue("innerWidth"_s, [this, weakThis = WeakPtr { *this }, width, attemptsLeft, reply = WTF::move(reply)](RefPtr<JSON::Value>&& value) mutable {
        if (!weakThis || !attemptsLeft || (value && value->asInteger().value_or(0) == width)) {
            replyOK(reply);
            return;
        }
        runAfter(25, [this, weakThis, width, attemptsLeft, reply = WTF::move(reply)]() mutable {
            if (weakThis)
                waitForViewportWidth(width, attemptsLeft - 1, WTF::move(reply));
            else
                replyOK(reply);
        });
    });
}

// MARK: Input

// CDP: Alt 1, Ctrl 2, Meta 4, Shift 8.
static WPEModifiers modifiersFromCDP(int modifiers)
{
    unsigned result = 0;
    if (modifiers & 1)
        result |= WPE_MODIFIER_KEYBOARD_ALT;
    if (modifiers & 2)
        result |= WPE_MODIFIER_KEYBOARD_CONTROL;
    if (modifiers & 4)
        result |= WPE_MODIFIER_KEYBOARD_META;
    if (modifiers & 8)
        result |= WPE_MODIFIER_KEYBOARD_SHIFT;
    return static_cast<WPEModifiers>(result);
}

static guint32 eventTime()
{
    return static_cast<guint32>(g_get_monotonic_time() / 1000);
}

// Events are handed to the page's process one at a time, the next when the
// previous one has been handled. A command is answered when nothing is left
// in that queue, so that script the client runs next sees its effect.
//
// The answers go out in the order the commands came in. Clients send a
// press and its release without waiting in between and keep track of which
// buttons are down from the order of the answers.
void PageTarget::whenInputProcessed(Reply&& reply)
{
    m_inputReplies.append(WTF::move(reply));
    if (!std::exchange(m_inputPollActive, true))
        pollInput(1500);
}

void PageTarget::pollInput(unsigned attemptsLeft)
{
    if (!m_view) {
        m_inputPollActive = false;
        for (auto& reply : std::exchange(m_inputReplies, { }))
            replyError(reply, "Target closed"_s);
        return;
    }
    auto& page = webkitWebViewGetPage(m_view);
    bool busy = page.isProcessingMouseEvents() || page.isProcessingKeyboardEvents() || page.isProcessingWheelEvents();
    if (!busy || !attemptsLeft) {
        // What the events set off in the page (a click handler, a focus
        // change) has run by the time the page answers anything else.
        m_inputPollActive = false;
        afterPageCatchesUp([replies = std::exchange(m_inputReplies, { })]() mutable {
            for (auto& reply : replies)
                replyOK(reply);
        });
        return;
    }
    runAfter(2, [this, protectedThis = Ref { *this }, attemptsLeft] {
        pollInput(attemptsLeft - 1);
    });
}

void PageTarget::inputDispatchMouseEvent(Session&, JSON::Object& params, Reply&& reply)
{
    auto* wpeView = webkit_web_view_get_wpe_view(m_view);
    if (!wpeView) {
        replyError(reply, "The view cannot receive input"_s);
        return;
    }
    focusView();

    auto type = params.getString("type"_s);
    double x = params.getDouble("x"_s).value_or(0);
    double y = params.getDouble("y"_s).value_or(0);
    auto buttonName = params.getString("button"_s);
    unsigned button = buttonName == "left"_s ? WPE_BUTTON_PRIMARY : buttonName == "middle"_s ? WPE_BUTTON_MIDDLE : buttonName == "right"_s ? WPE_BUTTON_SECONDARY : 0;
    unsigned buttonMask = button == WPE_BUTTON_PRIMARY ? WPE_MODIFIER_POINTER_BUTTON1 : button == WPE_BUTTON_MIDDLE ? WPE_MODIFIER_POINTER_BUTTON2 : button == WPE_BUTTON_SECONDARY ? WPE_MODIFIER_POINTER_BUTTON3 : 0;
    unsigned modifiers = modifiersFromCDP(params.getInteger("modifiers"_s).value_or(0));

    GRefPtr<WPEEvent> event;
    if (type == "mouseMoved"_s) {
        event = adoptGRef(wpe_event_pointer_move_new(WPE_EVENT_POINTER_MOVE, wpeView, WPE_INPUT_SOURCE_MOUSE, eventTime(),
            static_cast<WPEModifiers>(modifiers | m_pressedButtons), x, y, x - m_lastMouseX, y - m_lastMouseY));
    } else if (type == "mousePressed"_s && button) {
        m_pressedButtons |= buttonMask;
        event = adoptGRef(wpe_event_pointer_button_new(WPE_EVENT_POINTER_DOWN, wpeView, WPE_INPUT_SOURCE_MOUSE, eventTime(),
            static_cast<WPEModifiers>(modifiers | m_pressedButtons), button, x, y, std::max(1, params.getInteger("clickCount"_s).value_or(1))));
    } else if (type == "mouseReleased"_s && button) {
        m_pressedButtons &= ~buttonMask;
        event = adoptGRef(wpe_event_pointer_button_new(WPE_EVENT_POINTER_UP, wpeView, WPE_INPUT_SOURCE_MOUSE, eventTime(),
            static_cast<WPEModifiers>(modifiers | m_pressedButtons), button, x, y, 0));
    } else if (type == "mouseWheel"_s) {
        // CDP gives the distance to scroll in CSS pixels, down and right
        // positive. The platform counts the other way, and WebKit multiplies
        // what a mouse reports by this factor (WebEventFactoryWPE.cpp).
        static constexpr double mouseScrollMultiplier = 2.5;
        event = adoptGRef(wpe_event_scroll_new(wpeView, WPE_INPUT_SOURCE_MOUSE, eventTime(), static_cast<WPEModifiers>(modifiers),
            -params.getDouble("deltaX"_s).value_or(0) / mouseScrollMultiplier, -params.getDouble("deltaY"_s).value_or(0) / mouseScrollMultiplier, TRUE, FALSE, x, y));
    } else {
        replyInvalid(reply, makeString("Unexpected event type '"_s, type, '\''));
        return;
    }
    m_lastMouseX = x;
    m_lastMouseY = y;
    wpe_view_event(wpeView, event.get());
    whenInputProcessed(WTF::move(reply));
}

static unsigned keyvalForName(const String& key)
{
    static constexpr std::pair<ASCIILiteral, unsigned> names[] = {
        { "Enter"_s, 0xff0d }, { "Tab"_s, 0xff09 }, { "Backspace"_s, 0xff08 }, { "Escape"_s, 0xff1b }, { "Delete"_s, 0xffff },
        { "ArrowLeft"_s, 0xff51 }, { "ArrowUp"_s, 0xff52 }, { "ArrowRight"_s, 0xff53 }, { "ArrowDown"_s, 0xff54 },
        { "Home"_s, 0xff50 }, { "End"_s, 0xff57 }, { "PageUp"_s, 0xff55 }, { "PageDown"_s, 0xff56 }, { "Insert"_s, 0xff63 },
        { "Shift"_s, 0xffe1 }, { "Control"_s, 0xffe3 }, { "Alt"_s, 0xffe9 }, { "Meta"_s, 0xffe7 }, { "CapsLock"_s, 0xffe5 },
        { "ContextMenu"_s, 0xff67 }, { "Pause"_s, 0xff13 }, { "ScrollLock"_s, 0xff14 }, { "PrintScreen"_s, 0xff61 }, { "NumLock"_s, 0xff7f },
        { "F1"_s, 0xffbe }, { "F2"_s, 0xffbf }, { "F3"_s, 0xffc0 }, { "F4"_s, 0xffc1 }, { "F5"_s, 0xffc2 }, { "F6"_s, 0xffc3 },
        { "F7"_s, 0xffc4 }, { "F8"_s, 0xffc5 }, { "F9"_s, 0xffc6 }, { "F10"_s, 0xffc7 }, { "F11"_s, 0xffc8 }, { "F12"_s, 0xffc9 },
    };
    for (auto& entry : names) {
        if (key == entry.first)
            return entry.second;
    }
    return 0;
}

static char32_t singleCodePoint(const String& text)
{
    if (text.isEmpty())
        return 0;
    char32_t first = text[0];
    if (U16_IS_LEAD(first) && text.length() > 1 && U16_IS_TRAIL(text[1]))
        first = U16_GET_SUPPLEMENTARY(first, text[1]);
    unsigned length = first > 0xffff ? 2 : 1;
    return text.length() == length ? first : 0;
}

void PageTarget::inputInsertText(Session&, JSON::Object& params, Reply&& reply)
{
    focusView();
    webkit_web_view_execute_editing_command_with_argument(m_view, "InsertText", params.getString("text"_s).utf8().data());
    whenInputProcessed(WTF::move(reply));
}

void PageTarget::inputDispatchKeyEvent(Session& session, JSON::Object& params, Reply&& reply)
{
    auto* wpeView = webkit_web_view_get_wpe_view(m_view);
    if (!wpeView) {
        replyError(reply, "The view cannot receive input"_s);
        return;
    }
    focusView();

    auto type = params.getString("type"_s);
    auto key = params.getString("key"_s);
    auto text = params.getString("text"_s);
    if (type != "keyDown"_s && type != "keyUp"_s && type != "rawKeyDown"_s && type != "char"_s) {
        replyInvalid(reply, makeString("Unexpected event type '"_s, type, '\''));
        return;
    }
    if (type == "char"_s) {
        // Text with no key behind it, as from an input method.
        inputInsertText(session, params, WTF::move(reply));
        return;
    }

    // The platform event carries a key symbol, from which WebKit works out
    // the key name and the text to insert.
    unsigned keyval = keyvalForName(key);
    if (!keyval) {
        char32_t character = singleCodePoint(text);
        if (character < 0x20 || character == 0x7f)
            character = singleCodePoint(key);
        if (character >= 0x20 && character != 0x7f)
            keyval = wpe_unicode_to_keyval(character);
    }
    if (!keyval) {
        replyError(reply, makeString("Unknown key: \""_s, key, '"'));
        return;
    }

    // And the position of a physical key, which becomes the "code".
    unsigned keycode = 0;
    if (auto* keymap = wpe_display_get_keymap(wpe_view_get_display(wpeView))) {
        WPEKeymapEntry* entries = nullptr;
        guint entryCount = 0;
        if (wpe_keymap_get_entries_for_keyval(keymap, keyval, &entries, &entryCount) && entryCount)
            keycode = entries[0].keycode;
        g_free(entries);
    }

    bool isDown = type == "keyDown"_s || type == "rawKeyDown"_s;
    GRefPtr<WPEEvent> event = adoptGRef(wpe_event_keyboard_new(isDown ? WPE_EVENT_KEYBOARD_KEY_DOWN : WPE_EVENT_KEYBOARD_KEY_UP, wpeView,
        WPE_INPUT_SOURCE_KEYBOARD, eventTime(), modifiersFromCDP(params.getInteger("modifiers"_s).value_or(0)), keycode, keyval));
    wpe_view_event(wpeView, event.get());
    whenInputProcessed(WTF::move(reply));
}

} // namespace Zebkit::CDP
