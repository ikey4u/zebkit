/*
 * The endpoint's network side on GLib platforms: the listening socket, the
 * HTTP requests and the WebSockets, all libsoup's, and the public C API in
 * zebkit-cdp.h. What a request means is decided in CDPServer.cpp.
 */

#include "config.h"
#include "CDPInternal.h"

#include "zebkit-cdp.h"
#include <wtf/text/CString.h>
#include <wtf/text/MakeString.h>

namespace Zebkit::CDP {

static HTTPRequest requestFrom(SoupServerMessage* message, const char* path)
{
    auto header = [&](const char* name) {
        const char* value = soup_message_headers_get_one(soup_server_message_get_request_headers(message), name);
        return value ? String::fromUTF8(value) : String();
    };
    HTTPRequest request;
    request.method = str(soup_server_message_get_method(message));
    request.path = str(path);
    request.query = str(g_uri_get_query(soup_server_message_get_uri(message)));
    request.host = header("Host");
    request.origin = header("Origin");
    return request;
}

class SoupSocket final : public Socket {
public:
    static Ref<SoupSocket> create(SoupWebsocketConnection* socket) { return adoptRef(*new SoupSocket(socket)); }

    ~SoupSocket()
    {
        g_signal_handlers_disconnect_by_data(m_socket.get(), this);
    }

private:
    explicit SoupSocket(SoupWebsocketConnection* socket)
        : m_socket(socket)
    {
        soup_websocket_connection_set_max_incoming_payload_size(socket, maxIncomingMessageSize);
        g_signal_connect(socket, "message", G_CALLBACK(+[](SoupWebsocketConnection*, int type, GBytes* bytes, gpointer data) {
            Ref self { *static_cast<SoupSocket*>(data) };
            if (type != SOUP_WEBSOCKET_DATA_TEXT || !self->onMessage)
                return;
            gsize size;
            auto* text = static_cast<const char*>(g_bytes_get_data(bytes, &size));
            self->onMessage(String::fromUTF8(unsafeMakeSpan(text, size)));
        }), this);
        g_signal_connect(socket, "closed", G_CALLBACK(+[](SoupWebsocketConnection*, gpointer data) {
            Ref self { *static_cast<SoupSocket*>(data) };
            // Whoever is told may drop the last reference it knows of.
            if (auto closed = std::exchange(self->onClosed, nullptr))
                closed();
            self->deref(); // The reference the open socket held.
        }), this);
        // The socket is alive for as long as it is open, whoever holds it.
        ref();
    }

    bool isOpen() const final
    {
        return soup_websocket_connection_get_state(m_socket.get()) == SOUP_WEBSOCKET_STATE_OPEN;
    }

    void send(const String& text) final
    {
        if (isOpen())
            soup_websocket_connection_send_text(m_socket.get(), text.utf8().data());
    }

    void close(ASCIILiteral refusal) final
    {
        if (!isOpen())
            return;
        if (refusal.isNull())
            soup_websocket_connection_close(m_socket.get(), SOUP_WEBSOCKET_CLOSE_NORMAL, nullptr);
        else
            soup_websocket_connection_close(m_socket.get(), SOUP_WEBSOCKET_CLOSE_POLICY_VIOLATION, refusal.characters());
    }

    GRefPtr<SoupWebsocketConnection> m_socket;
};

class SoupListener final : public Listener {
public:
    explicit SoupListener(GRefPtr<SoupServer>&& server)
        : m_server(WTF::move(server))
    {
    }

    ~SoupListener()
    {
        soup_server_disconnect(m_server.get());
    }

private:
    GRefPtr<SoupServer> m_server;
};

std::unique_ptr<Listener> Listener::create(const String& address, unsigned port, String& error)
{
    GRefPtr<SoupServer> server = adoptGRef(soup_server_new("server-header", "zebkit", nullptr));
    GRefPtr<GInetAddress> inetAddress = adoptGRef(address.isEmpty() ? g_inet_address_new_loopback(G_SOCKET_FAMILY_IPV4) : g_inet_address_new_from_string(address.utf8().data()));
    if (!inetAddress) {
        error = makeString('\'', address, "' is not a numeric IP address"_s);
        return nullptr;
    }
    GRefPtr<GSocketAddress> socketAddress = adoptGRef(g_inet_socket_address_new(inetAddress.get(), port));
    GUniqueOutPtr<GError> listenError;
    if (!soup_server_listen(server.get(), socketAddress.get(), static_cast<SoupServerListenOptions>(0), &listenError.outPtr())) {
        error = listenError ? str(listenError->message) : "cannot listen"_str;
        return nullptr;
    }

    // libsoup runs the plain handler first, also for a request that asks
    // for a WebSocket, and upgrades only if that gave the request no status.
    soup_server_add_handler(server.get(), nullptr, [](SoupServer*, SoupServerMessage* message, const char* path, GHashTable*, gpointer) {
        auto response = Server::singleton().handleHTTP(requestFrom(message, path));
        if (!response)
            return;
        auto body = response->body.utf8();
        soup_server_message_set_status(message, response->status, nullptr);
        soup_server_message_set_response(message, response->type.characters(), SOUP_MEMORY_COPY, body.data(), body.length());
    }, nullptr, nullptr);
    soup_server_add_websocket_handler(server.get(), nullptr, nullptr, nullptr, [](SoupServer*, SoupServerMessage* message, const char* path, SoupWebsocketConnection* socket, gpointer) {
        auto request = requestFrom(message, path);
        // For a WebSocket, libsoup has the origin.
        request.origin = str(soup_websocket_connection_get_origin(socket));
        Server::singleton().acceptSocket(request, SoupSocket::create(socket));
    }, nullptr, nullptr);

    auto listener = makeUniqueWithoutFastMallocCheck<SoupListener>(GRefPtr<SoupServer> { server });
    GSList* uris = soup_server_get_uris(server.get());
    if (uris)
        listener->port = g_uri_get_port(static_cast<GUri*>(uris->data));
    g_slist_free_full(uris, reinterpret_cast<GDestroyNotify>(g_uri_unref));
    GUniquePtr<char> host(g_inet_address_to_string(inetAddress.get()));
    listener->host = g_inet_address_get_family(inetAddress.get()) == G_SOCKET_FAMILY_IPV6 ? makeString('[', str(host.get()), ']') : str(host.get());
    listener->isLoopback = g_inet_address_get_is_loopback(inetAddress.get());
    return listener;
}

} // namespace Zebkit::CDP

using namespace Zebkit::CDP;

extern "C" {

__attribute__((visibility("default"))) guint zebkit_cdp_listen(const char* address, guint port, GError** error)
{
    String reason;
    unsigned listening = Server::singleton().listen(str(address), port, reason);
    if (!listening)
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, reason.utf8().data());
    return listening;
}

__attribute__((visibility("default"))) guint zebkit_cdp_get_port(void)
{
    return Server::singleton().port();
}

__attribute__((visibility("default"))) void zebkit_cdp_stop(void)
{
    Server::singleton().stop();
}

__attribute__((visibility("default"))) void zebkit_cdp_set_discovery_enabled(gboolean enabled)
{
    Server::singleton().discoveryEnabled = enabled;
}

__attribute__((visibility("default"))) char* zebkit_cdp_get_url(void)
{
    auto url = Server::singleton().browserURL();
    return url.isNull() ? nullptr : g_strdup(url.utf8().data());
}

__attribute__((visibility("default"))) void zebkit_cdp_set_create_view_func(ZebkitCDPCreateViewFunc function, gpointer userData)
{
    Server::singleton().createViewFunc = function;
    Server::singleton().createViewData = userData;
}

}
