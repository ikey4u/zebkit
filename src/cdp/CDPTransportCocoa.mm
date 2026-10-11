/*
 * The endpoint's network side on macOS: a listening socket and one
 * ByteStream per client, on the main queue, and the public C API in
 * zebkit-cdp.h. HTTP and WebSockets are CDPWebSocket.cpp's; what a request
 * means is decided in CDPServer.cpp.
 *
 * Plain BSD sockets, watched by dispatch sources. Network.framework would
 * do as well for the bytes, but it learns the port a listener got only
 * some time after it was asked, and zebkit_cdp_listen() returns it.
 */

#include "config.h"
#include "CDPInternal.h"

#include "CDPWebSocket.h"
#include "zebkit-cdp.h"
#include <arpa/inet.h>
#include <dispatch/dispatch.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#include <wtf/BlockPtr.h>
#include <wtf/OSObjectPtr.h>
#include <wtf/text/CString.h>
#include <wtf/text/MakeString.h>

namespace Zebkit::CDP {

static void makeNonBlocking(int descriptor)
{
    fcntl(descriptor, F_SETFL, fcntl(descriptor, F_GETFL) | O_NONBLOCK);
    fcntl(descriptor, F_SETFD, FD_CLOEXEC);
    int on = 1;
    // A client that goes away must not kill the process that writes to it.
    setsockopt(descriptor, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
}

class SocketStream final : public ByteStream {
public:
    static Ref<SocketStream> create(int descriptor) { return adoptRef(*new SocketStream(descriptor)); }

    ~SocketStream()
    {
        ASSERT(m_descriptor < 0);
    }

private:
    explicit SocketStream(int descriptor)
        : m_descriptor(descriptor)
    {
        makeNonBlocking(descriptor);
        int on = 1;
        setsockopt(descriptor, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));

        // The sources keep the stream alive until it is closed.
        ref();
        m_readSource = adoptOSObject(dispatch_source_create(DISPATCH_SOURCE_TYPE_READ, descriptor, 0, dispatch_get_main_queue()));
        dispatch_source_set_event_handler(m_readSource.get(), ^{
            Ref protectedThis { *this };
            readAvailable();
        });
        // The descriptor is closed once nothing watches it any more.
        dispatch_source_set_cancel_handler(m_readSource.get(), ^{
            ::close(descriptor);
        });
        dispatch_resume(m_readSource.get());

        m_writeSource = adoptOSObject(dispatch_source_create(DISPATCH_SOURCE_TYPE_WRITE, descriptor, 0, dispatch_get_main_queue()));
        dispatch_source_set_event_handler(m_writeSource.get(), ^{
            Ref protectedThis { *this };
            flush();
        });
    }

    void readAvailable()
    {
        std::array<uint8_t, 64 * 1024> buffer;
        while (m_descriptor >= 0 && !m_closing) {
            ssize_t count = ::read(m_descriptor, buffer.data(), buffer.size());
            if (count > 0) {
                if (onData)
                    onData(std::span<const uint8_t> { buffer }.first(static_cast<size_t>(count)));
                continue;
            }
            if (count < 0 && (errno == EAGAIN || errno == EINTR))
                return;
            // The other side closed, or the connection broke.
            shutDown();
            return;
        }
    }

    void write(std::span<const uint8_t> data) final
    {
        if (m_descriptor < 0 || m_closing)
            return;
        m_pending.append(data);
        flush();
    }

    void flush()
    {
        while (m_descriptor >= 0 && !m_pending.isEmpty()) {
            ssize_t count = ::write(m_descriptor, m_pending.span().data(), m_pending.size());
            if (count > 0) {
                m_pending.removeAt(0, count);
                continue;
            }
            if (count < 0 && errno == EINTR)
                continue;
            if (count < 0 && errno == EAGAIN) {
                setWaitingToWrite(true);
                return;
            }
            m_pending.clear();
            shutDown();
            return;
        }
        setWaitingToWrite(false);
        if (m_closing)
            shutDown();
    }

    void setWaitingToWrite(bool waiting)
    {
        if (waiting == m_waitingToWrite)
            return;
        m_waitingToWrite = waiting;
        if (waiting)
            dispatch_resume(m_writeSource.get());
        else
            dispatch_suspend(m_writeSource.get());
    }

    void close() final
    {
        if (m_descriptor < 0 || std::exchange(m_closing, true))
            return;
        // Not from inside the caller, and after what is still to be sent.
        dispatch_async(dispatch_get_main_queue(), makeBlockPtr([protectedThis = Ref { *this }] {
            protectedThis->flush();
        }).get());
    }

    void shutDown()
    {
        if (m_descriptor < 0)
            return;
        // A suspended source cannot be cancelled.
        setWaitingToWrite(true);
        dispatch_source_cancel(m_writeSource.get());
        dispatch_source_cancel(m_readSource.get());
        // Sent now, though the descriptor lives a moment longer.
        ::shutdown(std::exchange(m_descriptor, -1), SHUT_RDWR);
        onData = nullptr;
        if (auto closed = std::exchange(onClosed, nullptr))
            closed();
        deref(); // The reference the open connection held.
    }

    int m_descriptor;
    bool m_closing { false };
    bool m_waitingToWrite { false };
    Vector<uint8_t> m_pending;
    OSObjectPtr<dispatch_source_t> m_readSource;
    OSObjectPtr<dispatch_source_t> m_writeSource;
};

class SocketListener final : public Listener {
public:
    explicit SocketListener(int descriptor)
    {
        m_source = adoptOSObject(dispatch_source_create(DISPATCH_SOURCE_TYPE_READ, descriptor, 0, dispatch_get_main_queue()));
        dispatch_source_set_event_handler(m_source.get(), ^{
            while (true) {
                int client = ::accept(descriptor, nullptr, nullptr);
                if (client < 0)
                    return;
                serveClient(SocketStream::create(client));
            }
        });
        dispatch_source_set_cancel_handler(m_source.get(), ^{
            ::close(descriptor);
        });
        dispatch_resume(m_source.get());
    }

    ~SocketListener()
    {
        dispatch_source_cancel(m_source.get());
    }

private:
    OSObjectPtr<dispatch_source_t> m_source;
};

std::unique_ptr<Listener> Listener::create(const String& address, unsigned port, String& error)
{
    auto text = address.isEmpty() ? CString("127.0.0.1") : address.utf8();
    struct sockaddr_storage storage { };
    socklen_t length = 0;
    bool isLoopback = false;
    auto* v4 = reinterpret_cast<struct sockaddr_in*>(&storage);
    auto* v6 = reinterpret_cast<struct sockaddr_in6*>(&storage);
    if (inet_pton(AF_INET, text.data(), &v4->sin_addr) == 1) {
        v4->sin_family = AF_INET;
        v4->sin_len = length = sizeof(*v4);
        v4->sin_port = htons(port);
        isLoopback = (ntohl(v4->sin_addr.s_addr) >> 24) == 127;
    } else if (inet_pton(AF_INET6, text.data(), &v6->sin6_addr) == 1) {
        v6->sin6_family = AF_INET6;
        v6->sin6_len = length = sizeof(*v6);
        v6->sin6_port = htons(port);
        isLoopback = IN6_IS_ADDR_LOOPBACK(&v6->sin6_addr);
    } else {
        error = makeString('\'', address, "' is not a numeric IP address"_s);
        return nullptr;
    }

    int descriptor = ::socket(storage.ss_family, SOCK_STREAM, 0);
    int on = 1;
    if (descriptor >= 0)
        setsockopt(descriptor, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    if (descriptor < 0 || ::bind(descriptor, reinterpret_cast<struct sockaddr*>(&storage), length) || ::listen(descriptor, 16)
        || getsockname(descriptor, reinterpret_cast<struct sockaddr*>(&storage), &length)) {
        error = String::fromUTF8(strerror(errno));
        if (descriptor >= 0)
            ::close(descriptor);
        return nullptr;
    }
    makeNonBlocking(descriptor);

    auto listener = makeUniqueWithoutFastMallocCheck<SocketListener>(descriptor);
    listener->port = ntohs(storage.ss_family == AF_INET6 ? v6->sin6_port : v4->sin_port);
    listener->host = storage.ss_family == AF_INET6 ? makeString('[', String::fromUTF8(text.data()), ']') : String::fromUTF8(text.data());
    listener->isLoopback = isLoopback;
    return listener;
}

// A string the caller owns and gives back to free().
static char* copyForCaller(const String& string)
{
    auto utf8 = string.utf8();
    auto bytes = utf8.spanIncludingNullTerminator();
    auto* copy = static_cast<char*>(malloc(bytes.size()));
    memcpySpan(unsafeMakeSpan(copy, bytes.size()), bytes);
    return copy;
}

} // namespace Zebkit::CDP

using Zebkit::CDP::Server;

extern "C" {

__attribute__((visibility("default"))) unsigned zebkit_cdp_listen(const char* address, unsigned port, char** error)
{
    String reason;
    unsigned listening = Server::singleton().listen(Zebkit::CDP::str(address), port, reason);
    if (!listening && error)
        *error = Zebkit::CDP::copyForCaller(reason);
    return listening;
}

__attribute__((visibility("default"))) unsigned zebkit_cdp_get_port(void)
{
    return Server::singleton().port();
}

__attribute__((visibility("default"))) void zebkit_cdp_stop(void)
{
    Server::singleton().stop();
}

__attribute__((visibility("default"))) void zebkit_cdp_set_discovery_enabled(bool enabled)
{
    Server::singleton().discoveryEnabled = enabled;
}

__attribute__((visibility("default"))) char* zebkit_cdp_get_url(void)
{
    auto url = Server::singleton().browserURL();
    return url.isNull() ? nullptr : Zebkit::CDP::copyForCaller(url);
}

__attribute__((visibility("default"))) void zebkit_cdp_set_create_view_func(ZebkitCDPCreateViewFunc function, void* userData)
{
    Server::singleton().createViewFunc = function;
    Server::singleton().createViewData = userData;
}

}
