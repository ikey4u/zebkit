/*
 * The endpoint's network side on Windows: a listening socket and one
 * ByteStream per client, on the main thread, and the public C API in
 * zebkit-cdp.h. HTTP and WebSockets are CDPWebSocket.cpp's; what a request
 * means is decided in CDPServer.cpp.
 *
 * Winsock sockets that report to a window: the main thread of a Windows
 * program runs a message loop, and a socket's news arrive in it as
 * messages (WSAAsyncSelect), so nothing here has a thread of its own.
 */

#include "config.h"
#include "CDPInternal.h"

// Linux builds every .cpp file of this directory.
#if PLATFORM(WIN)

#include "CDPWebSocket.h"
// Here the functions are defined, not imported.
#define ZEBKIT_CDP_API __declspec(dllexport)
#include "zebkit-cdp.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/text/CString.h>
#include <wtf/text/MakeString.h>

namespace Zebkit::CDP {

static constexpr UINT socketMessage = WM_USER + 1;

// What a socket's message is handed to.
static HashMap<uintptr_t, Function<void(unsigned event)>>& socketHandlers()
{
    static NeverDestroyed<HashMap<uintptr_t, Function<void(unsigned)>>> handlers;
    return handlers;
}

// The window the sockets report to. It is never shown.
static HWND socketWindow()
{
    static HWND window;
    if (window)
        return window;
    WSADATA data;
    WSAStartup(MAKEWORD(2, 2), &data);

    static const wchar_t className[] = L"ZebkitCDPSockets";
    WNDCLASSEXW windowClass { };
    windowClass.cbSize = sizeof(windowClass);
    windowClass.hInstance = ::GetModuleHandleW(nullptr);
    windowClass.lpszClassName = className;
    windowClass.lpfnWndProc = [](HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) -> LRESULT {
        if (message != socketMessage)
            return ::DefWindowProcW(hwnd, message, wParam, lParam);
        // A handler may close its socket, and others, while it runs.
        auto found = socketHandlers().find(static_cast<uintptr_t>(wParam));
        if (found != socketHandlers().end() && found->value)
            found->value(WSAGETSELECTERROR(lParam) ? FD_CLOSE : WSAGETSELECTEVENT(lParam));
        return 0;
    };
    ::RegisterClassExW(&windowClass);
    window = ::CreateWindowExW(0, className, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, windowClass.hInstance, nullptr);
    return window;
}

static void watch(SOCKET socket, long events, Function<void(unsigned)>&& handler)
{
    socketHandlers().set(static_cast<uintptr_t>(socket), WTF::move(handler));
    // Makes the socket non-blocking as well.
    WSAAsyncSelect(socket, socketWindow(), socketMessage, events);
}

static void unwatchAndClose(SOCKET socket)
{
    WSAAsyncSelect(socket, socketWindow(), 0, 0);
    socketHandlers().remove(static_cast<uintptr_t>(socket));
    ::closesocket(socket);
}

class SocketStream final : public ByteStream {
public:
    static Ref<SocketStream> create(SOCKET socket) { return adoptRef(*new SocketStream(socket)); }

    ~SocketStream()
    {
        ASSERT(m_socket == INVALID_SOCKET);
    }

private:
    explicit SocketStream(SOCKET socket)
        : m_socket(socket)
    {
        BOOL on = TRUE;
        setsockopt(socket, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&on), sizeof(on));

        // The open connection keeps the stream alive until it is closed.
        ref();
        watch(socket, FD_READ | FD_WRITE | FD_CLOSE, [this](unsigned event) {
            Ref protectedThis { *this };
            if (event & FD_WRITE)
                flush();
            if (event & (FD_READ | FD_CLOSE))
                readAvailable(event & FD_CLOSE);
        });
    }

    void readAvailable(bool closed)
    {
        std::array<uint8_t, 64 * 1024> buffer;
        while (m_socket != INVALID_SOCKET && !m_closing) {
            int count = ::recv(m_socket, reinterpret_cast<char*>(buffer.data()), static_cast<int>(buffer.size()), 0);
            if (count > 0) {
                if (onData)
                    onData(std::span<const uint8_t> { buffer }.first(static_cast<size_t>(count)));
                continue;
            }
            if (count < 0 && WSAGetLastError() == WSAEWOULDBLOCK && !closed)
                return;
            // The other side closed, or the connection broke.
            shutDown();
            return;
        }
    }

    void write(std::span<const uint8_t> data) final
    {
        if (m_socket == INVALID_SOCKET || m_closing)
            return;
        m_pending.append(data);
        flush();
    }

    void flush()
    {
        while (m_socket != INVALID_SOCKET && !m_pending.isEmpty()) {
            int count = ::send(m_socket, reinterpret_cast<const char*>(m_pending.span().data()), static_cast<int>(std::min<size_t>(m_pending.size(), 1 << 20)), 0);
            if (count > 0) {
                m_pending.removeAt(0, count);
                continue;
            }
            // The socket says, with FD_WRITE, when it takes more.
            if (count < 0 && WSAGetLastError() == WSAEWOULDBLOCK)
                return;
            m_pending.clear();
            shutDown();
            return;
        }
        if (m_closing)
            shutDown();
    }

    void close() final
    {
        if (m_socket == INVALID_SOCKET || std::exchange(m_closing, true))
            return;
        // Not from inside the caller, and after what is still to be sent.
        runAfter(0, [protectedThis = Ref { *this }] {
            protectedThis->flush();
        });
    }

    void shutDown()
    {
        if (m_socket == INVALID_SOCKET)
            return;
        ::shutdown(m_socket, SD_BOTH);
        unwatchAndClose(std::exchange(m_socket, INVALID_SOCKET));
        onData = nullptr;
        if (auto closed = std::exchange(onClosed, nullptr))
            closed();
        deref(); // The reference the open connection held.
    }

    SOCKET m_socket;
    bool m_closing { false };
    Vector<uint8_t> m_pending;
};

class SocketListener final : public Listener {
public:
    explicit SocketListener(SOCKET socket)
        : m_socket(socket)
    {
        watch(socket, FD_ACCEPT, [socket](unsigned) {
            while (true) {
                SOCKET client = ::accept(socket, nullptr, nullptr);
                if (client == INVALID_SOCKET)
                    return;
                serveClient(SocketStream::create(client));
            }
        });
    }

    ~SocketListener()
    {
        unwatchAndClose(m_socket);
    }

private:
    SOCKET m_socket;
};

static String lastSocketError()
{
    wchar_t message[256] { };
    int code = WSAGetLastError();
    if (!::FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code, 0, message, 255, nullptr))
        return makeString("socket error "_s, code);
    return String(message).trim(isUnicodeCompatibleASCIIWhitespace<char16_t>);
}

std::unique_ptr<Listener> Listener::create(const String& address, unsigned port, String& error)
{
    socketWindow();
    auto text = address.isEmpty() ? CString("127.0.0.1") : address.utf8();
    struct sockaddr_storage storage { };
    int length = 0;
    bool isLoopback = false;
    auto* v4 = reinterpret_cast<struct sockaddr_in*>(&storage);
    auto* v6 = reinterpret_cast<struct sockaddr_in6*>(&storage);
    if (inet_pton(AF_INET, text.data(), &v4->sin_addr) == 1) {
        v4->sin_family = AF_INET;
        length = sizeof(*v4);
        v4->sin_port = htons(port);
        isLoopback = (ntohl(v4->sin_addr.s_addr) >> 24) == 127;
    } else if (inet_pton(AF_INET6, text.data(), &v6->sin6_addr) == 1) {
        v6->sin6_family = AF_INET6;
        length = sizeof(*v6);
        v6->sin6_port = htons(port);
        isLoopback = IN6_IS_ADDR_LOOPBACK(&v6->sin6_addr);
    } else {
        error = makeString('\'', address, "' is not a numeric IP address"_s);
        return nullptr;
    }

    // No SO_REUSEADDR: on Windows it lets a second program take a port
    // that is in use, which is the opposite of what it is for elsewhere.
    SOCKET socket = ::socket(storage.ss_family, SOCK_STREAM, IPPROTO_TCP);
    if (socket == INVALID_SOCKET || ::bind(socket, reinterpret_cast<struct sockaddr*>(&storage), length) || ::listen(socket, 16)
        || getsockname(socket, reinterpret_cast<struct sockaddr*>(&storage), &length)) {
        error = lastSocketError();
        if (socket != INVALID_SOCKET)
            ::closesocket(socket);
        return nullptr;
    }

    auto listener = makeUniqueWithoutFastMallocCheck<SocketListener>(socket);
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

unsigned zebkit_cdp_listen(const char* address, unsigned port, char** error)
{
    String reason;
    unsigned listening = Server::singleton().listen(Zebkit::CDP::str(address), port, reason);
    if (!listening && error)
        *error = Zebkit::CDP::copyForCaller(reason);
    return listening;
}

unsigned zebkit_cdp_get_port(void)
{
    return Server::singleton().port();
}

void zebkit_cdp_stop(void)
{
    Server::singleton().stop();
}

void zebkit_cdp_set_discovery_enabled(bool enabled)
{
    Server::singleton().discoveryEnabled = enabled;
}

char* zebkit_cdp_get_url(void)
{
    auto url = Server::singleton().browserURL();
    return url.isNull() ? nullptr : Zebkit::CDP::copyForCaller(url);
}

void zebkit_cdp_set_create_view_func(ZebkitCDPCreateViewFunc function, void* userData)
{
    Server::singleton().createViewFunc = function;
    Server::singleton().createViewData = userData;
}

}

#endif // PLATFORM(WIN)
