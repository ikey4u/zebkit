// HTTP and WebSockets over a plain connection, for platforms whose own
// libraries do not serve both on one port. A platform's listener makes a
// ByteStream for each client it accepts and gives it to serveClient(); the
// requests reach Server::handleHTTP() and Server::acceptSocket() from there,
// as they do from libsoup on Linux.

#pragma once

#include "CDPPlatform.h"
#include <span>

namespace Zebkit::CDP {

// One accepted connection, as bytes. The platform makes it; serveClient()
// sets the two callbacks and then only writes and closes.
class ByteStream : public RefCounted<ByteStream> {
public:
    virtual ~ByteStream() = default;
    virtual void write(std::span<const uint8_t>) = 0;
    // What was written is still sent. onClosed follows, not from inside
    // this call.
    virtual void close() = 0;

    Function<void(std::span<const uint8_t>)> onData;
    // The other side went, or close() finished. Called at most once.
    Function<void()> onClosed;
};

void serveClient(Ref<ByteStream>&&);

} // namespace Zebkit::CDP
