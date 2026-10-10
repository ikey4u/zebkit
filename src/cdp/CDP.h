/*
 * Chrome DevTools Protocol endpoint for zebkit.
 *
 * WebKit does not speak CDP. It has its own remote debugging protocol, the
 * Web Inspector Protocol, which covers much of the same ground with
 * different message shapes and without the browser-level parts (targets,
 * input, navigation). This directory is a bridge compiled into the engine
 * library: it serves the HTTP and WebSocket endpoints that CDP clients
 * expect, answers what it can natively through the WebKit API, and
 * translates the rest to and from the Web Inspector Protocol.
 *
 * docs/CDP.md lists what is covered.
 */

#pragma once

typedef struct _WebKitWebView WebKitWebView;

namespace Zebkit::CDP {

// Called by WebKitWebView when a view has been constructed and when it is
// about to be destroyed. Every view in the process is a CDP page target
// while the server is listening.
void webViewCreated(WebKitWebView*);
void webViewDestroyed(WebKitWebView*);

} // namespace Zebkit::CDP
