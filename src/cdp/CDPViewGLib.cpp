/*
 * The platform layer for WebKit's GLib API and WPE (see CDPPlatform.h):
 * everything about a page that goes through the view and not through the
 * inspector. Navigation, input, window size, dialogs, file choosers, the
 * life of the view, cookies and the creation of views.
 *
 * Input is delivered as platform events to the view, the same path a real
 * mouse and keyboard take, so pages see trusted events and default actions
 * (focus, text entry, link activation) happen.
 *
 * What is the same on every platform, such as the bookkeeping of a
 * navigation and the order of input answers, is in CDPView.cpp.
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
        m_native.signalHandlers.append(g_signal_connect_data(m_view, signal, callback, this, nullptr, after ? G_CONNECT_AFTER : static_cast<GConnectFlags>(0)));
    };
    connect("load-changed", G_CALLBACK(+[](WebKitWebView*, WebKitLoadEvent event, gpointer data) {
        auto& page = *static_cast<PageTarget*>(data);
        if (event == WEBKIT_LOAD_STARTED)
            page.loadChanged(LoadEvent::Started);
        else if (event == WEBKIT_LOAD_COMMITTED)
            page.loadChanged(LoadEvent::Committed);
        else if (event == WEBKIT_LOAD_FINISHED)
            page.loadChanged(LoadEvent::Finished);
    }));
    connect("load-failed", G_CALLBACK(+[](WebKitWebView*, WebKitLoadEvent, const char*, GError* error, gpointer data) -> gboolean {
        static_cast<PageTarget*>(data)->loadFailed(error);
        return FALSE;
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

void PageTarget::loadFailed(GError* error)
{
    if (g_error_matches(error, WEBKIT_NETWORK_ERROR, WEBKIT_NETWORK_ERROR_CANCELLED)) {
        loadCancelled();
        return;
    }
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
    loadFailed(text);
}

bool PageTarget::scriptDialog(WebKitScriptDialog* dialog)
{
    if (!acceptsDialog())
        return false;

    // Keeping a reference holds the dialog, and the script that opened it,
    // until a client answers with Page.handleJavaScriptDialog.
    m_native.dialog = webkit_script_dialog_ref(dialog);
    ASCIILiteral type = "alert"_s;
    String defaultPrompt;
    switch (webkit_script_dialog_get_dialog_type(dialog)) {
    case WEBKIT_SCRIPT_DIALOG_ALERT:
        break;
    case WEBKIT_SCRIPT_DIALOG_CONFIRM:
        type = "confirm"_s;
        break;
    case WEBKIT_SCRIPT_DIALOG_PROMPT:
        type = "prompt"_s;
        defaultPrompt = str(webkit_script_dialog_prompt_get_default_text(dialog));
        break;
    case WEBKIT_SCRIPT_DIALOG_BEFORE_UNLOAD_CONFIRM:
        type = "beforeunload"_s;
        break;
    }
    announceDialog(type, str(webkit_script_dialog_get_message(dialog)), defaultPrompt);
    return true;
}

void PageTarget::answerDialog(bool accept, const String& promptText)
{
    auto* dialog = std::exchange(m_native.dialog, nullptr);
    switch (webkit_script_dialog_get_dialog_type(dialog)) {
    case WEBKIT_SCRIPT_DIALOG_ALERT:
        break;
    case WEBKIT_SCRIPT_DIALOG_CONFIRM:
    case WEBKIT_SCRIPT_DIALOG_BEFORE_UNLOAD_CONFIRM:
        webkit_script_dialog_confirm_set_confirmed(dialog, accept);
        break;
    case WEBKIT_SCRIPT_DIALOG_PROMPT:
        if (accept)
            webkit_script_dialog_prompt_set_text(dialog, promptText.utf8().data());
        break;
    }
    webkit_script_dialog_close(dialog);
    webkit_script_dialog_unref(dialog);
}

void PageTarget::closeDialog()
{
    if (auto* dialog = std::exchange(m_native.dialog, nullptr)) {
        webkit_script_dialog_close(dialog);
        webkit_script_dialog_unref(dialog);
    }
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

// MARK: Navigation

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

// MARK: What the portable code asks of a view

WebKit::WebPageProxy& pageProxy(PlatformView view)
{
    return webkitWebViewGetPage(view);
}

String viewTitle(PlatformView view)
{
    return str(webkit_web_view_get_title(view));
}

String viewURI(PlatformView view)
{
    return str(webkit_web_view_get_uri(view));
}

void loadURI(PlatformView view, const String& url)
{
    webkit_web_view_load_uri(view, url.utf8().data());
}

void stopLoading(PlatformView view)
{
    webkit_web_view_stop_loading(view);
}

void terminateWebProcess(PlatformView view)
{
    webkit_web_view_terminate_web_process(view);
}

void reloadView(PlatformView view, bool bypassCache)
{
    // Through the API, which also works when the page's process has died.
    if (bypassCache)
        webkit_web_view_reload_bypass_cache(view);
    else
        webkit_web_view_reload(view);
}

std::pair<int, int> viewSize(PlatformView view)
{
    auto* wpeView = webkit_web_view_get_wpe_view(view);
    return wpeView ? std::pair { wpe_view_get_width(wpeView), wpe_view_get_height(wpeView) } : std::pair { 0, 0 };
}

String engineVersion()
{
    return makeString(webkit_get_major_version(), '.', webkit_get_minor_version(), '.', webkit_get_micro_version());
}

PlatformNetworkSession networkSessionOf(PlatformView view)
{
    return webkit_web_view_get_network_session(view);
}

PlatformNetworkSession defaultNetworkSession()
{
    return webkit_network_session_get_default();
}

PlatformNetworkSession createEphemeralNetworkSession()
{
    return adoptGRef(webkit_network_session_new_ephemeral());
}

// MARK: What goes with the view

void PageTarget::releaseView()
{
    for (auto handler : m_native.signalHandlers)
        g_signal_handler_disconnect(m_view, handler);
    m_native.signalHandlers.clear();
    if (m_native.dialog)
        webkit_script_dialog_unref(m_native.dialog);
    m_native.dialog = nullptr;
}

bool PageTarget::hasOpenDialog() const
{
    return !!m_native.dialog;
}

// The network session is shared with other views, so what a client turns
// off is remembered and put back when the client goes.
void PageTarget::setCertificateErrorsIgnored(bool ignore)
{
    auto* session = webkit_web_view_get_network_session(m_view);
    if (!m_native.originalTLSPolicy)
        m_native.originalTLSPolicy = webkit_network_session_get_tls_errors_policy(session);
    webkit_network_session_set_tls_errors_policy(session, ignore ? WEBKIT_TLS_ERRORS_POLICY_IGNORE : WEBKIT_TLS_ERRORS_POLICY_FAIL);
}

void PageTarget::restoreCertificateChecks()
{
    if (auto policy = std::exchange(m_native.originalTLSPolicy, std::nullopt))
        webkit_network_session_set_tls_errors_policy(webkit_web_view_get_network_session(m_view), static_cast<WebKitTLSErrorsPolicy>(*policy));
}

// A page restored from the back/forward cache comes back with the script
// contexts it had, and WebKit's inspector does not announce them again.
// While a client is attached, history navigations load afresh. Views can
// share one settings object, so it counts the pages that need this and gets
// its own value back when the last of them is done.
void PageTarget::setPageCacheSuspended(bool suspended)
{
    static GQuark quark = g_quark_from_static_string("zebkit-cdp-page-cache");
    if (suspended == !!m_native.pageCacheSettings || (suspended && !m_view))
        return;
    if (suspended)
        m_native.pageCacheSettings = G_OBJECT(webkit_web_view_get_settings(m_view));
    auto settings = suspended ? m_native.pageCacheSettings : std::exchange(m_native.pageCacheSettings, nullptr);

    gsize state = GPOINTER_TO_SIZE(g_object_get_qdata(settings.get(), quark));
    gsize count = state >> 1;
    bool wasEnabled = state & 1;
    if (suspended) {
        if (!count++) {
            wasEnabled = webkit_settings_get_enable_page_cache(WEBKIT_SETTINGS(settings.get()));
            webkit_settings_set_enable_page_cache(WEBKIT_SETTINGS(settings.get()), FALSE);
        }
    } else if (count && !--count && wasEnabled)
        webkit_settings_set_enable_page_cache(WEBKIT_SETTINGS(settings.get()), TRUE);
    g_object_set_qdata(settings.get(), quark, GSIZE_TO_POINTER(count ? (count << 1 | wasEnabled) : 0));
}

// MARK: Cookies

void allCookies(const PlatformNetworkSession& session, Function<void(CookieList&&)>&& completion)
{
    auto* manager = webkit_network_session_get_cookie_manager(session.get());
    auto* holder = new Function<void(CookieList&&)>(WTF::move(completion));
    webkit_cookie_manager_get_all_cookies(manager, nullptr, [](GObject* source, GAsyncResult* result, gpointer data) {
        std::unique_ptr<Function<void(CookieList&&)>> completion(static_cast<Function<void(CookieList&&)>*>(data));
        CookieList cookies;
        GList* list = webkit_cookie_manager_get_all_cookies_finish(WEBKIT_COOKIE_MANAGER(source), result, nullptr);
        for (GList* item = list; item; item = item->next)
            cookies.append(GUniquePtr<SoupCookie>(static_cast<SoupCookie*>(item->data)));
        g_list_free(list);
        (*completion)(WTF::move(cookies));
    }, holder);
}

Ref<JSON::Object> cookieToCDP(const PlatformCookie& holder)
{
    auto* cookie = holder.get();
    auto object = JSON::Object::create();
    auto name = str(soup_cookie_get_name(cookie));
    auto value = str(soup_cookie_get_value(cookie));
    object->setString("name"_s, name);
    object->setString("value"_s, value);
    object->setString("domain"_s, str(soup_cookie_get_domain(cookie)));
    object->setString("path"_s, str(soup_cookie_get_path(cookie)));
    GDateTime* expires = soup_cookie_get_expires(cookie);
    object->setDouble("expires"_s, expires ? static_cast<double>(g_date_time_to_unix(expires)) : -1);
    object->setInteger("size"_s, name.length() + value.length());
    object->setBoolean("httpOnly"_s, soup_cookie_get_http_only(cookie));
    object->setBoolean("secure"_s, soup_cookie_get_secure(cookie));
    object->setBoolean("session"_s, !expires);
    switch (soup_cookie_get_same_site_policy(cookie)) {
    case SOUP_SAME_SITE_POLICY_STRICT:
        object->setString("sameSite"_s, "Strict"_s);
        break;
    case SOUP_SAME_SITE_POLICY_LAX:
        object->setString("sameSite"_s, "Lax"_s);
        break;
    case SOUP_SAME_SITE_POLICY_NONE:
        if (soup_cookie_get_secure(cookie))
            object->setString("sameSite"_s, "None"_s);
        break;
    }
    object->setString("priority"_s, "Medium"_s);
    object->setString("sourceScheme"_s, soup_cookie_get_secure(cookie) ? "Secure"_s : "NonSecure"_s);
    object->setInteger("sourcePort"_s, soup_cookie_get_secure(cookie) ? 443 : 80);
    return object;
}

PlatformCookie cookieFromCDP(JSON::Object& object, const String& fallbackURL)
{
    auto name = object.getString("name"_s);
    auto value = object.getString("value"_s);
    auto domain = object.getString("domain"_s);
    auto path = object.getString("path"_s);
    auto url = object.getString("url"_s);
    if (url.isEmpty())
        url = fallbackURL;

    GRefPtr<GUri> uri = url.isEmpty() ? nullptr : adoptGRef(g_uri_parse(url.utf8().data(), G_URI_FLAGS_NONE, nullptr));
    if (domain.isEmpty() && uri)
        domain = str(g_uri_get_host(uri.get()));
    if (name.isNull() || domain.isEmpty())
        return nullptr;
    if (path.isEmpty())
        path = "/"_s;

    GUniquePtr<SoupCookie> cookie(soup_cookie_new(name.utf8().data(), value.utf8().data(), domain.utf8().data(), path.utf8().data(), -1));
    bool secure = object.getBoolean("secure"_s).value_or(uri && !g_strcmp0(g_uri_get_scheme(uri.get()), "https"));
    soup_cookie_set_secure(cookie.get(), secure);
    soup_cookie_set_http_only(cookie.get(), object.getBoolean("httpOnly"_s).value_or(false));
    auto sameSite = object.getString("sameSite"_s);
    if (sameSite == "Strict"_s)
        soup_cookie_set_same_site_policy(cookie.get(), SOUP_SAME_SITE_POLICY_STRICT);
    else if (sameSite == "Lax"_s)
        soup_cookie_set_same_site_policy(cookie.get(), SOUP_SAME_SITE_POLICY_LAX);
    else if (sameSite == "None"_s)
        soup_cookie_set_same_site_policy(cookie.get(), SOUP_SAME_SITE_POLICY_NONE);
    auto expires = object.getDouble("expires"_s);
    if (expires && *expires > 0) {
        GRefPtr<GDateTime> date = adoptGRef(g_date_time_new_from_unix_utc(static_cast<gint64>(*expires)));
        soup_cookie_set_expires(cookie.get(), date.get());
    }
    return cookie;
}

void addCookie(const PlatformNetworkSession& session, const PlatformCookie& cookie, Function<void()>&& completion)
{
    auto* holder = new Function<void()>(WTF::move(completion));
    webkit_cookie_manager_add_cookie(webkit_network_session_get_cookie_manager(session.get()), cookie.get(), nullptr, [](GObject* source, GAsyncResult* result, gpointer data) {
        std::unique_ptr<Function<void()>> completion(static_cast<Function<void()>*>(data));
        webkit_cookie_manager_add_cookie_finish(WEBKIT_COOKIE_MANAGER(source), result, nullptr);
        (*completion)();
    }, holder);
}

void deleteCookie(const PlatformNetworkSession& session, const PlatformCookie& cookie, Function<void()>&& completion)
{
    auto* holder = new Function<void()>(WTF::move(completion));
    webkit_cookie_manager_delete_cookie(webkit_network_session_get_cookie_manager(session.get()), cookie.get(), nullptr, [](GObject* source, GAsyncResult* result, gpointer data) {
        std::unique_ptr<Function<void()>> completion(static_cast<Function<void()>*>(data));
        webkit_cookie_manager_delete_cookie_finish(WEBKIT_COOKIE_MANAGER(source), result, nullptr);
        (*completion)();
    }, holder);
}

bool cookieAppliesToURL(const PlatformCookie& cookie, const String& url)
{
    GRefPtr<GUri> uri = adoptGRef(g_uri_parse(url.utf8().data(), G_URI_FLAGS_NONE, nullptr));
    return uri && g_uri_get_host(uri.get()) && soup_cookie_applies_to_uri(cookie.get(), uri.get());
}

bool cookieMatches(const PlatformCookie& cookie, const String& name, const String& domain, const String& path, const String& url)
{
    if (name != str(soup_cookie_get_name(cookie.get())))
        return false;
    if (!domain.isEmpty() && !soup_cookie_domain_matches(cookie.get(), domain.utf8().data()))
        return false;
    if (!path.isEmpty() && path != str(soup_cookie_get_path(cookie.get())))
        return false;
    GRefPtr<GUri> uri = url.isEmpty() ? nullptr : adoptGRef(g_uri_parse(url.utf8().data(), G_URI_FLAGS_NONE, nullptr));
    if (uri && g_uri_get_host(uri.get()) && !soup_cookie_domain_matches(cookie.get(), g_uri_get_host(uri.get())))
        return false;
    return true;
}

// MARK: Views of the server

static constexpr int defaultViewWidth = 1280;
static constexpr int defaultViewHeight = 720;

String Server::userAgent()
{
    if (!m_pages.isEmpty())
        return str(webkit_settings_get_user_agent(webkit_web_view_get_settings(m_pages.first()->view())));
    GRefPtr<WebKitSettings> settings = adoptGRef(webkit_settings_new());
    return str(webkit_settings_get_user_agent(settings.get()));
}

PlatformView Server::createView(const String& browserContextID, PlatformView related)
{
    SetForScope contextScope(m_creatingContextID, related && pageForView(related) ? pageForView(related)->browserContextID : browserContextID);
    SetForScope openerScope(m_creatingOpenerID, related && pageForView(related) ? pageForView(related)->targetID() : String());

    // An isolated context needs its own network session, which only a view
    // made here is sure to get.
    if (createViewFunc && (related || !networkSessionFor(browserContextID))) {
        if (auto* view = createViewFunc(related, createViewData))
            return view;
    }

    WebKitWebView* view;
    if (related)
        view = WEBKIT_WEB_VIEW(g_object_new(WEBKIT_TYPE_WEB_VIEW, "related-view", related, nullptr));
    else {
        // Take everything else from a view the application made, so that
        // pages opened over CDP behave like the application's own.
        WebKitWebView* model = nullptr;
        for (auto& page : m_pages) {
            if (page->browserContextID.isEmpty()) {
                model = page->view();
                break;
            }
        }
        Vector<const char*> names;
        Vector<GValue> values;
        auto add = [&](const char* name, gpointer object) {
            if (!object)
                return;
            GValue value = G_VALUE_INIT;
            g_value_init(&value, G_OBJECT_TYPE(object));
            g_value_set_object(&value, object);
            names.append(name);
            values.append(value);
        };
        auto networkSession = networkSessionFor(browserContextID);
        if (model) {
            add("web-context", webkit_web_view_get_context(model));
            add("settings", webkit_web_view_get_settings(model));
            add("user-content-manager", webkit_web_view_get_user_content_manager(model));
            add("display", webkit_web_view_get_display(model));
            if (!networkSession)
                networkSession = webkit_web_view_get_network_session(model);
        }
        add("network-session", networkSession.get());
        view = WEBKIT_WEB_VIEW(g_object_new_with_properties(WEBKIT_TYPE_WEB_VIEW, names.size(), names.mutableSpan().data(), values.mutableSpan().data()));
        for (auto& value : values)
            g_value_unset(&value);
    }
    if (!view)
        return nullptr;

    if (RefPtr page = pageForView(view))
        page->ownedByServer = true;
    if (auto* wpeView = webkit_web_view_get_wpe_view(view)) {
        if (auto* toplevel = wpe_view_get_toplevel(wpeView))
            wpe_toplevel_resize(toplevel, defaultViewWidth, defaultViewHeight);
        wpe_view_focus_in(wpeView);
    }
    return view;
}

void webViewCreated(WebKitWebView* view)
{
    auto& server = Server::singleton();
    server.startFromEnvironmentOnce();
    // Views are tracked even while nothing listens, so that an application
    // can start the endpoint after it has created its views.
    server.addView(view);
}

void webViewDestroyed(WebKitWebView* view)
{
    Server::singleton().removeView(view);
}

} // namespace Zebkit::CDP
