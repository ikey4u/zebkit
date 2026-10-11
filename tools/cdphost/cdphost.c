/*
 * cdphost: the smallest application that embeds the engine and can be driven
 * over the Chrome DevTools Protocol. It opens one page and serves CDP until
 * it is killed. scripts/cdp.sh uses it as the browser under test, and it
 * shows what an embedding application has to do: create a view and call
 * zebkit_cdp_listen().
 *
 *   cdphost [--port N] [--address IP] [--private] [--headless] [--size WxH] [URL]
 *
 * --private serves no discovery pages: a client needs the address printed
 * on standard error, which only this process and whoever reads its output
 * know.
 */

#include <glib-unix.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wpe/headless/wpe-headless.h>
#include <wpe/webkit.h>
#include <zebkit-cdp.h>

static gboolean quit(gpointer loop)
{
    g_main_loop_quit(loop);
    return G_SOURCE_REMOVE;
}

int main(int argc, char** argv)
{
    const char* url = "about:blank";
    const char* address = NULL;
    guint port = 9222;
    gboolean headless = FALSE;
    gboolean private = FALSE;
    int width = 1280, height = 720;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--port") && i + 1 < argc)
            port = (guint)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--address") && i + 1 < argc)
            address = argv[++i];
        else if (!strcmp(argv[i], "--headless"))
            headless = TRUE;
        else if (!strcmp(argv[i], "--private"))
            private = TRUE;
        else if (!strcmp(argv[i], "--size") && i + 1 < argc) {
            if (sscanf(argv[++i], "%dx%d", &width, &height) != 2) {
                fprintf(stderr, "cdphost: --size takes WIDTHxHEIGHT\n");
                return 2;
            }
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "usage: %s [--port N] [--address IP] [--private] [--headless] [--size WxH] [URL]\n", argv[0]);
            return 2;
        } else
            url = argv[i];
    }

    /* Without --headless the engine picks Wayland, X11 or no window at all,
     * whichever the machine has. */
    WPEDisplay* display = headless ? wpe_display_headless_new() : NULL;
    WebKitWebView* view = display
        ? WEBKIT_WEB_VIEW(g_object_new(WEBKIT_TYPE_WEB_VIEW, "display", display, NULL))
        : WEBKIT_WEB_VIEW(g_object_new(WEBKIT_TYPE_WEB_VIEW, NULL));
    WPEView* wpeView = webkit_web_view_get_wpe_view(view);
    if (wpeView && wpe_view_get_toplevel(wpeView))
        wpe_toplevel_resize(wpe_view_get_toplevel(wpeView), width, height);

    GError* error = NULL;
    zebkit_cdp_set_discovery_enabled(!private);
    if (!zebkit_cdp_listen(address, port, &error)) {
        fprintf(stderr, "cdphost: %s\n", error->message);
        return 1;
    }

    GMainLoop* loop = g_main_loop_new(NULL, FALSE);
    g_unix_signal_add(SIGTERM, quit, loop);
    g_unix_signal_add(SIGINT, quit, loop);
    webkit_web_view_load_uri(view, url);
    g_main_loop_run(loop);
    zebkit_cdp_stop();
    return 0;
}
