/*
 * The portable part of the endpoint's server: connections and sessions, the
 * check of parameters against the protocol definition, HTTP discovery, the
 * routing of sockets, and the Target, Browser and Storage domains.
 *
 * What it needs from the platform is declared in CDPPlatform.h.
 */

#include "config.h"
#include "CDPInternal.h"

#include "WebPageInspectorController.h"
#include "WebPageProxy.h"
#include "WebsiteDataStore.h"
#include <pal/text/TextEncoding.h>
#include <arpa/inet.h>
#include <wtf/CallbackAggregator.h>
#include <wtf/MonotonicTime.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/RunLoop.h>
#include <wtf/URL.h>
#include <wtf/UUID.h>
#include <wtf/text/CString.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringToIntegerConversion.h>

namespace Zebkit::CDP {

static constexpr int defaultViewWidth = 1280;
static constexpr int defaultViewHeight = 720;

double monotonicSeconds()
{
    return MonotonicTime::now().secondsSinceEpoch().seconds();
}

// 32 upper-case hex digits, the shape of Chrome's target and session ids.
String randomID()
{
    return makeStringByReplacingAll(WTF::UUID::createVersion4().toString(), '-', ""_s).convertToASCIIUppercase();
}

// ZEBKIT_CDP_LOG=1 prints the CDP traffic to stderr, 2 adds the Web
// Inspector Protocol traffic behind it. Long messages are cut short.
int logLevel()
{
    static int level = [] {
        const char* value = std::getenv("ZEBKIT_CDP_LOG");
        return value ? atoi(value) : 0;
    }();
    return level;
}

void logMessage(const char* direction, const String& message)
{
    auto utf8 = message.utf8();
    fprintf(stderr, "%s %.*s%s\n", direction, static_cast<int>(std::min<size_t>(utf8.length(), 600)), utf8.data(), utf8.length() > 600 ? "..." : "");
}

void runAfter(unsigned milliseconds, Function<void()>&& function)
{
    RunLoop::mainSingleton().dispatchAfter(Seconds::fromMilliseconds(milliseconds), WTF::move(function));
}

bool isIPAddress(const String& text)
{
    unsigned char buffer[sizeof(struct in6_addr)];
    auto utf8 = text.utf8();
    return inet_pton(AF_INET, utf8.data(), buffer) == 1 || inet_pton(AF_INET6, utf8.data(), buffer) == 1;
}

// MARK: Session and Connection

Session::Session(Connection& connection, const String& id)
    : id(id)
    , connection(connection)
{
}

void Session::event(ASCIILiteral method, Ref<JSON::Object>&& params)
{
    RefPtr strongConnection = connection.get();
    if (!strongConnection)
        return;
    auto message = JSON::Object::create();
    message->setString("method"_s, method);
    message->setObject("params"_s, WTF::move(params));
    if (!id.isEmpty())
        message->setString("sessionId"_s, id);
    strongConnection->send(message);
}

Connection::Connection(Ref<Socket>&& socket)
    : m_socket(WTF::move(socket))
    , m_root(Session::create(*this, emptyString()))
{
}

Connection::~Connection() = default;

void Connection::send(const JSON::Object& message)
{
    if (!m_socket || !m_socket->isOpen())
        return;
    auto text = message.toJSONString();
    if (logLevel())
        logMessage("cdp <", text);
    m_socket->send(text);
}

void Connection::close()
{
    Ref protectedThis { *this };
    Vector<Ref<Session>> sessions;
    sessions.append(m_root);
    for (auto& session : m_sessions.values())
        sessions.append(session);
    m_sessions.clear();
    for (auto& session : sessions) {
        if (RefPtr page = session->page.get())
            page->detach(session);
        session->connection = nullptr;
    }
    RefPtr socket = std::exchange(m_socket, nullptr);
    if (!socket)
        return;
    // The callbacks refer to this object, and the socket can outlive it
    // while its closing handshake runs.
    socket->onMessage = nullptr;
    socket->onClosed = nullptr;
    if (socket->isOpen())
        socket->close();
}


Ref<Session> Connection::createSession(PageTarget& page)
{
    auto session = Session::create(*this, randomID());
    m_sessions.set(session->id, session.copyRef());
    page.attach(session);
    return session;
}

void Connection::removeSession(Session& session)
{
    Ref protectedSession { session };
    if (RefPtr page = session.page.get())
        page->detach(session);
    m_sessions.remove(session.id);
}

Vector<Ref<Session>> Connection::sessionsFor(PageTarget& page) const
{
    Vector<Ref<Session>> result;
    for (auto& session : m_sessions.values()) {
        if (session->page.get() == &page)
            result.append(session);
    }
    return result;
}

void Connection::receive(const String& text)
{
    Ref protectedThis { *this };
    if (logLevel())
        logMessage("cdp >", text);
    // A message that cannot be dispatched is answered the way Chrome
    // answers it, with the JSON-RPC error codes.
    auto reject = [&](std::optional<int> id, int code, ASCIILiteral text) {
        auto errorObject = JSON::Object::create();
        errorObject->setInteger("code"_s, code);
        errorObject->setString("message"_s, text);
        auto response = JSON::Object::create();
        if (id)
            response->setInteger("id"_s, *id);
        response->setObject("error"_s, WTF::move(errorObject));
        send(response);
    };
    RefPtr value = JSON::Value::parseJSON(text);
    if (!value) {
        reject({ }, -32700, "JSON: invalid message"_s);
        return;
    }
    RefPtr message = value->asObject();
    if (!message) {
        reject({ }, -32600, "Message must be an object"_s);
        return;
    }
    auto id = message->getInteger("id"_s);
    if (!id) {
        reject({ }, -32600, "Message must have integer 'id' property"_s);
        return;
    }
    auto method = message->getString("method"_s);
    if (method.isNull()) {
        reject(id, -32600, "Message must have string 'method' property"_s);
        return;
    }
    auto sessionID = message->getString("sessionId"_s);
    RefPtr<Session> session;
    if (sessionID.isEmpty())
        session = m_root.ptr();
    else
        session = m_sessions.get(sessionID);
    if (!session) {
        reject(id, -32001, "Session with given id not found."_s);
        return;
    }
    RefPtr params = message->getObject("params"_s);
    if (!params)
        params = JSON::Object::create();

    Reply reply = [weakThis = WeakPtr { *this }, id = *id, sessionID](RefPtr<JSON::Object>&& result, const String& error) {
        RefPtr connection = weakThis.get();
        if (!connection)
            return;
        auto response = JSON::Object::create();
        response->setInteger("id"_s, id);
        if (!error.isNull()) {
            auto errorObject = JSON::Object::create();
            if (error.endsWith("wasn't found"_s))
                errorObject->setInteger("code"_s, -32601);
            else if (error.startsWith(invalidParametersPrefix)) {
                errorObject->setInteger("code"_s, -32602);
                errorObject->setString("message"_s, "Invalid parameters"_s);
                errorObject->setString("data"_s, error.substring(invalidParametersPrefix.length()));
            } else if (error.startsWith(invalidValuePrefix)) {
                errorObject->setInteger("code"_s, -32602);
                errorObject->setString("message"_s, error.substring(invalidValuePrefix.length()));
            } else
                errorObject->setInteger("code"_s, -32000);
            if (!errorObject->getValue("message"_s))
                errorObject->setString("message"_s, error);
            response->setObject("error"_s, WTF::move(errorObject));
        } else
            response->setObject("result"_s, result ? result.releaseNonNull() : JSON::Object::create());
        if (!sessionID.isEmpty())
            response->setString("sessionId"_s, sessionID);
        connection->send(response);
    };

    // As in Chrome, parameters are checked against the protocol definition
    // before the command gets anywhere.
    if (auto problem = invalidParameters(method, *params); !problem.isNull()) {
        replyError(reply, makeString(invalidParametersPrefix, problem));
        return;
    }

    if (Server::singleton().browserCommand(*this, *session, method, *params, reply))
        return;
    if (RefPtr page = session->page.get()) {
        page->dispatch(*session, method, *params, WTF::move(reply));
        return;
    }
    replyError(reply, makeString('\'', method, "' wasn't found"_s));
}

// MARK: Parameters

struct CommandSignature {
    ASCIILiteral method;
    ASCIILiteral parameters;
};

// The commands that are implemented, with what the standard definition says
// about their parameters.
static constexpr CommandSignature commandSignatures[] = {
#include "CDPProtocol.inc"
};

// Null if the parameters are what the definition asks for, else what is
// wrong, in the words Chrome uses.
String invalidParameters(const String& method, JSON::Object& params)
{
    static NeverDestroyed<HashMap<String, ASCIILiteral>> table = [] {
        HashMap<String, ASCIILiteral> map;
        for (auto& signature : commandSignatures)
            map.add(String(signature.method), signature.parameters);
        return map;
    }();
    auto iterator = table->find(method);
    if (iterator == table->end())
        return { };

    for (auto parameter : StringView(iterator->value).split(' ')) {
        auto separator = parameter.find([](char16_t character) {
            return character == ':' || character == '?';
        });
        if (separator == notFound)
            continue;
        auto name = parameter.left(separator).toString();
        bool optional = parameter[separator] == '?';
        auto kind = parameter[separator + 1];
        auto fail = [&](ASCIILiteral what) {
            return makeString("Failed to deserialize params."_s, name, " - BINDINGS: "_s, what);
        };
        RefPtr value = params.getValue(name);
        if (!value || (optional && value->isNull())) {
            if (!optional)
                return fail("mandatory field missing"_s);
            continue;
        }
        auto type = value->type();
        bool isNumber = type == JSON::Value::Type::Integer || type == JSON::Value::Type::Double;
        switch (kind) {
        case 's':
            if (type != JSON::Value::Type::String)
                return fail("string value expected"_s);
            break;
        case 'b':
            if (type != JSON::Value::Type::Boolean)
                return fail("bool value expected"_s);
            break;
        case 'n':
            if (!isNumber)
                return fail("double value expected"_s);
            break;
        case 'i': {
            auto number = value->asDouble();
            if (!isNumber || !number || *number != std::trunc(*number) || std::abs(*number) > std::numeric_limits<int>::max())
                return fail("int32 value expected"_s);
            break;
        }
        case 'a':
            if (type != JSON::Value::Type::Array)
                return fail("array value expected"_s);
            break;
        case 'o':
            if (type != JSON::Value::Type::Object)
                return fail("object expected"_s);
            break;
        default:
            break;
        }
    }
    return { };
}

// MARK: Raw Web Inspector Protocol socket

// /devtools/wip/<targetId> carries WebKit's own protocol untranslated, for
// clients that speak it and for working out what the page really reports.
// It should not be open on a page at the same time as CDP sessions.
class RawChannel final : public Inspector::FrontendChannel {
    WTF_MAKE_NONCOPYABLE(RawChannel);
public:
    // Lives as long as the socket is open.
    RawChannel(PageTarget& page, Ref<Socket>&& socket)
        : m_page(page)
        , m_socket(WTF::move(socket))
    {
        pageProxy(page.view()).inspectorController().connectFrontend(*this);
        m_socket->onMessage = [this](const String& text) {
            if (RefPtr page = m_page.get(); page && page->view())
                pageProxy(page->view()).inspectorController().dispatchMessageFromFrontend(text);
        };
        m_socket->onClosed = [this] {
            if (RefPtr page = m_page.get(); page && page->view())
                pageProxy(page->view()).inspectorController().disconnectFrontend(*this);
            delete this;
        };
    }

private:
    ConnectionType connectionType() const final { return ConnectionType::Remote; }
    void sendMessageToFrontend(const String& message) final
    {
        if (m_socket->isOpen())
            m_socket->send(message);
    }

    WeakPtr<PageTarget> m_page;
    const Ref<Socket> m_socket;
};

// MARK: Server

Server& Server::singleton()
{
    static Server* server = new Server;
    return *server;
}

void Server::startFromEnvironmentOnce()
{
    if (m_checkedEnvironment)
        return;
    m_checkedEnvironment = true;
    const char* port = std::getenv("ZEBKIT_CDP_PORT");
    if (!port || !*port)
        return;
    auto number = parseInteger<uint16_t>(String::fromUTF8(port));
    if (!number) {
        fprintf(stderr, "zebkit: ZEBKIT_CDP_PORT=%s is not a port number; the CDP endpoint is not started\n", port);
        return;
    }
    if (const char* discovery = std::getenv("ZEBKIT_CDP_DISCOVERY"))
        discoveryEnabled = strcmp(discovery, "0");
    String error;
    if (!listen(str(std::getenv("ZEBKIT_CDP_ADDRESS")), *number, error))
        fprintf(stderr, "zebkit: cannot start the CDP endpoint: %s\n", error.utf8().data());
}

unsigned Server::listen(const String& address, unsigned port, String& error)
{
    m_checkedEnvironment = true;
    if (m_listener)
        return m_port;

    auto listener = Listener::create(address, port, error);
    if (!listener)
        return 0;
    m_port = listener->port;
    m_host = listener->host;
    m_browserID = randomID();
    if (!listener->isLoopback)
        fprintf(stderr, "zebkit: the CDP endpoint is listening on %s, which other machines can reach. It has no authentication: whoever connects controls the browser and can read what it can read.\n", m_host.utf8().data());
    m_listener = WTF::move(listener);
    // The line Chrome prints; launchers look for it to find the endpoint.
    fprintf(stderr, "\nDevTools listening on %s\n", browserURL().utf8().data());
    return m_port;
}

String Server::browserURL() const
{
    return m_listener ? makeString("ws://"_s, m_host, ':', m_port, "/devtools/browser/"_s, m_browserID) : String();
}

void Server::stop()
{
    if (!m_listener)
        return;
    for (auto& connection : copyToVector(m_connections))
        removeConnection(connection);
    for (auto& page : copyToVector(m_pages)) {
        if (page->ownedByServer)
            page->close(true);
    }
    m_browserContexts.clear();
    m_listener = nullptr;
    m_port = 0;
}

// A page in a browser can make requests to this port. As Chrome does, only
// accept requests addressed to an IP address or to localhost, so that a
// hostile site cannot reach the endpoint by rebinding its own name, and
// refuse WebSocket connections that come from a web origin.
static bool isTrustedHost(const String& header)
{
    if (header.isNull())
        return true;
    auto host = header;
    if (host.startsWith('[')) {
        auto end = host.find(']');
        return end != notFound && isIPAddress(host.substring(1, end - 1));
    }
    if (auto colon = host.reverseFind(':'); colon != notFound)
        host = host.left(colon);
    return isIPAddress(host) || equalLettersIgnoringASCIICase(host, "localhost"_s);
}

static bool isAllowedOrigin(const String& origin)
{
    if (origin.isEmpty())
        return true;
    const char* allowed = std::getenv("ZEBKIT_CDP_ALLOW_ORIGINS");
    if (!allowed)
        return false;
    if (!strcmp(allowed, "*"))
        return true;
    return String::fromUTF8(allowed).split(',').contains(origin);
}

Ref<JSON::Object> Server::versionInfo()
{
    auto info = JSON::Object::create();
    info->setString("protocolVersion"_s, "1.3"_s);
    info->setString("product"_s, makeString("Zebkit/"_s, engineVersion()));
    info->setString("revision"_s, "WebKit"_s);
    info->setString("userAgent"_s, userAgent());
    info->setString("jsVersion"_s, "JavaScriptCore"_s);
    return info;
}

std::optional<HTTPResponse> Server::handleHTTP(const HTTPRequest& request)
{
    auto text = [](unsigned status, ASCIILiteral body) {
        return HTTPResponse { status, body, "text/plain"_s };
    };
    auto json = [](const String& body) {
        return HTTPResponse { 200, body, "application/json; charset=UTF-8"_s };
    };

    if (!isTrustedHost(request.host))
        return text(403, "Host header is specified and is not an IP address or localhost."_s);

    // WebSocket paths: the platform performs the upgrade.
    if (request.path.startsWith("/devtools/"_s))
        return std::nullopt;

    // Script in a web page can send requests here, though it cannot read
    // the answers. None of them may have an effect: a request that says it
    // comes from a page is refused, and the one that creates something
    // needs a method a page cannot use without asking first.
    if (!isAllowedOrigin(request.origin))
        return text(403, "Requests from web pages are not accepted."_s);
    if (!discoveryEnabled)
        return text(404, "Not found"_s);

    auto host = request.host.isNull() ? makeString("127.0.0.1:"_s, m_port) : request.host;
    auto& path = request.path;

    auto describe = [&](PageTarget& page) {
        auto entry = JSON::Object::create();
        entry->setString("description"_s, emptyString());
        entry->setString("devtoolsFrontendUrl"_s, emptyString());
        entry->setString("id"_s, page.targetID());
        entry->setString("title"_s, viewTitle(page.view()));
        entry->setString("type"_s, "page"_s);
        entry->setString("url"_s, viewURI(page.view()));
        entry->setString("webSocketDebuggerUrl"_s, makeString("ws://"_s, host, "/devtools/page/"_s, page.targetID()));
        return entry;
    };

    if (path == "/json/version"_s || path == "/json/version/"_s) {
        auto info = JSON::Object::create();
        auto version = versionInfo();
        info->setString("Browser"_s, version->getString("product"_s));
        info->setString("Protocol-Version"_s, "1.3"_s);
        info->setString("User-Agent"_s, version->getString("userAgent"_s));
        info->setString("V8-Version"_s, emptyString());
        info->setString("WebKit-Version"_s, engineVersion());
        info->setString("webSocketDebuggerUrl"_s, makeString("ws://"_s, host, "/devtools/browser/"_s, m_browserID));
        return json(info->toJSONString());
    }
    if (path == "/json"_s || path == "/json/"_s || path == "/json/list"_s) {
        auto list = JSON::Array::create();
        for (auto& page : m_pages)
            list->pushObject(describe(page));
        return json(list->toJSONString());
    }
    if (path == "/json/new"_s) {
        if (request.method != "PUT"_s)
            return text(405, "Using unsafe HTTP verb GET to invoke /json/new. This action supports only PUT verb."_s);
        RefPtr page = createPage(request.query.isEmpty() ? String() : PAL::decodeURLEscapeSequences(request.query), emptyString());
        if (!page)
            return text(500, "Could not create a page."_s);
        return json(describe(*page)->toJSONString());
    }
    if (path.startsWith("/json/close/"_s) || path.startsWith("/json/activate/"_s)) {
        bool isClose = path.startsWith("/json/close/"_s);
        RefPtr page = pageForID(path.substring(isClose ? 12 : 15));
        if (!page)
            return text(404, "No such target id"_s);
        if (isClose)
            page->close(true);
        return text(200, isClose ? "Target is closing"_s : "Target activated"_s);
    }
    if (path == "/json/protocol"_s)
        return json("{\"domains\":[]}"_s);
    return text(404, "Not found"_s);
}

void Server::acceptSocket(const HTTPRequest& request, Ref<Socket>&& socket)
{
    if (!isTrustedHost(request.host) || !isAllowedOrigin(request.origin)) {
        socket->close("Rejected origin"_s);
        return;
    }

    auto& path = request.path;
    RefPtr<PageTarget> page;
    if (path.startsWith("/devtools/wip/"_s)) {
        page = pageForID(path.substring(14));
        if (page)
            new RawChannel(*page, WTF::move(socket));
        else
            socket->close("No such target id"_s);
        return;
    }
    if (path.startsWith("/devtools/page/"_s)) {
        page = pageForID(path.substring(15));
        if (!page) {
            socket->close("No such target id"_s);
            return;
        }
    } else if (path != makeString("/devtools/browser/"_s, m_browserID)) {
        // The id is random: with discovery off it is what lets a client in.
        socket->close("Unknown path"_s);
        return;
    }

    auto connection = Connection::create(socket.copyRef());
    addConnection(connection);
    if (page)
        page->attach(connection->rootSession());

    // The server's list keeps the connection alive until the socket closes.
    socket->onMessage = [connection = connection.ptr()](const String& text) {
        connection->receive(text);
    };
    socket->onClosed = [connection = connection.ptr()] {
        Server::singleton().removeConnection(*connection);
    };
}


void Server::addConnection(Connection& connection)
{
    m_connections.append(connection);
}

void Server::removeConnection(Connection& connection)
{
    Ref protectedConnection { connection };
    m_connections.removeFirstMatching([&](auto& item) {
        return item.ptr() == &connection;
    });
    connection.close();
}

PageTarget* Server::pageForView(PlatformView view)
{
    for (auto& page : m_pages) {
        if (page->view() == view)
            return page.ptr();
    }
    return nullptr;
}

PageTarget* Server::pageForID(const String& id)
{
    for (auto& page : m_pages) {
        if (page->targetID() == id)
            return page.ptr();
    }
    return nullptr;
}

void Server::attachSession(Connection& connection, PageTarget& page, bool waitingForDebugger)
{
    auto session = connection.createSession(page);
    session->waitingForDebugger = waitingForDebugger;
    auto params = JSON::Object::create();
    params->setString("sessionId"_s, session->id);
    params->setObject("targetInfo"_s, page.targetInfo());
    params->setBoolean("waitingForDebugger"_s, waitingForDebugger);
    connection.rootSession().event("Target.attachedToTarget"_s, WTF::move(params));
}

void Server::addView(PlatformView view)
{
    if (pageForView(view))
        return;
    auto page = PageTarget::create(view);
    page->browserContextID = m_creatingContextID;
    page->openerID = m_creatingOpenerID;
    m_pages.append(page.copyRef());

    for (auto& connection : copyToVector(m_connections)) {
        if (connection->discoverTargets) {
            auto params = JSON::Object::create();
            params->setObject("targetInfo"_s, page->targetInfo());
            connection->rootSession().event("Target.targetCreated"_s, WTF::move(params));
        }
        if (connection->autoAttach && !connection->rootSession().page)
            attachSession(connection, page, connection->waitForDebuggerOnStart);
    }
}

void Server::removeView(PlatformView view)
{
    RefPtr page = pageForView(view);
    if (!page)
        return;
    m_pages.removeFirstMatching([&](auto& item) {
        return item.ptr() == page.get();
    });

    for (auto& connection : copyToVector(m_connections)) {
        for (auto& session : connection->sessionsFor(*page)) {
            auto params = JSON::Object::create();
            params->setString("sessionId"_s, session->id);
            params->setString("targetId"_s, page->targetID());
            connection->rootSession().event("Target.detachedFromTarget"_s, WTF::move(params));
            connection->removeSession(session);
        }
        if (connection->discoverTargets) {
            auto params = JSON::Object::create();
            params->setString("targetId"_s, page->targetID());
            connection->rootSession().event("Target.targetDestroyed"_s, WTF::move(params));
        }
    }
    page->viewDestroyed();

    // A socket opened on the page itself has nothing left to talk to.
    for (auto& connection : copyToVector(m_connections)) {
        if (connection->rootSession().page.get() == page.get())
            removeConnection(connection);
    }
}

void Server::targetInfoChanged(PageTarget& page)
{
    for (auto& connection : copyToVector(m_connections)) {
        if (!connection->discoverTargets)
            continue;
        auto params = JSON::Object::create();
        params->setObject("targetInfo"_s, page.targetInfo());
        connection->rootSession().event("Target.targetInfoChanged"_s, WTF::move(params));
    }
}

PlatformNetworkSession Server::networkSessionFor(const String& browserContextID)
{
    if (browserContextID.isEmpty())
        return nullptr;
    auto iterator = m_browserContexts.find(browserContextID);
    return iterator == m_browserContexts.end() ? nullptr : iterator->value;
}


PageTarget* Server::createPage(const String& url, const String& browserContextID)
{
    auto view = createView(browserContextID, nullptr);
    auto* page = view ? pageForView(view) : nullptr;
    if (!page)
        return nullptr;

    // The page needs a document before a client can set it up. The address
    // asked for is loaded once every client that wants to prepare the page
    // first has said it is done.
    bool hasURL = !url.isEmpty() && url != "about:blank"_s;
    bool mustWait = false;
    for (auto& connection : m_connections) {
        for (auto& session : connection->sessionsFor(*page))
            mustWait |= session->waitingForDebugger;
    }
    if (hasURL && !mustWait)
        loadURI(view, url);
    else {
        loadURI(view, "about:blank"_s);
        if (hasURL)
            page->pendingInitialURL = url;
    }
    return page;
}

bool Server::browserCommand(Connection& connection, Session& session, const String& method, JSON::Object& params, Reply& reply)
{
    RefPtr sessionPage = session.page.get();

    if (method == "Browser.getVersion"_s) {
        reply(versionInfo(), { });
        return true;
    }
    if (method == "Browser.close"_s) {
        replyOK(reply);
        for (auto& page : copyToVector(m_pages)) {
            if (page->ownedByServer)
                page->close(true);
        }
        return true;
    }
    if (method == "Browser.getWindowForTarget"_s || method == "Browser.getWindowBounds"_s) {
        RefPtr page = sessionPage;
        if (auto targetID = params.getString("targetId"_s); !targetID.isEmpty()) {
            page = pageForID(targetID);
            if (!page) {
                replyError(reply, "No target with given id"_s);
                return true;
            }
        }
        auto bounds = JSON::Object::create();
        int width = defaultViewWidth;
        int height = defaultViewHeight;
        if (page) {
            if (auto size = viewSize(page->view()); size.first) {
                width = size.first;
                height = size.second;
            }
        }
        bounds->setInteger("left"_s, 0);
        bounds->setInteger("top"_s, 0);
        bounds->setInteger("width"_s, width);
        bounds->setInteger("height"_s, height);
        bounds->setString("windowState"_s, "normal"_s);
        auto result = JSON::Object::create();
        result->setInteger("windowId"_s, 1);
        result->setObject("bounds"_s, WTF::move(bounds));
        reply(WTF::move(result), { });
        return true;
    }
    if (method == "Browser.setWindowBounds"_s || method == "Browser.setDownloadBehavior"_s
        || method == "Browser.grantPermissions"_s || method == "Browser.resetPermissions"_s
        || method == "Target.setRemoteLocations"_s) {
        replyOK(reply);
        return true;
    }

    if (method == "Target.setDiscoverTargets"_s) {
        connection.discoverTargets = params.getBoolean("discover"_s).value_or(false);
        if (connection.discoverTargets) {
            for (auto& page : m_pages) {
                auto event = JSON::Object::create();
                event->setObject("targetInfo"_s, page->targetInfo());
                connection.rootSession().event("Target.targetCreated"_s, WTF::move(event));
            }
        }
        replyOK(reply);
        return true;
    }
    if (method == "Target.getTargets"_s) {
        auto infos = JSON::Array::create();
        for (auto& page : m_pages)
            infos->pushObject(page->targetInfo());
        auto result = JSON::Object::create();
        result->setArray("targetInfos"_s, WTF::move(infos));
        reply(WTF::move(result), { });
        return true;
    }
    if (method == "Target.getTargetInfo"_s) {
        RefPtr page = sessionPage;
        if (auto targetID = params.getString("targetId"_s); !targetID.isEmpty()) {
            page = pageForID(targetID);
            if (!page && targetID != m_browserID) {
                replyInvalid(reply, "No target with given id found"_s);
                return true;
            }
        }
        auto result = JSON::Object::create();
        if (page)
            result->setObject("targetInfo"_s, page->targetInfo());
        else {
            auto info = JSON::Object::create();
            info->setString("targetId"_s, m_browserID);
            info->setString("type"_s, "browser"_s);
            info->setString("title"_s, emptyString());
            info->setString("url"_s, emptyString());
            info->setBoolean("attached"_s, true);
            info->setBoolean("canAccessOpener"_s, false);
            result->setObject("targetInfo"_s, WTF::move(info));
        }
        reply(WTF::move(result), { });
        return true;
    }
    if (method == "Target.setAutoAttach"_s) {
        // Pages have no child targets here: frames and workers are reached
        // through the page. Only the browser-level request does anything.
        if (!sessionPage) {
            connection.autoAttach = params.getBoolean("autoAttach"_s).value_or(false);
            connection.waitForDebuggerOnStart = params.getBoolean("waitForDebuggerOnStart"_s).value_or(false);
            if (connection.autoAttach) {
                for (auto& page : copyToVector(m_pages)) {
                    if (connection.sessionsFor(page).isEmpty())
                        attachSession(connection, page, false);
                }
            }
        }
        replyOK(reply);
        return true;
    }
    if (method == "Target.attachToTarget"_s) {
        RefPtr page = pageForID(params.getString("targetId"_s));
        if (!page) {
            replyInvalid(reply, "No target with given id found"_s);
            return true;
        }
        auto newSession = connection.createSession(*page);
        auto event = JSON::Object::create();
        event->setString("sessionId"_s, newSession->id);
        event->setObject("targetInfo"_s, page->targetInfo());
        event->setBoolean("waitingForDebugger"_s, false);
        session.event("Target.attachedToTarget"_s, WTF::move(event));
        auto result = JSON::Object::create();
        result->setString("sessionId"_s, newSession->id);
        reply(WTF::move(result), { });
        return true;
    }
    if (method == "Target.detachFromTarget"_s) {
        auto sessionID = params.getString("sessionId"_s);
        for (auto& page : copyToVector(m_pages)) {
            for (auto& candidate : connection.sessionsFor(page)) {
                if (candidate->id != sessionID)
                    continue;
                auto event = JSON::Object::create();
                event->setString("sessionId"_s, sessionID);
                event->setString("targetId"_s, page->targetID());
                connection.removeSession(candidate);
                connection.rootSession().event("Target.detachedFromTarget"_s, WTF::move(event));
                replyOK(reply);
                return true;
            }
        }
        replyInvalid(reply, "No session with given id"_s);
        return true;
    }
    if (method == "Target.createTarget"_s) {
        auto contextID = params.getString("browserContextId"_s);
        if (!contextID.isEmpty() && !m_browserContexts.contains(contextID)) {
            replyError(reply, makeString("Failed to find browser context with id "_s, contextID));
            return true;
        }
        RefPtr page = createPage(params.getString("url"_s), contextID);
        if (!page) {
            replyError(reply, "Could not create a page"_s);
            return true;
        }
        auto result = JSON::Object::create();
        result->setString("targetId"_s, page->targetID());
        reply(WTF::move(result), { });
        return true;
    }
    if (method == "Target.closeTarget"_s) {
        RefPtr page = pageForID(params.getString("targetId"_s));
        if (!page) {
            replyInvalid(reply, "No target with given id found"_s);
            return true;
        }
        auto result = JSON::Object::create();
        result->setBoolean("success"_s, true);
        reply(WTF::move(result), { });
        page->close(true);
        return true;
    }
    if (method == "Target.activateTarget"_s) {
        RefPtr page = pageForID(params.getString("targetId"_s));
        if (!page) {
            replyInvalid(reply, "No target with given id found"_s);
            return true;
        }
        page->focusView();
        replyOK(reply);
        return true;
    }
    if (method == "Target.createBrowserContext"_s) {
        auto id = randomID();
        m_browserContexts.set(id, createEphemeralNetworkSession());
        auto result = JSON::Object::create();
        result->setString("browserContextId"_s, id);
        reply(WTF::move(result), { });
        return true;
    }
    if (method == "Target.getBrowserContexts"_s) {
        auto ids = JSON::Array::create();
        for (auto& id : m_browserContexts.keys())
            ids->pushString(id);
        auto result = JSON::Object::create();
        result->setArray("browserContextIds"_s, WTF::move(ids));
        reply(WTF::move(result), { });
        return true;
    }
    if (method == "Target.disposeBrowserContext"_s) {
        auto id = params.getString("browserContextId"_s);
        if (!m_browserContexts.contains(id)) {
            replyError(reply, makeString("Failed to find context with id "_s, id));
            return true;
        }
        for (auto& page : copyToVector(m_pages)) {
            if (page->browserContextID == id)
                page->close(true);
        }
        m_browserContexts.remove(id);
        replyOK(reply);
        return true;
    }

    // Cookies of a whole browser context. On a page session this is the
    // context the page belongs to.
    if (method == "Storage.getCookies"_s || method == "Storage.setCookies"_s || method == "Storage.clearCookies"_s) {
        auto contextID = params.getString("browserContextId"_s);
        if (contextID.isEmpty() && sessionPage)
            contextID = sessionPage->browserContextID;
        auto networkSession = networkSessionFor(contextID);
        if (!networkSession && sessionPage)
            networkSession = networkSessionOf(sessionPage->view());
        if (!networkSession && !m_pages.isEmpty())
            networkSession = networkSessionOf(m_pages.first()->view());
        if (!networkSession)
            networkSession = defaultNetworkSession();

        if (method == "Storage.getCookies"_s) {
            allCookies(networkSession, [reply = WTF::move(reply)](CookieList&& cookies) mutable {
                auto array = JSON::Array::create();
                for (auto& cookie : cookies)
                    array->pushObject(cookieToCDP(cookie));
                auto result = JSON::Object::create();
                result->setArray("cookies"_s, WTF::move(array));
                reply(WTF::move(result), { });
            });
            return true;
        }
        auto sharedReply = Box<Reply>::create(WTF::move(reply));
        auto aggregator = CallbackAggregator::create([sharedReply] {
            replyOK(*sharedReply);
        });
        if (method == "Storage.setCookies"_s) {
            if (RefPtr cookies = params.getArray("cookies"_s)) {
                for (auto& value : *cookies) {
                    RefPtr object = value->asObject();
                    auto cookie = object ? cookieFromCDP(*object, { }) : nullptr;
                    if (cookie)
                        addCookie(networkSession, cookie, [aggregator] { });
                }
            }
            return true;
        }
        allCookies(networkSession, [networkSession, aggregator](CookieList&& cookies) {
            for (auto& cookie : cookies)
                deleteCookie(networkSession, cookie, [aggregator] { });
        });
        return true;
    }

    return false;
}

} // namespace Zebkit::CDP
