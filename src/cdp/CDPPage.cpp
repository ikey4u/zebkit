/*
 * A page target: the connection to the page's Web Inspector Protocol
 * backend, and the translation of what the page reports into CDP events.
 *
 * WebKit's protocol is nested. The UI process answers a small Target domain
 * and relays everything else, as strings, to the web process that currently
 * holds the page ("inner target"). A navigation that changes process creates
 * a second, provisional inner target and later commits it, so each inner
 * target is set up as soon as it appears and held paused until then.
 */

#include "config.h"
#include "CDPInternal.h"

#include "WebKitWebViewPrivate.h"
#include "WebPageInspectorController.h"
#include "WebPageProxy.h"
#include "WebsiteDataStore.h"
#include <wpe/webkit.h>
#include <wtf/text/CString.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringToIntegerConversion.h>

namespace Zebkit::CDP {

static String originOf(const String& url)
{
    GRefPtr<GUri> uri = adoptGRef(g_uri_parse(url.utf8().data(), G_URI_FLAGS_NONE, nullptr));
    if (!uri || !g_uri_get_host(uri.get()))
        return url.startsWith("about:"_s) || url.isEmpty() ? "://"_s : url;
    int port = g_uri_get_port(uri.get());
    if (port > 0)
        return makeString(str(g_uri_get_scheme(uri.get())), "://"_s, str(g_uri_get_host(uri.get())), ':', port);
    return makeString(str(g_uri_get_scheme(uri.get())), "://"_s, str(g_uri_get_host(uri.get())));
}

PageTarget::PageTarget(WebKitWebView* view)
    : m_view(view)
    , m_targetID(randomID())
{
    connectViewSignals();
}

PageTarget::~PageTarget() = default;

Ref<JSON::Object> PageTarget::targetInfo() const
{
    auto info = JSON::Object::create();
    info->setString("targetId"_s, m_targetID);
    info->setString("type"_s, "page"_s);
    info->setString("title"_s, m_view ? str(webkit_web_view_get_title(m_view)) : emptyString());
    info->setString("url"_s, m_view ? str(webkit_web_view_get_uri(m_view)) : emptyString());
    info->setBoolean("attached"_s, !m_sessions.isEmpty());
    info->setBoolean("canAccessOpener"_s, false);
    if (!openerID.isEmpty())
        info->setString("openerId"_s, openerID);
    info->setString("browserContextId"_s, browserContextID.isEmpty() ? "default"_s : browserContextID);
    return info;
}

void PageTarget::attach(Session& session)
{
    session.page = *this;
    m_sessions.append(session);
    connectInspector();
}

void PageTarget::detach(Session& session)
{
    session.page = nullptr;
    m_sessions.removeFirstMatching([&](auto& item) {
        return item.ptr() == &session;
    });
    // A dialog holds the page's script until it is answered. When the last
    // client that knows of it leaves, nobody will.
    if (m_dialog && std::exchange(session.sawDialog, false)) {
        bool stillKnown = false;
        for (auto& other : m_sessions)
            stillKnown |= other->sawDialog;
        if (!stillKnown)
            dismissDialog();
    }
    if (m_sessions.isEmpty())
        disconnectInspector();
}

void PageTarget::viewDestroyed()
{
    Ref protectedThis { *this };
    for (auto& session : std::exchange(m_sessions, { }))
        session->page = nullptr;
    disconnectInspector();
    if (m_pendingNavigation)
        replyError(m_pendingNavigation, "Target closed"_s);
    m_pendingNavigation = nullptr;
    for (auto handler : m_signalHandlers)
        g_signal_handler_disconnect(m_view, handler);
    m_signalHandlers.clear();
    if (m_dialog)
        webkit_script_dialog_unref(m_dialog);
    m_dialog = nullptr;
    m_view = nullptr;
}

void PageTarget::forEachSession(NOESCAPE const Function<void(Session&)>& function)
{
    for (auto& session : copyToVector(m_sessions))
        function(session);
}

// MARK: Web Inspector Protocol plumbing

void PageTarget::connectInspector()
{
    if (m_inspectorConnected || !m_view)
        return;
    m_inspectorConnected = true;
    m_state.clear();
    m_state.append({ "Page.enable"_s, nullptr });
    m_state.append({ "Runtime.enable"_s, nullptr });
    m_state.append({ "Network.enable"_s, nullptr });
    m_state.append({ "Console.enable"_s, nullptr });
    updateBootstrapScript();

    setPageCacheSuspended(true);

    webkitWebViewGetPage(m_view).inspectorController().connectFrontend(*this);
    auto params = JSON::Object::create();
    params->setBoolean("pauseOnStart"_s, true);
    sendOuter("Target.setPauseOnStart"_s, WTF::move(params));
}

// A page restored from the back/forward cache comes back with the script
// contexts it had, and WebKit's inspector does not announce them again.
// While a client is attached, history navigations load afresh. Views can
// share one settings object, so it counts the pages that need this and gets
// its own value back when the last of them is done.
void PageTarget::setPageCacheSuspended(bool suspended)
{
    static GQuark quark = g_quark_from_static_string("zebkit-cdp-page-cache");
    if (suspended == !!m_pageCacheSettings || (suspended && !m_view))
        return;
    if (suspended)
        m_pageCacheSettings = G_OBJECT(webkit_web_view_get_settings(m_view));
    auto settings = suspended ? m_pageCacheSettings : std::exchange(m_pageCacheSettings, nullptr);

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

// Connects again from scratch. What had been sent to the page's process is
// gone when that process is new: after a crash, or for a view whose process
// was still starting.
void PageTarget::reconnectInspector()
{
    if (!m_inspectorConnected || !m_view)
        return;
    ++m_connectionGeneration;
    auto& controller = webkitWebViewGetPage(m_view).inspectorController();
    controller.disconnectFrontend(*this);
    m_innerTargetID = String();
    m_innerTargets.clear();
    controller.connectFrontend(*this);
    auto params = JSON::Object::create();
    params->setBoolean("pauseOnStart"_s, true);
    sendOuter("Target.setPauseOnStart"_s, WTF::move(params));
}

void PageTarget::disconnectInspector()
{
    if (!m_inspectorConnected)
        return;
    m_inspectorConnected = false;
    // What the client changed outside the page does not outlive it: script
    // must not stay blocked on a dialog nobody will answer, and the
    // application gets its window size and certificate checks back.
    dismissDialog();
    restoreViewSize();
    if (m_view) {
        if (auto policy = std::exchange(m_originalTLSPolicy, std::nullopt))
            webkit_network_session_set_tls_errors_policy(webkit_web_view_get_network_session(m_view), static_cast<WebKitTLSErrorsPolicy>(*policy));
        // Nothing may stay paused waiting for a client that has gone.
        auto params = JSON::Object::create();
        params->setBoolean("pauseOnStart"_s, false);
        sendOuter("Target.setPauseOnStart"_s, WTF::move(params));
        webkitWebViewGetPage(m_view).inspectorController().disconnectFrontend(*this);
    }
    setPageCacheSuspended(false);
    m_crashed = false;
    m_reconnectAttempts = 0;
    m_documentURLs.clear();
    m_capturedChildNodes = nullptr;
    m_childNodeCaptures = 0;
    m_innerTargetID = String();
    m_innerTargets.clear();
    m_queuedCommands.clear();
    failPendingCommands("Target closed"_s);
    m_contexts.clear();
    m_frames.clear();
    m_globalObjects.clear();
    m_backendNodes.clear();
    m_frameOwnerNodes.clear();
    m_requestIDs.clear();
    m_inflightRequests.clear();
    m_documentNodeID = 0;
    m_scripts.clear();
    m_bindings.clear();
    m_worlds.clear();
}

void PageTarget::sendOuter(const String& method, Ref<JSON::Object>&& params)
{
    if (!m_view)
        return;
    auto message = JSON::Object::create();
    message->setInteger("id"_s, m_nextCommandID++);
    message->setString("method"_s, method);
    message->setObject("params"_s, WTF::move(params));
    webkitWebViewGetPage(m_view).inspectorController().dispatchMessageFromFrontend(message->toJSONString());
}

void PageTarget::sendToInner(const String& innerTargetID, int id, const String& method, JSON::Object* params)
{
    if (!m_view)
        return;
    auto inner = JSON::Object::create();
    inner->setInteger("id"_s, id);
    inner->setString("method"_s, method);
    if (params)
        inner->setObject("params"_s, *params);

    // The wrapper carries the same id, so that an error from the relay
    // itself can be matched to the command it was carrying.
    auto outerParams = JSON::Object::create();
    outerParams->setString("targetId"_s, innerTargetID);
    outerParams->setString("message"_s, inner->toJSONString());
    if (logLevel() > 1)
        logMessage(makeString("    wip "_s, m_targetID.left(4), " >"_s).utf8().data(), inner->toJSONString());
    auto outer = JSON::Object::create();
    outer->setInteger("id"_s, id);
    outer->setString("method"_s, "Target.sendMessageToTarget"_s);
    outer->setObject("params"_s, WTF::move(outerParams));
    webkitWebViewGetPage(m_view).inspectorController().dispatchMessageFromFrontend(outer->toJSONString());
}

void PageTarget::wip(ASCIILiteral method, RefPtr<JSON::Object>&& params, WIPReply&& reply)
{
    if (!m_inspectorConnected || !m_view) {
        if (reply)
            reply(nullptr, "Target closed"_s);
        return;
    }
    if (m_crashed) {
        // Nothing will answer until the client loads something.
        if (reply)
            reply(nullptr, "Target crashed"_s);
        return;
    }
    int id = m_nextCommandID++;
    auto current = m_innerTargets.find(m_innerTargetID);
    if (current == m_innerTargets.end() || !current->value.ready) {
        // No inner target (after a crash or a close), or one whose process
        // has not answered yet; a new view's process is still starting and
        // would lose the message.
        m_pendingCommands.set(id, PendingCommand { String(), WTF::move(reply) });
        m_queuedCommands.append({ id, method, WTF::move(params) });
        return;
    }
    m_pendingCommands.set(id, PendingCommand { m_innerTargetID, WTF::move(reply) });
    sendToInner(m_innerTargetID, id, method, params.get());
}

// Settings that every inner target must be given: they are sent now and
// again to each inner target that appears later.
void PageTarget::setState(ASCIILiteral method, RefPtr<JSON::Object>&& params)
{
    bool found = false;
    for (auto& entry : m_state) {
        if (entry.first == method) {
            entry.second = params;
            found = true;
        }
    }
    if (!found)
        m_state.append({ method, params });
    // A target that is not ready yet gets the whole state when it is set up.
    auto current = m_innerTargets.find(m_innerTargetID);
    if (m_inspectorConnected && current != m_innerTargets.end() && current->value.ready)
        wip(method, WTF::move(params));
}

void PageTarget::failPendingCommands(const String& message, const String& exceptInnerTargetID)
{
    Vector<int> ids;
    for (auto& entry : m_pendingCommands) {
        // Queued commands have no inner target yet and stay queued.
        if (!exceptInnerTargetID.isNull() && (entry.value.innerTargetID.isNull() || entry.value.innerTargetID == exceptInnerTargetID))
            continue;
        ids.append(entry.key);
    }
    // In the order they were sent, as answers would have come.
    std::ranges::sort(ids);
    for (int id : ids) {
        auto pending = m_pendingCommands.take(id);
        if (pending.reply)
            pending.reply(nullptr, message);
    }
}

void PageTarget::initializeInnerTarget(const String& innerTargetID)
{
    // The frame tree comes first: its reply tells which frame is the main
    // one, which the events that follow cannot be translated without.
    int id = m_nextCommandID++;
    m_pendingCommands.set(id, PendingCommand { innerTargetID, [this, weakThis = WeakPtr { *this }, innerTargetID](JSON::Object* result, const String&) {
        if (!weakThis)
            return;
        auto iterator = m_innerTargets.find(innerTargetID);
        if (iterator == m_innerTargets.end())
            return;
        iterator->value.ready = true;
        m_reconnectAttempts = 0;
        if (RefPtr tree = result ? result->getObject("frameTree"_s) : nullptr) {
            if (RefPtr frame = tree->getObject("frame"_s))
                iterator->value.mainFrameID = frame->getString("id"_s);
            iterator->value.frameTree = WTF::move(tree);
        }
        if (innerTargetID == m_innerTargetID) {
            flushQueuedCommands();
            flushBufferedEvents();
        }
    } });
    sendToInner(innerTargetID, id, "Page.getResourceTree"_s, nullptr);

    for (auto& entry : m_state)
        sendToInner(innerTargetID, m_nextCommandID++, entry.first, entry.second.get());

    // A view that has just been created has no process yet, and what is
    // sent to it, including the inspector connection itself, is lost. Until
    // the first answer arrives the connection is made again from scratch.
    // A page that is merely busy answers late, not never, so each wait is
    // longer than the last.
    unsigned generation = m_connectionGeneration;
    unsigned delay = std::min(150u << std::min(m_reconnectAttempts, 4u), 1200u);
    runAfter(delay, [this, weakThis = WeakPtr { *this }, innerTargetID, generation] {
        if (!weakThis || !m_inspectorConnected || !m_view || generation != m_connectionGeneration)
            return;
        auto iterator = m_innerTargets.find(innerTargetID);
        if (iterator == m_innerTargets.end() || iterator->value.ready)
            return;
        ++m_reconnectAttempts;
        reconnectInspector();
    });
}

void PageTarget::flushQueuedCommands()
{
    for (auto& command : std::exchange(m_queuedCommands, { })) {
        auto iterator = m_pendingCommands.find(command.id);
        if (iterator != m_pendingCommands.end())
            iterator->value.innerTargetID = m_innerTargetID;
        sendToInner(m_innerTargetID, command.id, command.method, command.params.get());
    }
}

void PageTarget::innerTargetBecameCurrent(const String& innerTargetID)
{
    failPendingCommands("Inspected target navigated or closed"_s, innerTargetID);
    m_innerTargetID = innerTargetID;
    m_idPrefix = makeString(++m_innerTargetSequence, '-');
    resetDocumentState();

    auto iterator = m_innerTargets.find(innerTargetID);
    if (iterator != m_innerTargets.end() && iterator->value.ready)
        flushQueuedCommands();
    flushBufferedEvents();
}

void PageTarget::flushBufferedEvents()
{
    auto iterator = m_innerTargets.find(m_innerTargetID);
    if (iterator == m_innerTargets.end() || !iterator->value.ready)
        return;
    if (RefPtr tree = std::exchange(iterator->value.frameTree, nullptr))
        frameTreeToCDP(*tree);
    for (auto& event : std::exchange(iterator->value.bufferedEvents, { })) {
        RefPtr params = event->getObject("params"_s);
        if (!params)
            params = JSON::Object::create();
        handleInnerEvent(event->getString("method"_s), *params);
    }
}

void PageTarget::resetDocumentState()
{
    if (!m_contexts.isEmpty()) {
        forEachSession([](Session& session) {
            if (session.runtimeEnabled)
                session.event("Runtime.executionContextsCleared"_s, JSON::Object::create());
        });
    }
    m_contexts.clear();
    m_globalObjects.clear();
    m_backendNodes.clear();
    m_frameOwnerNodes.clear();
    m_frames.clear();
    m_requestIDs.clear();
    m_documentURLs.clear();
    m_inflightRequests.clear();
    m_documentNodeID = 0;
    m_loadFired = false;
    m_almostIdleFired = false;
    m_idleFired = false;
    ++m_networkIdleGeneration;
}

String PageTarget::mainFrameID() const
{
    auto iterator = m_innerTargets.find(m_innerTargetID);
    return iterator == m_innerTargets.end() ? String() : iterator->value.mainFrameID;
}

void PageTarget::sendMessageToFrontend(const String& text)
{
    Ref protectedThis { *this };
    RefPtr value = JSON::Value::parseJSON(text);
    RefPtr message = value ? value->asObject() : nullptr;
    if (!message)
        return;

    auto method = message->getString("method"_s);
    if (logLevel() > 1 && method != "Target.dispatchMessageFromTarget"_s && !(method.isNull() && !message->getObject("error"_s)))
        logMessage(makeString("    wip "_s, m_targetID.left(4), " <"_s).utf8().data(), text);
    if (method.isNull()) {
        // The reply to a wrapper. Only a failure to relay is of interest.
        RefPtr error = message->getObject("error"_s);
        auto id = message->getInteger("id"_s);
        if (error && id) {
            auto pending = m_pendingCommands.take(*id);
            if (pending.reply)
                pending.reply(nullptr, error->getString("message"_s));
        }
        return;
    }

    RefPtr params = message->getObject("params"_s);
    if (!params)
        return;

    if (method == "Target.dispatchMessageFromTarget"_s) {
        handleInnerMessage(params->getString("targetId"_s), params->getString("message"_s));
        return;
    }
    if (method == "Target.targetCreated"_s) {
        RefPtr info = params->getObject("targetInfo"_s);
        if (!info)
            return;
        auto id = info->getString("targetId"_s);
        bool isPage = info->getString("type"_s) == "page"_s;
        if (isPage) {
            m_innerTargets.set(id, InnerTarget { });
            initializeInnerTarget(id);
        }
        if (info->getBoolean("isPaused"_s).value_or(false)) {
            auto resume = JSON::Object::create();
            resume->setString("targetId"_s, id);
            sendOuter("Target.resume"_s, WTF::move(resume));
        }
        if (isPage && !info->getBoolean("isProvisional"_s).value_or(false))
            innerTargetBecameCurrent(id);
        return;
    }
    if (method == "Target.didCommitProvisionalTarget"_s) {
        auto oldID = params->getString("oldTargetId"_s);
        auto newID = params->getString("newTargetId"_s);
        m_innerTargets.remove(oldID);
        if (m_innerTargets.contains(newID))
            innerTargetBecameCurrent(newID);
        return;
    }
    if (method == "Target.targetDestroyed"_s) {
        auto id = params->getString("targetId"_s);
        m_innerTargets.remove(id);
        if (id == m_innerTargetID) {
            m_innerTargetID = String();
            failPendingCommands("Inspected target navigated or closed"_s, emptyString());
        }
        return;
    }
}

void PageTarget::handleInnerMessage(const String& innerTargetID, const String& text)
{
    if (logLevel() > 1)
        logMessage(makeString("    wip "_s, m_targetID.left(4), " <"_s).utf8().data(), text);
    RefPtr value = JSON::Value::parseJSON(text);
    RefPtr message = value ? value->asObject() : nullptr;
    if (!message)
        return;

    if (auto id = message->getInteger("id"_s)) {
        auto pending = m_pendingCommands.take(*id);
        if (!pending.reply)
            return;
        if (RefPtr error = message->getObject("error"_s)) {
            pending.reply(nullptr, error->getString("message"_s));
            return;
        }
        RefPtr result = message->getObject("result"_s);
        if (!result)
            result = JSON::Object::create();
        pending.reply(result.get(), { });
        return;
    }

    auto iterator = m_innerTargets.find(innerTargetID);
    if (iterator == m_innerTargets.end())
        return;
    // Events of a target that is still provisional, or whose main frame is
    // not known yet, wait until they can be told to the client in order.
    if (innerTargetID != m_innerTargetID || !iterator->value.ready) {
        iterator->value.bufferedEvents.append(message.releaseNonNull());
        return;
    }
    RefPtr params = message->getObject("params"_s);
    if (!params)
        params = JSON::Object::create();
    handleInnerEvent(message->getString("method"_s), *params);
}

// MARK: Frames and execution contexts

// The main frame has the id of the target, as in Chrome, which also keeps
// it the same when a navigation moves the page to another process.
String PageTarget::toCDPFrameID(const String& innerFrameID) const
{
    if (innerFrameID.isEmpty())
        return innerFrameID;
    return innerFrameID == mainFrameID() ? m_targetID : uniqueID(innerFrameID);
}

String PageTarget::toInnerFrameID(const String& cdpFrameID) const
{
    return cdpFrameID == m_targetID ? mainFrameID() : innerID(cdpFrameID);
}

// WebKit numbers frames, loaders and requests per process, starting over in
// each one. A page that moves to another process would hand out the same
// ids again for different things, so they are told apart by a prefix.
String PageTarget::uniqueID(const String& innerID) const
{
    return innerID.isEmpty() ? innerID : makeString(m_idPrefix, innerID);
}

String PageTarget::innerID(const String& uniqueID) const
{
    return uniqueID.startsWith(m_idPrefix) ? uniqueID.substring(m_idPrefix.length()) : uniqueID;
}

void PageTarget::rememberFrame(JSON::Object& innerFrame)
{
    Frame frame;
    frame.parentID = innerFrame.getString("parentId"_s);
    frame.loaderID = innerFrame.getString("loaderId"_s);
    frame.url = innerFrame.getString("url"_s);
    if (auto id = innerFrame.getString("id"_s); !id.isNull())
        m_frames.set(id, WTF::move(frame));
}

Ref<JSON::Object> PageTarget::frameToCDP(JSON::Object& innerFrame) const
{
    auto frame = JSON::Object::create();
    frame->setString("id"_s, toCDPFrameID(innerFrame.getString("id"_s)));
    if (auto parentID = innerFrame.getString("parentId"_s); !parentID.isEmpty())
        frame->setString("parentId"_s, toCDPFrameID(parentID));
    frame->setString("loaderId"_s, uniqueID(innerFrame.getString("loaderId"_s)));
    if (auto name = innerFrame.getString("name"_s); !name.isNull())
        frame->setString("name"_s, name);
    auto url = innerFrame.getString("url"_s);
    if (auto hash = url.find('#'); hash != notFound) {
        frame->setString("urlFragment"_s, url.substring(hash));
        url = url.left(hash);
    }
    if (url.isEmpty())
        url = "about:blank"_s;
    frame->setString("url"_s, url);
    frame->setString("domainAndRegistry"_s, emptyString());
    frame->setString("securityOrigin"_s, originOf(url));
    frame->setString("mimeType"_s, innerFrame.getString("mimeType"_s));
    frame->setString("secureContextType"_s, url.startsWith("http:"_s) ? "InsecureScheme"_s : "Secure"_s);
    frame->setString("crossOriginIsolatedContextType"_s, "NotIsolated"_s);
    frame->setArray("gatedAPIFeatures"_s, JSON::Array::create());
    return frame;
}

Ref<JSON::Object> PageTarget::frameTreeToCDP(JSON::Object& innerTree)
{
    auto tree = JSON::Object::create();
    if (RefPtr frame = innerTree.getObject("frame"_s)) {
        rememberFrame(*frame);
        tree->setObject("frame"_s, frameToCDP(*frame));
    }
    if (RefPtr children = innerTree.getArray("childFrames"_s)) {
        auto array = JSON::Array::create();
        for (auto& child : *children) {
            if (RefPtr object = child->asObject())
                array->pushObject(frameTreeToCDP(*object));
        }
        tree->setArray("childFrames"_s, WTF::move(array));
    }
    return tree;
}

// Isolated worlds are emulated: each one a client asks for is announced as
// a second context of every frame, with its own id, and commands sent to it
// run in the frame's main world. Client libraries only need their helper
// world to exist; they do not rely on it being separate.
int PageTarget::worldContextID(int innerContextID, unsigned worldIndex) const
{
    return worldContextBase + innerContextID * maxWorlds + worldIndex;
}

int PageTarget::toInnerContextID(int cdpContextID) const
{
    return cdpContextID >= worldContextBase ? (cdpContextID - worldContextBase) / maxWorlds : cdpContextID;
}

int PageTarget::defaultContextID() const
{
    auto mainFrame = mainFrameID();
    for (auto& entry : m_contexts) {
        if (entry.value.frameID == mainFrame)
            return entry.key;
    }
    return 0;
}

int PageTarget::contextForFrame(const String& innerFrameID) const
{
    for (auto& entry : m_contexts) {
        if (entry.value.frameID == innerFrameID)
            return entry.key;
    }
    return 0;
}

static Ref<JSON::Object> contextDescription(int id, const String& name, const String& origin, const String& frameID, bool isDefault)
{
    auto auxData = JSON::Object::create();
    auxData->setBoolean("isDefault"_s, isDefault);
    auxData->setString("type"_s, isDefault ? "default"_s : "isolated"_s);
    auxData->setString("frameId"_s, frameID);
    auto context = JSON::Object::create();
    context->setInteger("id"_s, id);
    context->setString("origin"_s, origin);
    context->setString("name"_s, name);
    context->setString("uniqueId"_s, String::number(id));
    context->setObject("auxData"_s, WTF::move(auxData));
    auto params = JSON::Object::create();
    params->setObject("context"_s, WTF::move(context));
    return params;
}

void PageTarget::announceContext(Session& session, int innerContextID)
{
    auto iterator = m_contexts.find(innerContextID);
    if (!session.runtimeEnabled || iterator == m_contexts.end())
        return;
    auto frameID = toCDPFrameID(iterator->value.frameID);
    auto& origin = iterator->value.origin;
    session.event("Runtime.executionContextCreated"_s, contextDescription(innerContextID, emptyString(), origin, frameID, true));
    for (unsigned index = 0; index < m_worlds.size(); ++index)
        session.event("Runtime.executionContextCreated"_s, contextDescription(worldContextID(innerContextID, index), m_worlds[index], origin, frameID, false));
}

unsigned PageTarget::ensureWorld(const String& name)
{
    auto index = m_worlds.find(name);
    if (index != notFound)
        return index;
    if (m_worlds.size() == maxWorlds)
        return maxWorlds - 1;
    m_worlds.append(name);
    index = m_worlds.size() - 1;
    forEachSession([&](Session& session) {
        if (!session.runtimeEnabled)
            return;
        for (auto& entry : m_contexts)
            session.event("Runtime.executionContextCreated"_s, contextDescription(worldContextID(entry.key, index), name, entry.value.origin, toCDPFrameID(entry.value.frameID), false));
    });
    return index;
}

void PageTarget::destroyContextsOfFrame(const String& innerFrameID)
{
    Vector<int> ids;
    for (auto& entry : m_contexts) {
        if (entry.value.frameID == innerFrameID)
            ids.append(entry.key);
    }
    for (int id : ids) {
        m_contexts.remove(id);
        m_globalObjects.remove(id);
        forEachSession([&](Session& session) {
            if (!session.runtimeEnabled)
                return;
            auto announce = [&](int contextID) {
                auto params = JSON::Object::create();
                params->setInteger("executionContextId"_s, contextID);
                params->setString("executionContextUniqueId"_s, String::number(contextID));
                session.event("Runtime.executionContextDestroyed"_s, WTF::move(params));
            };
            announce(id);
            for (unsigned index = 0; index < m_worlds.size(); ++index)
                announce(worldContextID(id, index));
        });
    }
}

void PageTarget::removeFrame(const String& innerFrameID, bool announce)
{
    Vector<String> children;
    for (auto& entry : m_frames) {
        if (entry.value.parentID == innerFrameID)
            children.append(entry.key);
    }
    for (auto& child : children)
        removeFrame(child, true);

    destroyContextsOfFrame(innerFrameID);
    if (!announce)
        return;
    if (!m_frames.remove(innerFrameID))
        return;
    forEachSession([&](Session& session) {
        if (!session.pageEnabled)
            return;
        auto params = JSON::Object::create();
        params->setString("frameId"_s, toCDPFrameID(innerFrameID));
        params->setString("reason"_s, "remove"_s);
        session.event("Page.frameDetached"_s, WTF::move(params));
    });
}

void PageTarget::emitLifecycle(const String& innerFrameID, ASCIILiteral name)
{
    auto iterator = m_frames.find(innerFrameID);
    auto loaderID = iterator == m_frames.end() ? String() : iterator->value.loaderID;
    forEachSession([&](Session& session) {
        if (!session.pageEnabled || !session.lifecycleEnabled)
            return;
        auto params = JSON::Object::create();
        params->setString("frameId"_s, toCDPFrameID(innerFrameID));
        params->setString("loaderId"_s, uniqueID(loaderID));
        params->setString("name"_s, name);
        params->setDouble("timestamp"_s, monotonicSeconds());
        session.event("Page.lifecycleEvent"_s, WTF::move(params));
    });
}

// "Network idle" is not something WebKit reports. It is derived here the
// way Chrome defines it: the load event has fired and no request has been
// in flight for half a second.
void PageTarget::scheduleNetworkIdle()
{
    // "Almost idle" allows two requests, which is what lets a page that
    // keeps a connection open for updates be seen as loaded at all.
    unsigned generation = ++m_networkIdleGeneration;
    if (!m_loadFired || m_inflightRequests.size() > 2)
        return;
    runAfter(500, [this, weakThis = WeakPtr { *this }, generation] {
        if (!weakThis || generation != m_networkIdleGeneration || !m_inspectorConnected)
            return;
        if (!std::exchange(m_almostIdleFired, true))
            emitLifecycle(mainFrameID(), "networkAlmostIdle"_s);
        if (m_inflightRequests.isEmpty() && !std::exchange(m_idleFired, true))
            emitLifecycle(mainFrameID(), "networkIdle"_s);
    });
}

// MARK: Events from the page

static ASCIILiteral resourceTypeToCDP(const String& type)
{
    if (type == "Document"_s)
        return "Document"_s;
    if (type == "StyleSheet"_s)
        return "Stylesheet"_s;
    if (type == "Image"_s)
        return "Image"_s;
    if (type == "Font"_s)
        return "Font"_s;
    if (type == "Script"_s)
        return "Script"_s;
    if (type == "XHR"_s)
        return "XHR"_s;
    if (type == "Fetch"_s)
        return "Fetch"_s;
    if (type == "Ping"_s || type == "Beacon"_s)
        return "Ping"_s;
    if (type == "WebSocket"_s)
        return "WebSocket"_s;
    if (type == "EventSource"_s)
        return "EventSource"_s;
    return "Other"_s;
}

static Ref<JSON::Object> responseToCDP(JSON::Object& inner)
{
    auto response = JSON::Object::create();
    auto url = inner.getString("url"_s);
    auto source = inner.getString("source"_s);
    response->setString("url"_s, url);
    response->setInteger("status"_s, inner.getInteger("status"_s).value_or(0));
    response->setString("statusText"_s, inner.getString("statusText"_s));
    RefPtr headers = inner.getObject("headers"_s);
    response->setObject("headers"_s, headers ? headers.releaseNonNull() : JSON::Object::create());
    response->setString("mimeType"_s, inner.getString("mimeType"_s));
    response->setString("charset"_s, emptyString());
    if (RefPtr requestHeaders = inner.getObject("requestHeaders"_s))
        response->setObject("requestHeaders"_s, requestHeaders.releaseNonNull());
    response->setBoolean("connectionReused"_s, false);
    response->setInteger("connectionId"_s, 0);
    response->setBoolean("fromDiskCache"_s, source == "disk-cache"_s || source == "memory-cache"_s);
    response->setBoolean("fromServiceWorker"_s, source == "service-worker"_s);
    response->setInteger("encodedDataLength"_s, 0);
    response->setString("protocol"_s, "http/1.1"_s);
    response->setString("securityState"_s, url.startsWith("https:"_s) ? "secure"_s : "insecure"_s);
    return response;
}

String PageTarget::toCDPRequestID(const String& innerRequestID) const
{
    auto iterator = m_requestIDs.find(innerRequestID);
    return uniqueID(iterator == m_requestIDs.end() ? innerRequestID : iterator->value);
}

String PageTarget::toInnerRequestID(const String& cdpRequestID) const
{
    auto requestID = innerID(cdpRequestID);
    for (auto& entry : m_requestIDs) {
        if (entry.value == requestID)
            return entry.key;
    }
    return requestID;
}

void PageTarget::handleRequestWillBeSent(JSON::Object& params)
{
    auto innerRequestID = params.getString("requestId"_s);
    auto loaderID = params.getString("loaderId"_s);
    auto type = params.getString("type"_s);
    if (innerRequestID.isNull())
        return;
    // In CDP the request that loads a document has the id of its loader;
    // clients find the response of a navigation that way.
    if (type == "Document"_s && !loaderID.isEmpty())
        m_requestIDs.set(innerRequestID, loaderID);
    m_inflightRequests.add(innerRequestID);
    scheduleNetworkIdle();

    RefPtr innerRequest = params.getObject("request"_s);
    RefPtr redirectResponse = params.getObject("redirectResponse"_s);
    RefPtr initiator = params.getObject("initiator"_s);
    forEachSession([&](Session& session) {
        if (!session.networkEnabled)
            return;
        auto request = JSON::Object::create();
        if (innerRequest) {
            auto url = innerRequest->getString("url"_s);
            if (auto hash = url.find('#'); hash != notFound) {
                request->setString("urlFragment"_s, url.substring(hash));
                url = url.left(hash);
            }
            request->setString("url"_s, url);
            request->setString("method"_s, innerRequest->getString("method"_s));
            RefPtr headers = innerRequest->getObject("headers"_s);
            request->setObject("headers"_s, headers ? headers.releaseNonNull() : JSON::Object::create());
            auto postData = innerRequest->getString("postData"_s);
            if (!postData.isNull())
                request->setString("postData"_s, postData);
            request->setBoolean("hasPostData"_s, !postData.isNull());
        }
        request->setString("initialPriority"_s, "Medium"_s);
        request->setString("referrerPolicy"_s, "strict-origin-when-cross-origin"_s);
        request->setString("mixedContentType"_s, "none"_s);

        auto cdpInitiator = JSON::Object::create();
        auto initiatorType = initiator ? initiator->getString("type"_s) : String();
        cdpInitiator->setString("type"_s, initiatorType == "parser"_s || initiatorType == "script"_s ? initiatorType : "other"_s);

        auto event = JSON::Object::create();
        event->setString("requestId"_s, toCDPRequestID(innerRequestID));
        event->setString("loaderId"_s, uniqueID(loaderID));
        event->setString("documentURL"_s, params.getString("documentURL"_s));
        event->setObject("request"_s, WTF::move(request));
        event->setDouble("timestamp"_s, monotonicSeconds());
        event->setDouble("wallTime"_s, params.getDouble("walltime"_s).value_or(g_get_real_time() / 1000000.0));
        event->setObject("initiator"_s, WTF::move(cdpInitiator));
        event->setBoolean("redirectHasExtraInfo"_s, false);
        if (redirectResponse)
            event->setObject("redirectResponse"_s, responseToCDP(*redirectResponse));
        event->setString("type"_s, resourceTypeToCDP(type));
        event->setString("frameId"_s, toCDPFrameID(params.getString("frameId"_s)));
        event->setBoolean("hasUserGesture"_s, false);
        session.event("Network.requestWillBeSent"_s, WTF::move(event));
    });
}

static int contextOfObject(const String& objectID)
{
    RefPtr value = JSON::Value::parseJSON(objectID);
    RefPtr object = value ? value->asObject() : nullptr;
    return object ? object->getInteger("injectedScriptId"_s).value_or(0) : 0;
}

void PageTarget::handleConsoleMessage(JSON::Object& message)
{
    auto source = message.getString("source"_s);
    auto level = message.getString("level"_s);
    auto text = message.getString("text"_s);
    auto type = message.getString("type"_s);
    auto url = message.getString("url"_s);
    int line = message.getInteger("line"_s).value_or(0);
    int column = message.getInteger("column"_s).value_or(0);
    RefPtr parameters = message.getArray("parameters"_s);
    double timestamp = g_get_real_time() / 1000.0;

    if (source == "console-api"_s) {
        int contextID = 0;
        if (parameters) {
            for (auto& parameter : *parameters) {
                RefPtr object = parameter->asObject();
                if (object && !contextID)
                    contextID = contextOfObject(object->getString("objectId"_s));
            }
        }
        if (!contextID)
            contextID = defaultContextID();

        if (parameters && parameters->length() >= 3) {
            RefPtr first = parameters->get(0)->asObject();
            if (first && first->getString("value"_s) == bindingMarker) {
                RefPtr nameObject = parameters->get(1)->asObject();
                RefPtr payloadObject = parameters->get(2)->asObject();
                auto name = nameObject ? nameObject->getString("value"_s) : String();
                for (auto& binding : m_bindings) {
                    if (binding.name != name)
                        continue;
                    int reportedContext = contextID;
                    if (!binding.worldName.isNull()) {
                        if (auto index = m_worlds.find(binding.worldName); index != notFound)
                            reportedContext = worldContextID(contextID, index);
                    }
                    forEachSession([&](Session& session) {
                        if (!session.runtimeEnabled)
                            return;
                        auto event = JSON::Object::create();
                        event->setString("name"_s, name);
                        event->setString("payload"_s, payloadObject ? payloadObject->getString("value"_s) : String());
                        event->setInteger("executionContextId"_s, reportedContext);
                        session.event("Runtime.bindingCalled"_s, WTF::move(event));
                    });
                }
                return;
            }
        }

        ASCIILiteral cdpType = "log"_s;
        if (type == "dir"_s)
            cdpType = "dir"_s;
        else if (type == "dirxml"_s)
            cdpType = "dirxml"_s;
        else if (type == "table"_s)
            cdpType = "table"_s;
        else if (type == "trace"_s)
            cdpType = "trace"_s;
        else if (type == "clear"_s)
            cdpType = "clear"_s;
        else if (type == "startGroup"_s)
            cdpType = "startGroup"_s;
        else if (type == "startGroupCollapsed"_s)
            cdpType = "startGroupCollapsed"_s;
        else if (type == "endGroup"_s)
            cdpType = "endGroup"_s;
        else if (type == "assert"_s)
            cdpType = "assert"_s;
        else if (type == "timing"_s)
            cdpType = "timeEnd"_s;
        else if (level == "info"_s)
            cdpType = "info"_s;
        else if (level == "warning"_s)
            cdpType = "warning"_s;
        else if (level == "error"_s)
            cdpType = "error"_s;
        else if (level == "debug"_s)
            cdpType = "debug"_s;

        forEachSession([&](Session& session) {
            if (!session.runtimeEnabled)
                return;
            auto arguments = JSON::Array::create();
            if (parameters) {
                for (auto& parameter : *parameters) {
                    if (RefPtr object = parameter->asObject())
                        arguments->pushObject(remoteObjectToCDP(*object));
                }
            } else {
                auto argument = JSON::Object::create();
                argument->setString("type"_s, "string"_s);
                argument->setString("value"_s, text);
                arguments->pushObject(WTF::move(argument));
            }
            auto frame = JSON::Object::create();
            frame->setString("functionName"_s, emptyString());
            frame->setString("scriptId"_s, "0"_s);
            frame->setString("url"_s, url.isNull() ? emptyString() : url);
            frame->setInteger("lineNumber"_s, std::max(0, line - 1));
            frame->setInteger("columnNumber"_s, std::max(0, column - 1));
            auto frames = JSON::Array::create();
            frames->pushObject(WTF::move(frame));
            auto stackTrace = JSON::Object::create();
            stackTrace->setArray("callFrames"_s, WTF::move(frames));

            auto event = JSON::Object::create();
            event->setString("type"_s, cdpType);
            event->setArray("args"_s, WTF::move(arguments));
            event->setInteger("executionContextId"_s, contextID);
            event->setDouble("timestamp"_s, timestamp);
            event->setObject("stackTrace"_s, WTF::move(stackTrace));
            session.event("Runtime.consoleAPICalled"_s, WTF::move(event));
        });
        return;
    }

    if (source == "javascript"_s && level == "error"_s) {
        forEachSession([&](Session& session) {
            if (!session.runtimeEnabled)
                return;
            auto className = "Error"_str;
            if (auto colon = text.find(": "_s); colon != notFound && colon < 40 && !text.left(colon).contains(' '))
                className = text.left(colon);
            auto exception = JSON::Object::create();
            exception->setString("type"_s, "object"_s);
            exception->setString("subtype"_s, "error"_s);
            exception->setString("className"_s, className);
            exception->setString("description"_s, text);
            auto details = JSON::Object::create();
            details->setInteger("exceptionId"_s, m_nextExceptionID++);
            details->setString("text"_s, "Uncaught"_s);
            details->setInteger("lineNumber"_s, std::max(0, line - 1));
            details->setInteger("columnNumber"_s, std::max(0, column - 1));
            if (!url.isNull())
                details->setString("url"_s, url);
            details->setObject("exception"_s, WTF::move(exception));
            details->setInteger("executionContextId"_s, defaultContextID());
            auto event = JSON::Object::create();
            event->setDouble("timestamp"_s, timestamp);
            event->setObject("exceptionDetails"_s, WTF::move(details));
            session.event("Runtime.exceptionThrown"_s, WTF::move(event));
        });
    }

    forEachSession([&](Session& session) {
        if (!session.logEnabled)
            return;
        auto entry = JSON::Object::create();
        entry->setString("source"_s, source == "network"_s || source == "javascript"_s || source == "security"_s || source == "storage"_s || source == "rendering"_s ? source : "other"_s);
        entry->setString("level"_s, level == "error"_s ? "error"_s : level == "warning"_s ? "warning"_s : level == "debug"_s ? "verbose"_s : "info"_s);
        entry->setString("text"_s, text);
        entry->setDouble("timestamp"_s, timestamp);
        if (!url.isNull())
            entry->setString("url"_s, url);
        entry->setInteger("lineNumber"_s, std::max(0, line - 1));
        auto event = JSON::Object::create();
        event->setObject("entry"_s, WTF::move(entry));
        session.event("Log.entryAdded"_s, WTF::move(event));
    });
}

// Remembers which DOM nodes are frame owners, from any node description the
// page sends, and gives every node the backendNodeId CDP expects.
void PageTarget::scanNode(JSON::Object& node)
{
    int nodeID = node.getInteger("nodeId"_s).value_or(0);
    node.setInteger("backendNodeId"_s, nodeID);
    // WebKit puts on every node the frame it is in. CDP puts on a frame
    // element the frame it holds, which here is the frame of the document
    // nested in the element.
    node.remove("frameId"_s);
    if (RefPtr content = node.getObject("contentDocument"_s)) {
        if (auto frameID = content->getString("frameId"_s); !frameID.isEmpty()) {
            m_frameOwnerNodes.set(nodeID, frameID);
            node.setString("frameId"_s, toCDPFrameID(frameID));
        }
    }
    for (auto key : { "children"_s, "shadowRoots"_s, "pseudoElements"_s }) {
        if (RefPtr array = node.getArray(key)) {
            for (auto& child : *array) {
                if (RefPtr object = child->asObject())
                    scanNode(*object);
            }
        }
    }
    for (auto key : { "contentDocument"_s, "templateContent"_s }) {
        if (RefPtr object = node.getObject(key))
            scanNode(*object);
    }
}

void PageTarget::handleInnerEvent(const String& method, JSON::Object& params)
{
    if (method == "Runtime.executionContextCreated"_s) {
        RefPtr context = params.getObject("context"_s);
        if (!context || context->getString("type"_s) != "normal"_s)
            return;
        int id = context->getInteger("id"_s).value_or(0);
        auto frameID = context->getString("frameId"_s);
        // Ids at and above worldContextBase stand for emulated worlds.
        if (id <= 0 || id >= worldContextBase)
            return;
        // A frame has one main world; a new one replaces what was there.
        destroyContextsOfFrame(frameID);
        Context entry;
        entry.frameID = frameID;
        if (auto frame = m_frames.find(frameID); frame != m_frames.end())
            entry.origin = originOf(frame->value.url);
        m_contexts.set(id, WTF::move(entry));
        forEachSession([&](Session& session) {
            announceContext(session, id);
        });
        return;
    }

    if (method == "Page.frameNavigated"_s) {
        RefPtr frame = params.getObject("frame"_s);
        if (!frame)
            return;
        auto frameID = frame->getString("id"_s);
        auto parentID = frame->getString("parentId"_s);
        bool isMainFrame = parentID.isEmpty();
        // For a history navigation WebKit reports the address of the
        // document being left. The response for this loader has the right one.
        if (auto url = m_documentURLs.find(frame->getString("loaderId"_s)); url != m_documentURLs.end())
            frame->setString("url"_s, url->value);
        if (isMainFrame) {
            if (auto target = m_innerTargets.find(m_innerTargetID); target != m_innerTargets.end())
                target->value.mainFrameID = frameID;
        }
        bool isNewFrame = !m_frames.contains(frameID);
        // The document the frame held is gone, with its child frames.
        removeFrame(frameID, false);
        rememberFrame(*frame);
        if (isMainFrame) {
            m_backendNodes.clear();
            m_frameOwnerNodes.clear();
            m_documentNodeID = 0;
            m_loadFired = false;
            m_almostIdleFired = false;
            m_idleFired = false;
            ++m_networkIdleGeneration;
            // Everything recorded for the documents that are gone goes too,
            // or a page that is used for days never stops growing. Requests
            // of the old document that were cut off are not reported as
            // finished, and would keep the new one from ever being idle.
            auto loaderID = frame->getString("loaderId"_s);
            m_requestIDs.removeIf([&](auto& entry) {
                return entry.value != loaderID;
            });
            m_documentURLs.removeIf([&](auto& entry) {
                return entry.key != loaderID;
            });
            m_inflightRequests.clear();
        }

        if (isNewFrame && !isMainFrame) {
            forEachSession([&](Session& session) {
                if (!session.pageEnabled)
                    return;
                auto event = JSON::Object::create();
                event->setString("frameId"_s, toCDPFrameID(frameID));
                event->setString("parentFrameId"_s, toCDPFrameID(parentID));
                session.event("Page.frameAttached"_s, WTF::move(event));
            });
        }
        emitLifecycle(frameID, "init"_s);
        forEachSession([&](Session& session) {
            if (!session.pageEnabled)
                return;
            auto event = JSON::Object::create();
            event->setObject("frame"_s, frameToCDP(*frame));
            event->setString("type"_s, "Navigation"_s);
            session.event("Page.frameNavigated"_s, WTF::move(event));
        });
        emitLifecycle(frameID, "commit"_s);
        if (isMainFrame) {
            // The page WebKit shows after a failed load commits like any
            // other. It is not the navigation a client may be waiting for.
            bool isErrorPage = std::exchange(m_errorPageExpected, false) && frame->getString("url"_s) == "about:blank"_s;
            if (!isErrorPage)
                finishNavigation(uniqueID(frame->getString("loaderId"_s)), { });
            else if (!m_pendingErrorText.isNull())
                finishNavigation({ }, std::exchange(m_pendingErrorText, String()));
        } else {
            // WebKit reports load progress for the main frame only.
            emitLifecycle(frameID, "DOMContentLoaded"_s);
            emitLifecycle(frameID, "load"_s);
        }
        return;
    }

    if (method == "Page.frameDetached"_s) {
        removeFrame(params.getString("frameId"_s), true);
        return;
    }

    if (method == "Page.domContentEventFired"_s || method == "Page.loadEventFired"_s) {
        bool isLoad = method == "Page.loadEventFired"_s;
        forEachSession([&](Session& session) {
            if (!session.pageEnabled)
                return;
            auto event = JSON::Object::create();
            event->setDouble("timestamp"_s, monotonicSeconds());
            session.event(isLoad ? "Page.loadEventFired"_s : "Page.domContentEventFired"_s, WTF::move(event));
        });
        emitLifecycle(mainFrameID(), isLoad ? "load"_s : "DOMContentLoaded"_s);
        if (isLoad) {
            m_loadFired = true;
            scheduleNetworkIdle();
        }
        return;
    }

    if (method == "Network.requestWillBeSent"_s) {
        handleRequestWillBeSent(params);
        return;
    }

    if (method == "Network.responseReceived"_s) {
        RefPtr response = params.getObject("response"_s);
        if (response && params.getString("type"_s) == "Document"_s && !params.getString("loaderId"_s).isNull())
            m_documentURLs.set(params.getString("loaderId"_s), response->getString("url"_s));
        forEachSession([&](Session& session) {
            if (!session.networkEnabled || !response)
                return;
            auto event = JSON::Object::create();
            event->setString("requestId"_s, toCDPRequestID(params.getString("requestId"_s)));
            event->setString("loaderId"_s, uniqueID(params.getString("loaderId"_s)));
            event->setDouble("timestamp"_s, monotonicSeconds());
            event->setString("type"_s, resourceTypeToCDP(params.getString("type"_s)));
            event->setObject("response"_s, responseToCDP(*response));
            event->setBoolean("hasExtraInfo"_s, false);
            event->setString("frameId"_s, toCDPFrameID(params.getString("frameId"_s)));
            session.event("Network.responseReceived"_s, WTF::move(event));
        });
        return;
    }

    if (method == "Network.dataReceived"_s) {
        forEachSession([&](Session& session) {
            if (!session.networkEnabled)
                return;
            auto event = JSON::Object::create();
            event->setString("requestId"_s, toCDPRequestID(params.getString("requestId"_s)));
            event->setDouble("timestamp"_s, monotonicSeconds());
            event->setInteger("dataLength"_s, params.getInteger("dataLength"_s).value_or(0));
            event->setInteger("encodedDataLength"_s, params.getInteger("encodedDataLength"_s).value_or(0));
            session.event("Network.dataReceived"_s, WTF::move(event));
        });
        return;
    }

    if (method == "Network.loadingFinished"_s || method == "Network.loadingFailed"_s) {
        bool failed = method == "Network.loadingFailed"_s;
        auto innerRequestID = params.getString("requestId"_s);
        forEachSession([&](Session& session) {
            if (!session.networkEnabled)
                return;
            auto event = JSON::Object::create();
            event->setString("requestId"_s, toCDPRequestID(innerRequestID));
            event->setDouble("timestamp"_s, monotonicSeconds());
            if (failed) {
                event->setString("type"_s, "Other"_s);
                event->setString("errorText"_s, params.getString("errorText"_s));
                event->setBoolean("canceled"_s, params.getBoolean("canceled"_s).value_or(false));
            } else {
                int length = 0;
                if (RefPtr metrics = params.getObject("metrics"_s))
                    length = metrics->getInteger("responseBodyBytesReceived"_s).value_or(0);
                event->setInteger("encodedDataLength"_s, std::max(0, length));
            }
            session.event(failed ? "Network.loadingFailed"_s : "Network.loadingFinished"_s, WTF::move(event));
        });
        m_inflightRequests.remove(innerRequestID);
        scheduleNetworkIdle();
        return;
    }

    if (method == "Network.requestServedFromMemoryCache"_s) {
        // CDP reports a cache hit as an ordinary, instant exchange.
        RefPtr resource = params.getObject("resource"_s);
        RefPtr response = resource ? resource->getObject("response"_s) : nullptr;
        if (!resource || !response)
            return;
        auto request = JSON::Object::create();
        request->setString("url"_s, resource->getString("url"_s));
        request->setString("method"_s, "GET"_s);
        request->setObject("headers"_s, JSON::Object::create());
        auto sent = JSON::Object::create();
        sent->setString("requestId"_s, params.getString("requestId"_s));
        sent->setString("frameId"_s, params.getString("frameId"_s));
        sent->setString("loaderId"_s, params.getString("loaderId"_s));
        sent->setString("documentURL"_s, params.getString("documentURL"_s));
        sent->setString("type"_s, resource->getString("type"_s));
        sent->setObject("request"_s, WTF::move(request));
        handleRequestWillBeSent(sent);

        auto received = JSON::Object::create();
        received->setString("requestId"_s, params.getString("requestId"_s));
        received->setString("frameId"_s, params.getString("frameId"_s));
        received->setString("loaderId"_s, params.getString("loaderId"_s));
        received->setString("type"_s, resource->getString("type"_s));
        response->setString("source"_s, "memory-cache"_s);
        received->setObject("response"_s, response.releaseNonNull());
        handleInnerEvent("Network.responseReceived"_s, received);

        auto finished = JSON::Object::create();
        finished->setString("requestId"_s, params.getString("requestId"_s));
        handleInnerEvent("Network.loadingFinished"_s, finished);
        return;
    }

    if (method == "Console.messageAdded"_s) {
        if (RefPtr message = params.getObject("message"_s))
            handleConsoleMessage(*message);
        return;
    }

    if (method.startsWith("DOM."_s)) {
        if (method == "DOM.documentUpdated"_s) {
            m_documentNodeID = 0;
            m_frameOwnerNodes.clear();
        }
        if (RefPtr nodes = params.getArray("nodes"_s)) {
            for (auto& node : *nodes) {
                if (RefPtr object = node->asObject())
                    scanNode(*object);
            }
            if (m_capturedChildNodes)
                m_capturedChildNodes->set(params.getInteger("parentId"_s).value_or(0), nodes);
        }
        if (RefPtr node = params.getObject("node"_s))
            scanNode(*node);

        static constexpr ASCIILiteral forwarded[] = {
            "DOM.documentUpdated"_s, "DOM.setChildNodes"_s, "DOM.attributeModified"_s, "DOM.attributeRemoved"_s,
            "DOM.characterDataModified"_s, "DOM.childNodeCountUpdated"_s, "DOM.childNodeInserted"_s, "DOM.childNodeRemoved"_s,
        };
        for (auto name : forwarded) {
            if (method != name)
                continue;
            forEachSession([&](Session& session) {
                if (session.domEnabled)
                    session.event(name, Ref { params });
            });
        }
        return;
    }
}

} // namespace Zebkit::CDP
