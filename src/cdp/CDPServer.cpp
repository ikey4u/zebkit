/*
 * The endpoint itself: HTTP discovery, WebSocket connections, sessions, and
 * the browser-level domains (Target, Browser, Storage) that have no
 * counterpart in WebKit's protocol and are answered from the WebKit API.
 */

#include "config.h"
#include "CDPInternal.h"

#include "WebKitWebViewPrivate.h"
#include "WebPageInspectorController.h"
#include "WebPageProxy.h"
#include "WebsiteDataStore.h"
#include "zebkit-cdp.h"
#include <WebCore/GUniquePtrSoup.h>
#include <wpe/webkit.h>
#include <wtf/CallbackAggregator.h>
#include <wtf/MonotonicTime.h>
#include <wtf/glib/GUniquePtr.h>
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
    GUniquePtr<char> uuid(g_uuid_string_random());
    return makeStringByReplacingAll(str(uuid.get()), '-', ""_s).convertToASCIIUppercase();
}

// ZEBKIT_CDP_LOG=1 prints the CDP traffic to stderr, 2 adds the Web
// Inspector Protocol traffic behind it. Long messages are cut short.
int logLevel()
{
    static int level = [] {
        const char* value = g_getenv("ZEBKIT_CDP_LOG");
        return value ? atoi(value) : 0;
    }();
    return level;
}

void logMessage(const char* direction, const String& message)
{
    auto utf8 = message.utf8();
    g_printerr("%s %.*s%s\n", direction, static_cast<int>(std::min<size_t>(utf8.length(), 600)), utf8.data(), utf8.length() > 600 ? "..." : "");
}

void runAfter(unsigned milliseconds, Function<void()>&& function)
{
    auto* holder = new Function<void()>(WTF::move(function));
    g_timeout_add_full(G_PRIORITY_DEFAULT, milliseconds, [](gpointer data) -> gboolean {
        (*static_cast<Function<void()>*>(data))();
        return G_SOURCE_REMOVE;
    }, holder, [](gpointer data) {
        delete static_cast<Function<void()>*>(data);
    });
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

Connection::Connection(SoupWebsocketConnection* socket)
    : m_socket(socket)
    , m_root(Session::create(*this, emptyString()))
{
}

Connection::~Connection() = default;

void Connection::send(const JSON::Object& message)
{
    if (!m_socket || soup_websocket_connection_get_state(m_socket.get()) != SOUP_WEBSOCKET_STATE_OPEN)
        return;
    auto text = message.toJSONString();
    if (logLevel())
        logMessage("cdp <", text);
    soup_websocket_connection_send_text(m_socket.get(), text.utf8().data());
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
    if (!m_socket)
        return;
    // The handlers carry a plain pointer to this object, and the socket can
    // outlive it while its closing handshake runs.
    g_signal_handlers_disconnect_by_data(m_socket.get(), this);
    if (soup_websocket_connection_get_state(m_socket.get()) == SOUP_WEBSOCKET_STATE_OPEN)
        soup_websocket_connection_close(m_socket.get(), SOUP_WEBSOCKET_CLOSE_NORMAL, nullptr);
    m_socket = nullptr;
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
    RefPtr value = JSON::Value::parseJSON(text);
    RefPtr message = value ? value->asObject() : nullptr;
    if (!message)
        return;

    auto id = message->getInteger("id"_s);
    auto method = message->getString("method"_s);
    auto sessionID = message->getString("sessionId"_s);
    RefPtr params = message->getObject("params"_s);
    if (!params)
        params = JSON::Object::create();
    if (!id || method.isNull())
        return;

    Reply reply = [weakThis = WeakPtr { *this }, id = *id, sessionID](RefPtr<JSON::Object>&& result, const String& error) {
        RefPtr connection = weakThis.get();
        if (!connection)
            return;
        auto response = JSON::Object::create();
        response->setInteger("id"_s, id);
        if (!error.isNull()) {
            auto errorObject = JSON::Object::create();
            bool notFound = error.endsWith("wasn't found"_s);
            errorObject->setInteger("code"_s, notFound ? -32601 : -32000);
            errorObject->setString("message"_s, error);
            response->setObject("error"_s, WTF::move(errorObject));
        } else
            response->setObject("result"_s, result ? result.releaseNonNull() : JSON::Object::create());
        if (!sessionID.isEmpty())
            response->setString("sessionId"_s, sessionID);
        connection->send(response);
    };

    RefPtr<Session> session;
    if (sessionID.isEmpty())
        session = m_root.ptr();
    else
        session = m_sessions.get(sessionID);
    if (!session) {
        replyError(reply, "Session with given id not found."_s);
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

// MARK: Cookies

void allCookies(WebKitNetworkSession* session, Function<void(CookieList&&)>&& completion)
{
    auto* manager = webkit_network_session_get_cookie_manager(session);
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

Ref<JSON::Object> cookieToCDP(SoupCookie* cookie)
{
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
    object->setBoolean("sameParty"_s, false);
    object->setString("sourceScheme"_s, soup_cookie_get_secure(cookie) ? "Secure"_s : "NonSecure"_s);
    object->setInteger("sourcePort"_s, soup_cookie_get_secure(cookie) ? 443 : 80);
    return object;
}

GUniquePtr<SoupCookie> cookieFromCDP(JSON::Object& object, const String& fallbackURL)
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

void addCookie(WebKitNetworkSession* session, SoupCookie* cookie, Function<void()>&& completion)
{
    auto* holder = new Function<void()>(WTF::move(completion));
    webkit_cookie_manager_add_cookie(webkit_network_session_get_cookie_manager(session), cookie, nullptr, [](GObject* source, GAsyncResult* result, gpointer data) {
        std::unique_ptr<Function<void()>> completion(static_cast<Function<void()>*>(data));
        webkit_cookie_manager_add_cookie_finish(WEBKIT_COOKIE_MANAGER(source), result, nullptr);
        (*completion)();
    }, holder);
}

void deleteCookie(WebKitNetworkSession* session, SoupCookie* cookie, Function<void()>&& completion)
{
    auto* holder = new Function<void()>(WTF::move(completion));
    webkit_cookie_manager_delete_cookie(webkit_network_session_get_cookie_manager(session), cookie, nullptr, [](GObject* source, GAsyncResult* result, gpointer data) {
        std::unique_ptr<Function<void()>> completion(static_cast<Function<void()>*>(data));
        webkit_cookie_manager_delete_cookie_finish(WEBKIT_COOKIE_MANAGER(source), result, nullptr);
        (*completion)();
    }, holder);
}

// MARK: Raw Web Inspector Protocol socket

// /devtools/wip/<targetId> carries WebKit's own protocol untranslated, for
// clients that speak it and for working out what the page really reports.
// It should not be open on a page at the same time as CDP sessions.
class RawChannel final : public Inspector::FrontendChannel {
    WTF_MAKE_NONCOPYABLE(RawChannel);
public:
    RawChannel(PageTarget& page, SoupWebsocketConnection* socket)
        : m_page(page)
        , m_socket(socket)
    {
        webkitWebViewGetPage(page.view()).inspectorController().connectFrontend(*this);
        g_signal_connect(socket, "message", G_CALLBACK(+[](SoupWebsocketConnection*, int type, GBytes* bytes, gpointer data) {
            auto& channel = *static_cast<RawChannel*>(data);
            RefPtr page = channel.m_page.get();
            if (type != SOUP_WEBSOCKET_DATA_TEXT || !page)
                return;
            gsize size;
            auto* text = static_cast<const char*>(g_bytes_get_data(bytes, &size));
            webkitWebViewGetPage(page->view()).inspectorController().dispatchMessageFromFrontend(String::fromUTF8(unsafeMakeSpan(text, size)));
        }), this);
        g_signal_connect(socket, "closed", G_CALLBACK(+[](SoupWebsocketConnection*, gpointer data) {
            auto* channel = static_cast<RawChannel*>(data);
            if (RefPtr page = channel->m_page.get())
                webkitWebViewGetPage(page->view()).inspectorController().disconnectFrontend(*channel);
            delete channel;
        }), this);
    }

private:
    ConnectionType connectionType() const final { return ConnectionType::Remote; }
    void sendMessageToFrontend(const String& message) final
    {
        if (soup_websocket_connection_get_state(m_socket.get()) == SOUP_WEBSOCKET_STATE_OPEN)
            soup_websocket_connection_send_text(m_socket.get(), message.utf8().data());
    }

    WeakPtr<PageTarget> m_page;
    GRefPtr<SoupWebsocketConnection> m_socket;
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
    const char* port = g_getenv("ZEBKIT_CDP_PORT");
    if (!port || !*port)
        return;
    char* end = nullptr;
    guint64 number = g_ascii_strtoull(port, &end, 10);
    if (*end || number > 65535) {
        g_printerr("zebkit: ZEBKIT_CDP_PORT=%s is not a port number; the CDP endpoint is not started\n", port);
        return;
    }
    if (const char* discovery = g_getenv("ZEBKIT_CDP_DISCOVERY"))
        discoveryEnabled = g_strcmp0(discovery, "0");
    GUniqueOutPtr<GError> error;
    if (!listen(g_getenv("ZEBKIT_CDP_ADDRESS"), number, &error.outPtr()))
        g_printerr("zebkit: cannot start the CDP endpoint: %s\n", error ? error->message : "unknown error");
}

unsigned Server::listen(const char* address, unsigned port, GError** error)
{
    m_checkedEnvironment = true;
    if (m_server)
        return m_port;

    GRefPtr<SoupServer> server = adoptGRef(soup_server_new("server-header", "zebkit", nullptr));
    GRefPtr<GInetAddress> inetAddress = adoptGRef(address && *address ? g_inet_address_new_from_string(address) : g_inet_address_new_loopback(G_SOCKET_FAMILY_IPV4));
    if (!inetAddress) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "'%s' is not a numeric IP address", address);
        return 0;
    }
    GRefPtr<GSocketAddress> socketAddress = adoptGRef(g_inet_socket_address_new(inetAddress.get(), port));
    if (!soup_server_listen(server.get(), socketAddress.get(), static_cast<SoupServerListenOptions>(0), error))
        return 0;

    GSList* uris = soup_server_get_uris(server.get());
    if (uris)
        m_port = g_uri_get_port(static_cast<GUri*>(uris->data));
    g_slist_free_full(uris, reinterpret_cast<GDestroyNotify>(g_uri_unref));

    soup_server_add_handler(server.get(), nullptr, [](SoupServer*, SoupServerMessage* message, const char* path, GHashTable*, gpointer) {
        Server::singleton().handleHTTP(message, path);
    }, nullptr, nullptr);
    soup_server_add_websocket_handler(server.get(), nullptr, nullptr, nullptr, [](SoupServer*, SoupServerMessage* message, const char* path, SoupWebsocketConnection* socket, gpointer) {
        Server::singleton().handleWebSocket(message, path, socket);
    }, nullptr, nullptr);

    m_server = WTF::move(server);
    m_browserID = randomID();
    GUniquePtr<char> host(g_inet_address_to_string(inetAddress.get()));
    m_host = g_inet_address_get_family(inetAddress.get()) == G_SOCKET_FAMILY_IPV6 ? makeString('[', str(host.get()), ']') : str(host.get());

    if (!g_inet_address_get_is_loopback(inetAddress.get()))
        g_printerr("zebkit: the CDP endpoint is listening on %s, which other machines can reach. It has no authentication: whoever connects controls the browser and can read what it can read.\n", host.get());
    // The line Chrome prints; launchers look for it to find the endpoint.
    g_printerr("\nDevTools listening on %s\n", browserURL().utf8().data());
    return m_port;
}

String Server::browserURL() const
{
    return m_server ? makeString("ws://"_s, m_host, ':', m_port, "/devtools/browser/"_s, m_browserID) : String();
}

void Server::stop()
{
    if (!m_server)
        return;
    for (auto& connection : copyToVector(m_connections))
        removeConnection(connection);
    for (auto& page : copyToVector(m_pages)) {
        if (page->ownedByServer)
            page->close(true);
    }
    m_browserContexts.clear();
    soup_server_disconnect(m_server.get());
    m_server = nullptr;
    m_port = 0;
}

// A page in a browser can make requests to this port. As Chrome does, only
// accept requests addressed to an IP address or to localhost, so that a
// hostile site cannot reach the endpoint by rebinding its own name, and
// refuse WebSocket connections that come from a web origin.
static bool isTrustedHost(SoupServerMessage* message)
{
    const char* header = soup_message_headers_get_one(soup_server_message_get_request_headers(message), "Host");
    if (!header)
        return true;
    auto host = str(header);
    if (host.startsWith('[')) {
        auto end = host.find(']');
        return end != notFound && g_hostname_is_ip_address(host.substring(1, end - 1).utf8().data());
    }
    if (auto colon = host.reverseFind(':'); colon != notFound)
        host = host.left(colon);
    return g_hostname_is_ip_address(host.utf8().data()) || equalLettersIgnoringASCIICase(host, "localhost"_s);
}

static bool isAllowedOrigin(const char* origin)
{
    if (!origin || !*origin)
        return true;
    const char* allowed = g_getenv("ZEBKIT_CDP_ALLOW_ORIGINS");
    if (!allowed)
        return false;
    if (!g_strcmp0(allowed, "*"))
        return true;
    GUniquePtr<char*> entries(g_strsplit(allowed, ",", -1));
    return g_strv_contains(entries.get(), origin);
}

static void respond(SoupServerMessage* message, unsigned status, const String& body, const char* type = "application/json; charset=UTF-8")
{
    auto utf8 = body.utf8();
    soup_server_message_set_status(message, status, nullptr);
    soup_server_message_set_response(message, type, SOUP_MEMORY_COPY, utf8.data(), utf8.length());
}

String Server::userAgent()
{
    if (!m_pages.isEmpty())
        return str(webkit_settings_get_user_agent(webkit_web_view_get_settings(m_pages.first()->view())));
    GRefPtr<WebKitSettings> settings = adoptGRef(webkit_settings_new());
    return str(webkit_settings_get_user_agent(settings.get()));
}

Ref<JSON::Object> Server::versionInfo()
{
    auto info = JSON::Object::create();
    info->setString("protocolVersion"_s, "1.3"_s);
    info->setString("product"_s, makeString("Zebkit/"_s, webkit_get_major_version(), '.', webkit_get_minor_version(), '.', webkit_get_micro_version()));
    info->setString("revision"_s, "WebKit"_s);
    info->setString("userAgent"_s, userAgent());
    info->setString("jsVersion"_s, "JavaScriptCore"_s);
    return info;
}

void Server::handleHTTP(SoupServerMessage* message, const char* rawPath)
{
    if (!isTrustedHost(message)) {
        respond(message, 403, "Host header is specified and is not an IP address or localhost."_s, "text/plain");
        return;
    }

    // WebSocket paths. libsoup runs this handler first and performs the
    // upgrade only if the request has been given no status.
    if (g_str_has_prefix(rawPath, "/devtools/"))
        return;

    // Script in a web page can send requests here, though it cannot read
    // the answers. None of them may have an effect: a request that says it
    // comes from a page is refused, and the one that creates something
    // needs a method a page cannot use without asking first.
    auto* requestHeaders = soup_server_message_get_request_headers(message);
    if (!isAllowedOrigin(soup_message_headers_get_one(requestHeaders, "Origin"))) {
        respond(message, 403, "Requests from web pages are not accepted."_s, "text/plain");
        return;
    }
    if (!discoveryEnabled) {
        respond(message, 404, "Not found"_s, "text/plain");
        return;
    }

    const char* hostHeader = soup_message_headers_get_one(soup_server_message_get_request_headers(message), "Host");
    auto host = hostHeader ? str(hostHeader) : makeString("127.0.0.1:"_s, m_port);
    auto path = str(rawPath);
    GUri* uri = soup_server_message_get_uri(message);
    auto query = str(g_uri_get_query(uri));

    auto describe = [&](PageTarget& page) {
        auto entry = JSON::Object::create();
        entry->setString("description"_s, emptyString());
        entry->setString("devtoolsFrontendUrl"_s, emptyString());
        entry->setString("id"_s, page.targetID());
        entry->setString("title"_s, str(webkit_web_view_get_title(page.view())));
        entry->setString("type"_s, "page"_s);
        entry->setString("url"_s, str(webkit_web_view_get_uri(page.view())));
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
        info->setString("WebKit-Version"_s, makeString(webkit_get_major_version(), '.', webkit_get_minor_version(), '.', webkit_get_micro_version()));
        info->setString("webSocketDebuggerUrl"_s, makeString("ws://"_s, host, "/devtools/browser/"_s, m_browserID));
        respond(message, 200, info->toJSONString());
        return;
    }
    if (path == "/json"_s || path == "/json/"_s || path == "/json/list"_s) {
        auto list = JSON::Array::create();
        for (auto& page : m_pages)
            list->pushObject(describe(page));
        respond(message, 200, list->toJSONString());
        return;
    }
    if (path == "/json/new"_s) {
        if (g_strcmp0(soup_server_message_get_method(message), "PUT")) {
            respond(message, 405, "Using unsafe HTTP verb GET to invoke /json/new. This action supports only PUT verb."_s, "text/plain");
            return;
        }
        GUniquePtr<char> url(query.isEmpty() ? nullptr : g_uri_unescape_string(query.utf8().data(), nullptr));
        RefPtr page = createPage(str(url.get()), emptyString());
        if (!page) {
            respond(message, 500, "Could not create a page."_s, "text/plain");
            return;
        }
        respond(message, 200, describe(*page)->toJSONString());
        return;
    }
    if (path.startsWith("/json/close/"_s) || path.startsWith("/json/activate/"_s)) {
        bool isClose = path.startsWith("/json/close/"_s);
        RefPtr page = pageForID(path.substring(isClose ? 12 : 15));
        if (!page) {
            respond(message, 404, "No such target id"_s, "text/plain");
            return;
        }
        if (isClose)
            page->close(true);
        respond(message, 200, isClose ? "Target is closing"_s : "Target activated"_s, "text/plain");
        return;
    }
    if (path == "/json/protocol"_s) {
        respond(message, 200, "{\"domains\":[]}"_s);
        return;
    }
    respond(message, 404, "Not found"_s, "text/plain");
}

void Server::handleWebSocket(SoupServerMessage* message, const char* rawPath, SoupWebsocketConnection* socket)
{
    if (!isTrustedHost(message) || !isAllowedOrigin(soup_websocket_connection_get_origin(socket))) {
        soup_websocket_connection_close(socket, SOUP_WEBSOCKET_CLOSE_POLICY_VIOLATION, "Rejected origin");
        return;
    }
    soup_websocket_connection_set_max_incoming_payload_size(socket, maxIncomingMessageSize);

    auto path = str(rawPath);
    RefPtr<PageTarget> page;
    if (path.startsWith("/devtools/wip/"_s)) {
        page = pageForID(path.substring(14));
        if (page)
            new RawChannel(*page, socket);
        else
            soup_websocket_connection_close(socket, SOUP_WEBSOCKET_CLOSE_POLICY_VIOLATION, "No such target id");
        return;
    }
    if (path.startsWith("/devtools/page/"_s)) {
        page = pageForID(path.substring(15));
        if (!page) {
            soup_websocket_connection_close(socket, SOUP_WEBSOCKET_CLOSE_POLICY_VIOLATION, "No such target id");
            return;
        }
    } else if (path != makeString("/devtools/browser/"_s, m_browserID)) {
        // The id is random: with discovery off it is what lets a client in.
        soup_websocket_connection_close(socket, SOUP_WEBSOCKET_CLOSE_POLICY_VIOLATION, "Unknown path");
        return;
    }

    auto connection = Connection::create(socket);
    addConnection(connection);
    if (page)
        page->attach(connection->rootSession());

    g_signal_connect(socket, "message", G_CALLBACK(+[](SoupWebsocketConnection*, int type, GBytes* bytes, gpointer data) {
        if (type != SOUP_WEBSOCKET_DATA_TEXT)
            return;
        gsize size;
        auto* text = static_cast<const char*>(g_bytes_get_data(bytes, &size));
        static_cast<Connection*>(data)->receive(String::fromUTF8(unsafeMakeSpan(text, size)));
    }), connection.ptr());
    g_signal_connect(socket, "closed", G_CALLBACK(+[](SoupWebsocketConnection*, gpointer data) {
        Server::singleton().removeConnection(*static_cast<Connection*>(data));
    }), connection.ptr());
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

PageTarget* Server::pageForView(WebKitWebView* view)
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

void Server::addView(WebKitWebView* view)
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

void Server::removeView(WebKitWebView* view)
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

WebKitNetworkSession* Server::networkSessionFor(const String& browserContextID)
{
    if (browserContextID.isEmpty())
        return nullptr;
    auto iterator = m_browserContexts.find(browserContextID);
    return iterator == m_browserContexts.end() ? nullptr : WEBKIT_NETWORK_SESSION(iterator->value.get());
}

WebKitWebView* Server::createView(const String& browserContextID, WebKitWebView* related)
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
        auto* networkSession = networkSessionFor(browserContextID);
        if (model) {
            add("web-context", webkit_web_view_get_context(model));
            add("settings", webkit_web_view_get_settings(model));
            add("user-content-manager", webkit_web_view_get_user_content_manager(model));
            add("display", webkit_web_view_get_display(model));
            if (!networkSession)
                networkSession = webkit_web_view_get_network_session(model);
        }
        add("network-session", networkSession);
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

PageTarget* Server::createPage(const String& url, const String& browserContextID)
{
    auto* view = createView(browserContextID, nullptr);
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
        webkit_web_view_load_uri(view, url.utf8().data());
    else {
        webkit_web_view_load_uri(view, "about:blank");
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
        if (auto targetID = params.getString("targetId"_s); !targetID.isEmpty())
            page = pageForID(targetID);
        auto bounds = JSON::Object::create();
        int width = defaultViewWidth;
        int height = defaultViewHeight;
        if (page) {
            if (auto* wpeView = webkit_web_view_get_wpe_view(page->view())) {
                width = wpe_view_get_width(wpeView);
                height = wpe_view_get_height(wpeView);
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
                replyError(reply, "No target with given id found"_s);
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
            replyError(reply, "No target with given id found"_s);
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
        replyError(reply, "No session with given id"_s);
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
            replyError(reply, "No target with given id found"_s);
            return true;
        }
        auto result = JSON::Object::create();
        result->setBoolean("success"_s, true);
        reply(WTF::move(result), { });
        page->close(true);
        return true;
    }
    if (method == "Target.activateTarget"_s) {
        if (RefPtr page = pageForID(params.getString("targetId"_s))) {
            if (auto* wpeView = webkit_web_view_get_wpe_view(page->view()))
                wpe_view_focus_in(wpeView);
        }
        replyOK(reply);
        return true;
    }
    if (method == "Target.createBrowserContext"_s) {
        auto id = randomID();
        m_browserContexts.set(id, adoptGRef(G_OBJECT(webkit_network_session_new_ephemeral())));
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
            replyError(reply, makeString("Failed to find browser context with id "_s, id));
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
        GRefPtr<WebKitNetworkSession> networkSession = networkSessionFor(contextID);
        if (!networkSession && sessionPage)
            networkSession = webkit_web_view_get_network_session(sessionPage->view());
        if (!networkSession && !m_pages.isEmpty())
            networkSession = webkit_web_view_get_network_session(m_pages.first()->view());
        if (!networkSession)
            networkSession = webkit_network_session_get_default();

        if (method == "Storage.getCookies"_s) {
            allCookies(networkSession.get(), [reply = WTF::move(reply)](CookieList&& cookies) mutable {
                auto array = JSON::Array::create();
                for (auto& cookie : cookies)
                    array->pushObject(cookieToCDP(cookie.get()));
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
                        addCookie(networkSession.get(), cookie.get(), [aggregator] { });
                }
            }
            return true;
        }
        allCookies(networkSession.get(), [networkSession, aggregator](CookieList&& cookies) {
            for (auto& cookie : cookies)
                deleteCookie(networkSession.get(), cookie.get(), [aggregator] { });
        });
        return true;
    }

    return false;
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

using namespace Zebkit::CDP;

extern "C" {

__attribute__((visibility("default"))) guint webkit_zebkit_cdp_listen(const char* address, guint port, GError** error)
{
    return Server::singleton().listen(address, port, error);
}

__attribute__((visibility("default"))) guint webkit_zebkit_cdp_get_port(void)
{
    return Server::singleton().port();
}

__attribute__((visibility("default"))) void webkit_zebkit_cdp_stop(void)
{
    Server::singleton().stop();
}

__attribute__((visibility("default"))) void webkit_zebkit_cdp_set_discovery_enabled(gboolean enabled)
{
    Server::singleton().discoveryEnabled = enabled;
}

__attribute__((visibility("default"))) char* webkit_zebkit_cdp_get_url(void)
{
    auto url = Server::singleton().browserURL();
    return url.isNull() ? nullptr : g_strdup(url.utf8().data());
}

__attribute__((visibility("default"))) void webkit_zebkit_cdp_set_create_view_func(ZebkitCDPCreateViewFunc function, gpointer userData)
{
    Server::singleton().createViewFunc = function;
    Server::singleton().createViewData = userData;
}

}
