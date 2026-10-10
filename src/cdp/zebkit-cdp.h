/*
 * Public C interface of the zebkit CDP endpoint, for applications that embed
 * the engine. Nothing here is needed when the endpoint is started with the
 * ZEBKIT_CDP_PORT environment variable.
 */

#ifndef ZEBKIT_CDP_H
#define ZEBKIT_CDP_H

#include <glib.h>

G_BEGIN_DECLS

typedef struct _WebKitWebView WebKitWebView;

/*
 * Starts the endpoint. @address is a numeric IP address, or NULL for
 * 127.0.0.1. @port may be 0 to let the system choose. Returns the port in
 * use, or 0 with @error set. Calling it again returns the existing port.
 *
 * Every WebKitWebView of the process is exposed as a page, including views
 * created before this call.
 */
guint webkit_zebkit_cdp_listen(const char *address, guint port, GError **error);

/* The port the endpoint listens on, or 0 when it is not running. */
guint webkit_zebkit_cdp_get_port(void);

/*
 * Stops the endpoint: disconnects every client, closes the pages the
 * endpoint created and stops listening. The application's own views are
 * left as they are. The endpoint can be started again.
 */
void webkit_zebkit_cdp_stop(void);

/*
 * The endpoint has no password. By default it also serves the HTTP pages
 * (/json/version, /json/list) from which any program on the machine can
 * learn the address of the WebSocket. With discovery disabled those pages
 * are not served, and a client can connect only if it was given the address
 * webkit_zebkit_cdp_get_url() returns, which contains a random id. Call this
 * before webkit_zebkit_cdp_listen(); ZEBKIT_CDP_DISCOVERY=0 does the same.
 */
void webkit_zebkit_cdp_set_discovery_enabled(gboolean enabled);

/*
 * The WebSocket address of the browser target, what Puppeteer calls
 * browserWSEndpoint, or NULL when the endpoint is not running. Free with
 * g_free().
 */
char *webkit_zebkit_cdp_get_url(void);

/*
 * Called when a client asks for a new page (Target.createTarget, /json/new)
 * or a page opens a window. Return a new view, to which the application
 * holds the reference, or NULL to let the endpoint create and own one.
 * @related is the opener for window.open() and NULL otherwise; a view
 * created for it must be constructed with the "related-view" property.
 */
typedef WebKitWebView *(*ZebkitCDPCreateViewFunc)(WebKitWebView *related, gpointer user_data);

void webkit_zebkit_cdp_set_create_view_func(ZebkitCDPCreateViewFunc func, gpointer user_data);

G_END_DECLS

#endif
