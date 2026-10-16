/*
 * Public C interface of the zebkit CDP endpoint, for applications that embed
 * the engine. Nothing here is needed when the endpoint is started with the
 * ZEBKIT_CDP_PORT environment variable.
 *
 * The interface is the same on every platform but for the types that are
 * the platform's: the view (WebKitWebView on Linux, WKWebView on macOS,
 * WKViewRef on Windows), and how an error is reported.
 */

#ifndef ZEBKIT_CDP_H
#define ZEBKIT_CDP_H

#ifdef __APPLE__

#include <stdbool.h>

#ifdef __OBJC__
@class WKWebView;
@class WKWebViewConfiguration;
#else
typedef struct objc_object WKWebView;
typedef struct objc_object WKWebViewConfiguration;
#endif

#ifdef __cplusplus
extern "C" {
#endif

/*
 * As below, with these differences. @error receives a message to free()
 * and may be NULL. A view the application creates for the endpoint must be
 * created with the configuration it is handed, which carries the page's
 * opener or the browser context it belongs to.
 */
unsigned zebkit_cdp_listen(const char *address, unsigned port, char **error);
unsigned zebkit_cdp_get_port(void);
void zebkit_cdp_stop(void);
void zebkit_cdp_set_discovery_enabled(bool enabled);
/* Free with free(). */
char *zebkit_cdp_get_url(void);

typedef WKWebView *(*ZebkitCDPCreateViewFunc)(WKWebView *related, WKWebViewConfiguration *configuration, void *user_data);

void zebkit_cdp_set_create_view_func(ZebkitCDPCreateViewFunc func, void *user_data);

#ifdef __cplusplus
}
#endif

#elif defined(_WIN32)

#include <WebKit/WKBase.h>
#include <stdbool.h>

#ifndef ZEBKIT_CDP_API
#define ZEBKIT_CDP_API __declspec(dllimport)
#endif

#ifdef __cplusplus
extern "C" {
#endif

/*
 * As below, with the differences macOS has too. @error receives a message
 * to free() and may be NULL. A view the application creates for the
 * endpoint (WKViewCreate) must be created with the configuration it is
 * handed, which carries the page's opener or the browser context it
 * belongs to.
 */
ZEBKIT_CDP_API unsigned zebkit_cdp_listen(const char *address, unsigned port, char **error);
ZEBKIT_CDP_API unsigned zebkit_cdp_get_port(void);
ZEBKIT_CDP_API void zebkit_cdp_stop(void);
ZEBKIT_CDP_API void zebkit_cdp_set_discovery_enabled(bool enabled);
/* Free with free(). */
ZEBKIT_CDP_API char *zebkit_cdp_get_url(void);

typedef WKViewRef (*ZebkitCDPCreateViewFunc)(WKViewRef related, WKPageConfigurationRef configuration, void *user_data);

ZEBKIT_CDP_API void zebkit_cdp_set_create_view_func(ZebkitCDPCreateViewFunc func, void *user_data);

#ifdef __cplusplus
}
#endif

#else /* Linux */

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
guint zebkit_cdp_listen(const char *address, guint port, GError **error);

/* The port the endpoint listens on, or 0 when it is not running. */
guint zebkit_cdp_get_port(void);

/*
 * Stops the endpoint: disconnects every client, closes the pages the
 * endpoint created and stops listening. The application's own views are
 * left as they are. The endpoint can be started again.
 */
void zebkit_cdp_stop(void);

/*
 * The endpoint has no password. By default it also serves the HTTP pages
 * (/json/version, /json/list) from which any program on the machine can
 * learn the address of the WebSocket. With discovery disabled those pages
 * are not served, and a client can connect only if it was given the address
 * zebkit_cdp_get_url() returns, which contains a random id. Call this
 * before zebkit_cdp_listen(); ZEBKIT_CDP_DISCOVERY=0 does the same.
 */
void zebkit_cdp_set_discovery_enabled(gboolean enabled);

/*
 * The WebSocket address of the browser target, what Puppeteer calls
 * browserWSEndpoint, or NULL when the endpoint is not running. Free with
 * g_free().
 */
char *zebkit_cdp_get_url(void);

/*
 * Called when a client asks for a new page (Target.createTarget, /json/new)
 * or a page opens a window. Return a new view, to which the application
 * holds the reference, or NULL to let the endpoint create and own one.
 * @related is the opener for window.open() and NULL otherwise; a view
 * created for it must be constructed with the "related-view" property.
 */
typedef WebKitWebView *(*ZebkitCDPCreateViewFunc)(WebKitWebView *related, gpointer user_data);

void zebkit_cdp_set_create_view_func(ZebkitCDPCreateViewFunc func, gpointer user_data);

G_END_DECLS

#endif /* Linux */

#endif
