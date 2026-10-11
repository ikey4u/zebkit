/*
 * HTTP/1.1 requests and WebSockets (RFC 6455) over a ByteStream: as much of
 * the two as a CDP client uses. One request per connection; a request for
 * /devtools/... becomes a WebSocket, anything else is answered and the
 * connection closed. Messages are text, as CDP's are.
 */

#include "config.h"
#include "CDPWebSocket.h"

#include "CDPInternal.h"
#include <wtf/SHA1.h>
#include <wtf/text/Base64.h>
#include <wtf/text/CString.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringToIntegerConversion.h>

namespace Zebkit::CDP {

// A request line and headers longer than this are not a CDP client's.
static constexpr size_t maxRequestSize = 64 * 1024;
// RFC 6455, 1.3: appended to the client's key to prove the request was read.
static constexpr auto handshakeGUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"_s;

enum : uint8_t {
    OpcodeContinuation = 0,
    OpcodeText = 1,
    OpcodeBinary = 2,
    OpcodeClose = 8,
    OpcodePing = 9,
    OpcodePong = 10,
};

enum : uint16_t {
    CloseNormal = 1000,
    CloseProtocolError = 1002,
    ClosePolicyViolation = 1008,
    CloseTooBig = 1009,
};

class Client final : public Socket {
public:
    static void serve(Ref<ByteStream>&& stream)
    {
        // Alive for as long as the connection is, whoever else holds it.
        auto& client = adoptRef(*new Client(WTF::move(stream))).leakRef();
        client.m_stream->onData = [&client](std::span<const uint8_t> data) {
            Ref protectedClient { client };
            client.received(data);
        };
        client.m_stream->onClosed = [&client] {
            client.streamClosed();
            client.deref();
        };
    }

private:
    enum class State : uint8_t { Request, Open, Closing, Closed };

    explicit Client(Ref<ByteStream>&& stream)
        : m_stream(WTF::move(stream))
    {
    }

    // Socket
    bool isOpen() const final { return m_state == State::Open; }

    void send(const String& text) final
    {
        if (isOpen())
            sendFrame(OpcodeText, byteCast<uint8_t>(text.utf8().span()));
    }

    void close(ASCIILiteral refusal) final
    {
        if (isOpen())
            closeWith(refusal.isNull() ? CloseNormal : ClosePolicyViolation, refusal);
    }

    void closeWith(uint16_t code, ASCIILiteral reason = { })
    {
        Vector<uint8_t> payload;
        payload.append(static_cast<uint8_t>(code >> 8));
        payload.append(static_cast<uint8_t>(code));
        if (!reason.isNull())
            payload.append(byteCast<uint8_t>(reason.span()));
        sendFrame(OpcodeClose, payload.span());
        m_state = State::Closing;
        m_stream->close();
    }

    void streamClosed()
    {
        m_state = State::Closed;
        m_stream->onData = nullptr;
        // Whoever is told may drop the last reference it knows of.
        if (auto closed = std::exchange(onClosed, nullptr))
            closed();
        onMessage = nullptr;
    }

    void received(std::span<const uint8_t> data)
    {
        if (m_state == State::Closing || m_state == State::Closed)
            return;
        m_buffer.append(data);
        if (m_state == State::Request)
            readRequest();
        while (m_state == State::Open && readFrame()) { }
    }

    // MARK: HTTP

    void respond(unsigned status, const String& body, ASCIILiteral type)
    {
        ASCIILiteral reason = status == 200 ? "OK"_s : status == 400 ? "Bad Request"_s : status == 403 ? "Forbidden"_s
            : status == 404 ? "Not Found"_s : status == 405 ? "Method Not Allowed"_s : status == 431 ? "Request Header Fields Too Large"_s : "Error"_s;
        auto bytes = body.utf8();
        auto head = makeString("HTTP/1.1 "_s, status, ' ', reason, "\r\nServer: zebkit\r\n"_s,
            type.isNull() ? ""_s : "Content-Type: "_s, type.isNull() ? ""_s : type, type.isNull() ? ""_s : "\r\n"_s,
            "Content-Length: "_s, bytes.length(), "\r\nConnection: close\r\n\r\n"_s).utf8();
        m_stream->write(byteCast<uint8_t>(head.span()));
        m_stream->write(byteCast<uint8_t>(bytes.span()));
        m_state = State::Closing;
        m_stream->close();
    }

    void readRequest()
    {
        static constexpr std::array<uint8_t, 4> terminator { '\r', '\n', '\r', '\n' };
        auto buffered = m_buffer.span();
        auto end = std::ranges::search(buffered, terminator);
        if (end.empty()) {
            if (m_buffer.size() > maxRequestSize)
                respond(431, "Request too large"_s, "text/plain"_s);
            return;
        }
        size_t headLength = std::ranges::distance(buffered.begin(), end.begin());
        auto head = String::fromUTF8(m_buffer.span().first(headLength));
        m_buffer.removeAt(0, headLength + terminator.size());

        auto lines = head.split("\r\n"_s);
        auto requestLine = lines.isEmpty() ? Vector<String>() : lines[0].split(' ');
        if (requestLine.size() != 3 || !requestLine[2].startsWith("HTTP/1."_s)) {
            respond(400, "Bad request"_s, "text/plain"_s);
            return;
        }
        StringMap<String> headers;
        for (size_t index = 1; index < lines.size(); ++index) {
            auto colon = lines[index].find(':');
            if (colon != notFound)
                headers.set(lines[index].left(colon).trim(isASCIIWhitespace).convertToASCIILowercase(), lines[index].substring(colon + 1).trim(isASCIIWhitespace));
        }

        HTTPRequest request;
        request.method = requestLine[0];
        auto target = requestLine[1];
        auto question = target.find('?');
        request.path = question == notFound ? target : target.left(question);
        // Null when there is none, empty for "path?".
        if (question != notFound)
            request.query = makeString(""_s, target.substring(question + 1));
        request.host = headers.get("host"_s);
        request.origin = headers.get("origin"_s);

        if (auto response = Server::singleton().handleHTTP(request)) {
            respond(response->status, response->body, response->type);
            return;
        }

        // Not answered: the request is for a WebSocket.
        auto key = headers.get("sec-websocket-key"_s);
        if (!equalLettersIgnoringASCIICase(headers.get("upgrade"_s), "websocket"_s) || key.isEmpty()) {
            respond(400, "This address is a WebSocket"_s, "text/plain"_s);
            return;
        }
        SHA1 sha1;
        sha1.addBytes(byteCast<uint8_t>(key.utf8().span()));
        sha1.addBytes(byteCast<uint8_t>(handshakeGUID.span()));
        SHA1::Digest digest;
        sha1.computeHash(digest);
        auto accept = makeString("HTTP/1.1 101 Switching Protocols\r\nServer: zebkit\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: "_s,
            base64EncodeToString(std::span<const uint8_t> { digest }), "\r\n\r\n"_s).utf8();
        m_stream->write(byteCast<uint8_t>(accept.span()));
        m_state = State::Open;
        Server::singleton().acceptSocket(request, *this);
    }

    // MARK: Frames

    void sendFrame(uint8_t opcode, std::span<const uint8_t> payload)
    {
        Vector<uint8_t> frame;
        frame.reserveInitialCapacity(payload.size() + 10);
        frame.append(static_cast<uint8_t>(0x80 | opcode));
        if (payload.size() < 126)
            frame.append(static_cast<uint8_t>(payload.size()));
        else if (payload.size() <= 0xffff) {
            frame.append(126);
            frame.append(static_cast<uint8_t>(payload.size() >> 8));
            frame.append(static_cast<uint8_t>(payload.size()));
        } else {
            frame.append(127);
            for (int shift = 56; shift >= 0; shift -= 8)
                frame.append(static_cast<uint8_t>(static_cast<uint64_t>(payload.size()) >> shift));
        }
        frame.append(payload);
        m_stream->write(frame.span());
    }

    // Takes one frame off the buffer. False when there is not a whole one.
    bool readFrame()
    {
        auto bytes = m_buffer.span();
        if (bytes.size() < 2)
            return false;
        bool final = bytes[0] & 0x80;
        uint8_t opcode = bytes[0] & 0x0f;
        bool masked = bytes[1] & 0x80;
        uint64_t length = bytes[1] & 0x7f;
        size_t offset = 2;
        if (length == 126) {
            if (bytes.size() < 4)
                return false;
            length = bytes[2] << 8 | bytes[3];
            offset = 4;
        } else if (length == 127) {
            if (bytes.size() < 10)
                return false;
            length = 0;
            for (size_t index = 2; index < 10; ++index)
                length = length << 8 | bytes[index];
            offset = 10;
        }
        // A client masks what it sends, and control frames are short and whole.
        if (!masked || (bytes[0] & 0x70) || (opcode >= OpcodeClose && (length > 125 || !final))) {
            closeWith(CloseProtocolError);
            return false;
        }
        if (length > maxIncomingMessageSize || m_message.size() + length > maxIncomingMessageSize) {
            closeWith(CloseTooBig);
            return false;
        }
        if (bytes.size() < offset + 4 + length)
            return false;
        auto mask = bytes.subspan(offset, 4);
        auto payload = m_buffer.mutableSpan().subspan(offset + 4, static_cast<size_t>(length));
        for (size_t index = 0; index < payload.size(); ++index)
            payload[index] ^= mask[index % 4];

        switch (opcode) {
        case OpcodeText:
        case OpcodeBinary:
        case OpcodeContinuation:
            if (opcode != OpcodeContinuation)
                m_messageIsText = opcode == OpcodeText;
            m_message.append(payload);
            if (final) {
                auto message = std::exchange(m_message, { });
                if (m_messageIsText && onMessage)
                    onMessage(String::fromUTF8(message.span()));
            }
            break;
        case OpcodePing:
            sendFrame(OpcodePong, payload);
            break;
        case OpcodePong:
            break;
        case OpcodeClose:
            closeWith(CloseNormal);
            return false;
        default:
            closeWith(CloseProtocolError);
            return false;
        }
        // The message's handler may have closed the socket.
        if (m_buffer.size() >= offset + 4 + length)
            m_buffer.removeAt(0, offset + 4 + static_cast<size_t>(length));
        return true;
    }

    const Ref<ByteStream> m_stream;
    State m_state { State::Request };
    Vector<uint8_t> m_buffer;
    Vector<uint8_t> m_message;
    bool m_messageIsText { false };
};

void serveClient(Ref<ByteStream>&& stream)
{
    Client::serve(WTF::move(stream));
}

} // namespace Zebkit::CDP
