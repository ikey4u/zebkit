#pragma once

#include "CDP.h"
#include "CDPPlatform.h"
#include <JavaScriptCore/InspectorFrontendChannel.h>
#include <wtf/Box.h>
#include <wtf/Function.h>
#include <wtf/HashMap.h>
#include <wtf/HashSet.h>
#include <wtf/JSONValues.h>
#include <wtf/RefCounted.h>
#include <wtf/SetForScope.h>
#include <wtf/Vector.h>
#include <wtf/WeakPtr.h>
#include <wtf/text/StringHash.h>
#include <wtf/text/WTFString.h>

namespace WebKit {
class WebOpenPanelResultListenerProxy;
}

namespace Zebkit::CDP {

class Connection;
class PageTarget;
class Server;

// Completes one CDP command: a result object, or an error message.
using Reply = Function<void(RefPtr<JSON::Object>&&, const String& error)>;
// Completes one Web Inspector Protocol command sent to the page.
using WIPReply = Function<void(JSON::Object* result, const String& error)>;

// WTF's maps reserve two key values for their own use and cannot look up or
// hold a null string. Ids arrive from clients and from the page, so every
// value they can send must be harmless: integer ids are 32 bits wide and the
// keys 64, which puts the reserved values out of their reach.
template<typename V> using IntMap = HashMap<int64_t, V, DefaultHash<int64_t>, WTF::UnsignedWithZeroKeyHashTraits<int64_t>>;
struct NullSafeStringHash {
    static unsigned hash(const String& key) { return key.isNull() ? 0 : StringHash::hash(key); }
    static bool equal(const String& a, const String& b) { return a == b; }
    static constexpr bool safeToCompareToEmptyOrDeleted = false;
};
// Lookups with a null key find nothing; a null key must not be inserted.
template<typename V> using StringMap = HashMap<String, V, NullSafeStringHash>;

inline String str(const char* characters) { return characters ? String::fromUTF8(characters) : emptyString(); }

// console.debug() calls that start with this string are calls of a function
// installed by Runtime.addBinding.
static constexpr auto bindingMarker = "\x01zebkit-binding"_s;
// Isolated worlds are WebKit user worlds. Their names carry this prefix
// inside WebKit, which tells them from worlds the application made.
static constexpr auto worldPrefix = "zebkit:"_s;
static constexpr unsigned maxWorlds = 16;
// The longest side of a screenshot, as in Chrome. A page can be made as tall
// as it likes, and the image is held in memory several times over.
static constexpr int maxScreenshotSide = 16384;
// Handles DOM.describeNode keeps alive for the backendNodeIds it gave out,
// per document. The oldest are let go beyond this.
static constexpr int maxBackendNodes = 20000;
// The largest message accepted from a client.
static constexpr uint64_t maxIncomingMessageSize = 256 * 1024 * 1024;

Ref<JSON::Object> remoteObjectToCDP(JSON::Object&);
// The script context a remote object's id says the object is in; 0 if none.
int contextOfObject(const String& objectID);
// Re-encodes a base64 PNG as "jpeg" or "webp"; null if that fails.
String transcodeScreenshot(const String& base64PNG, const String& format, int quality);

inline void replyOK(Reply& reply) { reply(JSON::Object::create(), { }); }
inline void replyError(Reply& reply, const String& message) { reply(nullptr, message); }
// An error that starts with this is answered as "Invalid parameters", with
// the rest as its detail.
static constexpr auto invalidParametersPrefix = "\x1finvalid:"_s;
// Chrome answers a command whose parameters name something that is not
// there, or have a value that is not allowed, with the "invalid params"
// code and a message of the handler's own.
static constexpr auto invalidValuePrefix = "\x1fvalue:"_s;
inline void replyInvalid(Reply& reply, const String& message) { reply(nullptr, makeString(invalidValuePrefix, message)); }
String invalidParameters(const String& method, JSON::Object& params);

double monotonicSeconds();
String randomID();
void runAfter(unsigned milliseconds, Function<void()>&&);
int logLevel();
void logMessage(const String& direction, const String&);

// Whether the text is a numeric IPv4 or IPv6 address.
bool isIPAddress(const String&);
// The text's one character, or 0 if it is not exactly one.
char32_t singleCodePoint(const String&);

// What a view reports of a load of its main frame.
enum class LoadEvent : uint8_t { Started, Committed, Finished };

// A client's view of one target. The root session of a connection has an
// empty id; the others are created by Target.attachToTarget and auto-attach
// and are addressed with "sessionId" on the same socket.
class Session final : public RefCounted<Session> {
public:
    static Ref<Session> create(Connection& connection, const String& id) { return adoptRef(*new Session(connection, id)); }

    void event(ASCIILiteral method, Ref<JSON::Object>&& params);
    bool isClosed() const { return !connection; }

    String id;
    WeakPtr<Connection> connection;
    WeakPtr<PageTarget> page;

    bool runtimeEnabled { false };
    bool pageEnabled { false };
    bool networkEnabled { false };
    bool logEnabled { false };
    bool domEnabled { false };
    bool lifecycleEnabled { false };
    bool waitingForDebugger { false };
    // Was told of the dialog that is open now, so can be expected to answer it.
    bool sawDialog { false };

private:
    Session(Connection&, const String& id);
};

class Connection final : public RefCounted<Connection>, public CanMakeWeakPtr<Connection> {
public:
    static Ref<Connection> create(Ref<Socket>&& socket) { return adoptRef(*new Connection(WTF::move(socket))); }
    ~Connection();

    void send(const JSON::Object&);
    void close();
    void receive(const String& text);

    Session& rootSession() { return m_root.get(); }
    Ref<Session> createSession(PageTarget&);
    void removeSession(Session&);
    Vector<Ref<Session>> sessionsFor(PageTarget&) const;

    bool discoverTargets { false };
    bool autoAttach { false };
    bool waitForDebuggerOnStart { false };

private:
    explicit Connection(Ref<Socket>&&);

    RefPtr<Socket> m_socket;
    Ref<Session> m_root;
    StringMap<Ref<Session>> m_sessions;
};

// One web view.
class PageTarget final : public RefCounted<PageTarget>, public CanMakeWeakPtr<PageTarget>, private Inspector::FrontendChannel {
public:
    static Ref<PageTarget> create(PlatformView view) { return adoptRef(*new PageTarget(view)); }
    ~PageTarget();

    PlatformView view() const { return m_view; }
    const String& targetID() const { return m_targetID; }
    Ref<JSON::Object> targetInfo() const;

    String browserContextID;
    String openerID;
    // Set for views this endpoint created; it destroys them when they close.
    bool ownedByServer { false };
    // The address to load once the client that is setting the page up lets it run.
    String pendingInitialURL;

    void attach(Session&);
    void detach(Session&);
    void viewDestroyed();
    void dispatch(Session&, const String& method, JSON::Object& params, Reply&&);
    // A forced close does not ask the page (beforeunload) first.
    void close(bool force = false);
    void resumeIfWaiting();

    // What the platform tells of the view (CDPView.cpp). Each platform has
    // its own notifications, with its own types, and calls these from them.
    void loadChanged(LoadEvent);
    void loadFailed(ASCIILiteral errorText);
    void loadCancelled();
    void processTerminated();
    void infoChanged();
    void addressChanged();
    void finishNavigation(const String& loaderID, const String& errorText);
#if USE(GLIB)
    void loadFailed(GError*);
    bool scriptDialog(WebKitScriptDialog*);
    bool runFileChooser(GObject* request);
#elif PLATFORM(COCOA)
    // Only if acceptsDialog().
    void scriptDialog(ASCIILiteral type, const String& message, const String& defaultPrompt, Function<void(bool accept, const String& promptText)>&&);
    // False if the endpoint leaves it to the application.
    bool runFileChooser(WebKit::WebOpenPanelResultListenerProxy&);
#endif
    // A dialog is held open for a client only when one is there to answer it.
    bool acceptsDialog() const;
    PlatformPageState& native() { return m_native; }

private:
    explicit PageTarget(PlatformView);

    // Inspector::FrontendChannel
    ConnectionType connectionType() const final { return ConnectionType::Remote; }
    void sendMessageToFrontend(const String&) final;

    // CDPPage.cpp: the Web Inspector Protocol side.
    struct InnerTarget {
        String mainFrameID;
        bool ready { false };
        RefPtr<JSON::Object> frameTree;
        Vector<Ref<JSON::Object>> bufferedEvents;
    };
    struct PendingCommand {
        String innerTargetID;
        WIPReply reply;
    };
    struct QueuedCommand {
        int id;
        ASCIILiteral method;
        RefPtr<JSON::Object> params;
    };
    void connectInspector();
    void disconnectInspector();
    void reconnectInspector();
    void setPageCacheSuspended(bool);
    void dismissDialog();
    void restoreViewSize();
    void sendOuter(const String& method, Ref<JSON::Object>&& params);
    void sendToInner(const String& innerTargetID, int id, const String& method, JSON::Object* params);
    void wip(ASCIILiteral method, RefPtr<JSON::Object>&& params, WIPReply&& = nullptr);
    void setState(ASCIILiteral method, RefPtr<JSON::Object>&& params, const String& key = { });
    void initializeInnerTarget(const String& innerTargetID);
    void innerTargetBecameCurrent(const String& innerTargetID);
    void failPendingCommands(const String& message, const String& exceptInnerTargetID = String());
    String mainFrameID() const;
    void handleInnerMessage(const String& innerTargetID, const String& message);
    void handleInnerEvent(const String& method, JSON::Object& params);
    void flushBufferedEvents();
    void flushQueuedCommands();
    void resetDocumentState();

    // Translation helpers.
    using Handler = void (PageTarget::*)(Session&, JSON::Object&, Reply&&);
    static const HashMap<String, Handler>& handlers();
    String toCDPFrameID(const String&) const;
    String toInnerFrameID(const String&) const;
    String uniqueID(const String& innerID) const;
    String innerID(const String& uniqueID) const;
    Ref<JSON::Object> frameToCDP(JSON::Object& innerFrame) const;
    Ref<JSON::Object> frameTreeToCDP(JSON::Object& innerTree);
    void rememberFrame(JSON::Object& innerFrame);
    void removeFrame(const String& innerFrameID, bool announce);
    void destroyContextsOfFrame(const String& innerFrameID);
    int defaultContextID() const;
    int contextForFrame(const String& innerFrameID, const String& world = { }) const;
    bool isKnownContext(int contextID) const;
    void announceContextToAll(int contextID);
    void forgetContext(int contextID);
    void clearContexts();
    void resolveNodeInContext(const String& objectID, int contextID, Function<void(JSON::Object* remoteObject, const String& error)>&&);
    void scanNode(JSON::Object&);
    static String bindingSource(const String& name);
    void afterPageCatchesUp(Function<void()>&&);
    void ensureDocument(Function<void()>&&);
    void registerBackendNode(const String& objectID, Function<void(int)>&&);
    void callOnNode(JSON::Object& params, const String& function, ASCIILiteral failure, Function<void(RefPtr<JSON::Value>&&, const String& objectID, const String& error)>&&);
    void findFrameOwner(const String& innerFrameID, Function<void(const String& objectID)>&&);
    void frameOffset(const String& innerFrameID, Function<void(double x, double y)>&&);
    void frameOffsetOfObject(const String& objectID, Function<void(double x, double y)>&&);
    void domPassThrough(const String& method, JSON::Object& params, Reply&&);
    void waitForViewportWidth(int width, unsigned attemptsLeft, Reply&&);
    void navigatedWithinDocument(const String& url);
    bool ensureWorld(const String& name);
    void announceContext(Session&, int contextID);
    void forEachSession(NOESCAPE const Function<void(Session&)>&);
    void emitLifecycle(const String& innerFrameID, ASCIILiteral name);
    void updateBootstrapScript();
    void scheduleNetworkIdle();
    void globalObjectFor(int innerContextID, Function<void(const String& objectID, const String& error)>&&);
    void callOn(const String& objectID, const String& function, RefPtr<JSON::Array>&& arguments, bool returnByValue, WIPReply&&, bool userGesture = false);
    void resolveNode(JSON::Object& params, Function<void(const String& objectID, const String& error)>&&);
    void evaluateValue(const String& expression, Function<void(RefPtr<JSON::Value>&&)>&&);
    void finishEvaluation(JSON::Object* result, const String& error, bool returnByValue, bool awaitPromise, bool generatePreview, Reply&&);
    void handleConsoleMessage(JSON::Object& message);
    void handleRequestWillBeSent(JSON::Object& params);
    String toCDPRequestID(const String& innerRequestID) const;
    String toInnerRequestID(const String& cdpRequestID) const;
    void confirmNavigationCommitted(unsigned generation, unsigned attemptsLeft);

    // Page
    void pageEnable(Session&, JSON::Object&, Reply&&);
    void pageDisable(Session&, JSON::Object&, Reply&&);
    void pageGetFrameTree(Session&, JSON::Object&, Reply&&);
    void pageNavigate(Session&, JSON::Object&, Reply&&);
    void pageReload(Session&, JSON::Object&, Reply&&);
    void pageStopLoading(Session&, JSON::Object&, Reply&&);
    void pageSetLifecycleEventsEnabled(Session&, JSON::Object&, Reply&&);
    void pageAddScriptToEvaluateOnNewDocument(Session&, JSON::Object&, Reply&&);
    void pageRemoveScriptToEvaluateOnNewDocument(Session&, JSON::Object&, Reply&&);
    void pageCreateIsolatedWorld(Session&, JSON::Object&, Reply&&);
    void pageCaptureScreenshot(Session&, JSON::Object&, Reply&&);
    void pageGetLayoutMetrics(Session&, JSON::Object&, Reply&&);
    void pageGetNavigationHistory(Session&, JSON::Object&, Reply&&);
    void pageNavigateToHistoryEntry(Session&, JSON::Object&, Reply&&);
    void pageHandleJavaScriptDialog(Session&, JSON::Object&, Reply&&);
    void pageBringToFront(Session&, JSON::Object&, Reply&&);
    void pageClose(Session&, JSON::Object&, Reply&&);
    void pageCrash(Session&, JSON::Object&, Reply&&);
    // Runtime
    void runtimeEnable(Session&, JSON::Object&, Reply&&);
    void runtimeDisable(Session&, JSON::Object&, Reply&&);
    void runtimeEvaluate(Session&, JSON::Object&, Reply&&);
    void runtimeCallFunctionOn(Session&, JSON::Object&, Reply&&);
    void runtimeAwaitPromise(Session&, JSON::Object&, Reply&&);
    void runtimeGetProperties(Session&, JSON::Object&, Reply&&);
    void runtimeReleaseObject(Session&, JSON::Object&, Reply&&);
    void runtimeReleaseObjectGroup(Session&, JSON::Object&, Reply&&);
    void runtimeRunIfWaitingForDebugger(Session&, JSON::Object&, Reply&&);
    void runtimeAddBinding(Session&, JSON::Object&, Reply&&);
    void runtimeRemoveBinding(Session&, JSON::Object&, Reply&&);
    // DOM
    void domEnable(Session&, JSON::Object&, Reply&&);
    void domDisable(Session&, JSON::Object&, Reply&&);
    void domGetDocument(Session&, JSON::Object&, Reply&&);
    void domDescribeNode(Session&, JSON::Object&, Reply&&);
    void domResolveNode(Session&, JSON::Object&, Reply&&);
    void domRequestNode(Session&, JSON::Object&, Reply&&);
    void domGetBoxModel(Session&, JSON::Object&, Reply&&);
    void domGetContentQuads(Session&, JSON::Object&, Reply&&);
    void domScrollIntoViewIfNeeded(Session&, JSON::Object&, Reply&&);
    void domFocus(Session&, JSON::Object&, Reply&&);
    void domGetOuterHTML(Session&, JSON::Object&, Reply&&);
    void domSetFileInputFiles(Session&, JSON::Object&, Reply&&);
    void domGetFrameOwner(Session&, JSON::Object&, Reply&&);
    // Network
    void networkEnable(Session&, JSON::Object&, Reply&&);
    void networkDisable(Session&, JSON::Object&, Reply&&);
    void networkSetExtraHTTPHeaders(Session&, JSON::Object&, Reply&&);
    void networkSetCacheDisabled(Session&, JSON::Object&, Reply&&);
    void networkSetUserAgentOverride(Session&, JSON::Object&, Reply&&);
    void networkGetResponseBody(Session&, JSON::Object&, Reply&&);
    void networkGetCookies(Session&, JSON::Object&, Reply&&);
    void networkGetAllCookies(Session&, JSON::Object&, Reply&&);
    void networkSetCookie(Session&, JSON::Object&, Reply&&);
    void networkSetCookies(Session&, JSON::Object&, Reply&&);
    void networkDeleteCookies(Session&, JSON::Object&, Reply&&);
    void networkClearBrowserCookies(Session&, JSON::Object&, Reply&&);
    void securitySetIgnoreCertificateErrors(Session&, JSON::Object&, Reply&&);
    // Emulation, Log
    void emulationSetDeviceMetricsOverride(Session&, JSON::Object&, Reply&&);
    void emulationClearDeviceMetricsOverride(Session&, JSON::Object&, Reply&&);
    void emulationSetEmulatedMedia(Session&, JSON::Object&, Reply&&);
    void logEnable(Session&, JSON::Object&, Reply&&);
    void logDisable(Session&, JSON::Object&, Reply&&);
    // Input (platform)
    void inputDispatchMouseEvent(Session&, JSON::Object&, Reply&&);
    void inputDispatchKeyEvent(Session&, JSON::Object&, Reply&&);
    void inputInsertText(Session&, JSON::Object&, Reply&&);
    void whenInputProcessed(Reply&&);
    void pollInput(unsigned attemptsLeft);
    void focusView();
    void acknowledge(Session&, JSON::Object&, Reply&&);

    // Platform: what goes with the view.
    void connectViewSignals();
    void releaseView();
    void restoreCertificateChecks();
    void setCertificateErrorsIgnored(bool);
    bool hasOpenDialog() const;
    // The dialog is open (hasOpenDialog()): answer it, or just let it go.
    void answerDialog(bool accept, const String& promptText);
    void closeDialog();
    void announceDialog(ASCIILiteral type, const String& message, const String& defaultPrompt);

    PlatformView m_view;
    PlatformPageState m_native;
    String m_targetID;
    Vector<Ref<Session>> m_sessions;

    // Web Inspector Protocol state.
    bool m_inspectorConnected { false };
    unsigned m_connectionGeneration { 0 };
    unsigned m_reconnectAttempts { 0 };
    bool m_errorPageExpected { false };
    // The page's process died and nothing has been loaded since.
    bool m_crashed { false };
    int m_nextCommandID { 1 };
    String m_innerTargetID;
    String m_idPrefix;
    unsigned m_innerTargetSequence { 0 };
    StringMap<InnerTarget> m_innerTargets;
    IntMap<PendingCommand> m_pendingCommands;
    Vector<QueuedCommand> m_queuedCommands;
    struct State {
        String key;
        ASCIILiteral method;
        RefPtr<JSON::Object> params;
    };
    Vector<State> m_state;

    // Per-document state, rebuilt as the page navigates.
    struct Frame {
        String parentID;
        String loaderID;
        String url;
    };
    struct Context {
        String frameID;
        // Null for the frame's main world, else the name of an isolated one.
        String world;
        String origin;
        // WebKit makes each isolated world in every frame and document.
        // Chrome has one only where a client created it or a script was
        // registered for it, and those are the ones clients are told of.
        bool announced { false };
    };
    StringMap<Frame> m_frames;
    IntMap<Context> m_contexts;
    IntMap<String> m_globalObjects;
    IntMap<String> m_backendNodes;
    IntMap<String> m_frameOwnerNodes;
    StringMap<String> m_requestIDs;
    StringMap<String> m_documentURLs;
    // Requests that have not finished, with their resource types.
    StringMap<ASCIILiteral> m_inflightRequests;
    std::unique_ptr<IntMap<RefPtr<JSON::Array>>> m_capturedChildNodes;
    unsigned m_childNodeCaptures { 0 };
    int m_nextBackendNodeID { 0 };
    int m_nextExceptionID { 1 };
    int m_documentNodeID { 0 };
    bool m_loadFired { false };
    bool m_almostIdleFired { false };
    bool m_idleFired { false };
    unsigned m_networkIdleGeneration { 0 };

    // What clients asked for, kept across navigations.
    struct Script {
        int identifier;
        String source;
        // Null for the main world.
        String world;
    };
    struct Binding {
        String name;
        String worldName;
    };
    Vector<Script> m_scripts;
    Vector<Binding> m_bindings;
    Vector<String> m_worlds;
    int m_nextScriptID { 1 };

    // Native state.
    Reply m_pendingNavigation;
    String m_pendingErrorText;
    unsigned m_navigationGeneration { 0 };
    String m_dialogDefaultPrompt;
    Vector<String> m_pendingFiles;
    bool m_expectingFileChooser { false };
    Vector<Reply> m_inputReplies;
    bool m_inputPollActive { false };
    unsigned m_pressedButtons { 0 };
    double m_lastMouseX { 0 };
    double m_lastMouseY { 0 };
    // What a client changed outside the page, to put back when it leaves.
    int m_originalWidth { 0 };
    int m_originalHeight { 0 };

    friend class Server;
};

class Server {
public:
    static Server& singleton();

    // Returns the port, or 0 with the reason in error.
    unsigned listen(const String& address, unsigned port, String& error);
    void stop();
    String browserURL() const;
    // Without discovery the HTTP pages that list the sockets are not served:
    // only who was given browserURL() can connect.
    bool discoveryEnabled { true };
    bool hasConnections() const { return !m_connections.isEmpty(); }
    unsigned port() const { return m_port; }
    bool isListening() const { return !!m_listener; }
    void startFromEnvironmentOnce();

    void addView(PlatformView);
    void removeView(PlatformView);
    PageTarget* pageForView(PlatformView);
    PageTarget* pageForID(const String&);
#if PLATFORM(COCOA)
    bool anyPageAcceptsDialog() const;
#endif

    void addConnection(Connection&);
    void removeConnection(Connection&);

    // Target and Browser domains, valid on any session.
    bool browserCommand(Connection&, Session&, const String& method, JSON::Object& params, Reply&);
    void targetInfoChanged(PageTarget&);
    // Platform. A new view for a client or for a page that opened a
    // window (related is then the opener); null if there cannot be one.
    PlatformView createView(const String& browserContextID, PlatformView related);
    PageTarget* createPage(const String& url, const String& browserContextID);

    // Platform.
    String userAgent();
    Ref<JSON::Object> versionInfo();

#if PLATFORM(COCOA)
    // The view has to be made with the configuration it is given.
    typedef PlatformView (*CreateViewFunc)(PlatformView, WKWebViewConfiguration *, void*);
#else
    typedef PlatformView (*CreateViewFunc)(PlatformView, void*);
#endif
    CreateViewFunc createViewFunc { nullptr };
    void* createViewData { nullptr };

    // For the platform's listener. No answer means the request is the
    // start of a WebSocket, which comes to acceptSocket() once it is one.
    std::optional<HTTPResponse> handleHTTP(const HTTPRequest&);
    void acceptSocket(const HTTPRequest&, Ref<Socket>&&);
    // Null for the default context and for one that does not exist.
    PlatformNetworkSession networkSessionFor(const String& browserContextID);

private:
    void attachSession(Connection&, PageTarget&, bool waitingForDebugger);

    std::unique_ptr<Listener> m_listener;
    unsigned m_port { 0 };
    String m_host;
    bool m_checkedEnvironment { false };
    String m_browserID;
    String m_creatingContextID;
    String m_creatingOpenerID;
    Vector<Ref<PageTarget>> m_pages;
    Vector<Ref<Connection>> m_connections;
    StringMap<PlatformNetworkSession> m_browserContexts;
};

} // namespace Zebkit::CDP
