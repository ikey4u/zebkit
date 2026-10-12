/*
 * window: a small browser window, using zebkit.
 *
 *   window [URL]
 *
 * A window with a web page in it and, across the top, a back button, a
 * forward button and a box to type an address into.
 *
 * What a host has to do is all here: create a WebKitWebView, give its
 * window a size and a title, load something, and run the GLib main loop.
 * The engine chooses the display itself: Wayland if there is one, else X11,
 * else no window at all (the page still loads and can be driven over CDP).
 *
 * zebkit draws web pages and nothing else: it has no buttons or text boxes
 * of its own. An application that has a toolkit draws its controls with
 * that. This one has none, so the bar is itself a piece of web page: a
 * script the engine runs in every document, in a world of its own where the
 * page's scripts cannot reach it, and which talks to this program through a
 * message handler. toolbar_script in window-bar.h is that script; this
 * file is the host's side of the conversation. window.m is the same
 * program for macOS.
 *
 * Build against a staged or packaged zebkit (see examples/README.md):
 *
 *   cc window.c -o window $(pkg-config --cflags --libs wpe-webkit-2.0)
 */

#include "window-bar.h"
#include <string.h>
#include <wpe/webkit.h>

/* Tells the bar where the page is and which buttons can do something. */
static void update_toolbar(WebKitWebView* view)
{
    const char* uri = webkit_web_view_get_uri(view);
    char* escaped = g_uri_escape_string(uri ? uri : "", NULL, FALSE);
    char* script = g_strdup_printf("globalThis.zebkitBar && zebkitBar.update(decodeURIComponent('%s'), %s, %s)", escaped,
        webkit_web_view_can_go_back(view) ? "true" : "false", webkit_web_view_can_go_forward(view) ? "true" : "false");
    webkit_web_view_evaluate_javascript(view, script, -1, BAR_WORLD, NULL, NULL, NULL, NULL);
    g_free(script);
    g_free(escaped);
}

/* What was typed is an address if it looks like one; "example.com" means
 * "https://example.com". */
static char* address_from_input(const char* text)
{
    if (strstr(text, "://") || g_str_has_prefix(text, "about:") || g_str_has_prefix(text, "data:"))
        return g_strdup(text);
    return g_strconcat("https://", text, NULL);
}

static void toolbar_message(WebKitUserContentManager* manager, JSCValue* value, gpointer data)
{
    WebKitWebView* view = WEBKIT_WEB_VIEW(data);
    char* message = jsc_value_to_string(value);
    if (!strcmp(message, "back"))
        webkit_web_view_go_back(view);
    else if (!strcmp(message, "forward"))
        webkit_web_view_go_forward(view);
    else if (g_str_has_prefix(message, "go ")) {
        char* address = address_from_input(message + 3);
        webkit_web_view_load_uri(view, address);
        g_free(address);
    } else if (!strcmp(message, "ready"))
        update_toolbar(view);
    g_free(message);
}

static void page_changed(WebKitWebView* view, GParamSpec* spec, gpointer data)
{
    update_toolbar(view);
}

static void load_changed(WebKitWebView* view, WebKitLoadEvent event, gpointer data)
{
    update_toolbar(view);
}

static void title_changed(WebKitWebView* view, GParamSpec* spec, gpointer toplevel)
{
    const char* title = webkit_web_view_get_title(view);
    wpe_toplevel_set_title(WPE_TOPLEVEL(toplevel), title && *title ? title : "zebkit");
}

static void window_closed(WPEView* view, gpointer loop)
{
    g_main_loop_quit(loop);
}

int main(int argc, char** argv)
{
    const char* url = argc > 1 ? argv[1] : "https://example.com/";
    GMainLoop* loop = g_main_loop_new(NULL, FALSE);

    /* The bar: a script for every document, and the channel it reports on. */
    WebKitUserContentManager* content = webkit_user_content_manager_new();
    WebKitUserScript* script = webkit_user_script_new_for_world(toolbar_script,
        WEBKIT_USER_CONTENT_INJECT_TOP_FRAME, WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START, BAR_WORLD, NULL, NULL);
    webkit_user_content_manager_add_script(content, script);
    webkit_user_script_unref(script);
    webkit_user_content_manager_register_script_message_handler(content, BAR_CHANNEL, BAR_WORLD);

    /* The view is the page. */
    WebKitWebView* view = WEBKIT_WEB_VIEW(g_object_new(WEBKIT_TYPE_WEB_VIEW, "user-content-manager", content, NULL));
    g_signal_connect(content, "script-message-received::" BAR_CHANNEL, G_CALLBACK(toolbar_message), view);
    g_signal_connect(view, "notify::uri", G_CALLBACK(page_changed), NULL);
    g_signal_connect(view, "load-changed", G_CALLBACK(load_changed), NULL);

    /* The window the view is shown in. There is none when the machine has
     * no display. */
    WPEView* surface = webkit_web_view_get_wpe_view(view);
    WPEToplevel* window = surface ? wpe_view_get_toplevel(surface) : NULL;
    if (window) {
        wpe_toplevel_resize(window, 1024, 768);
        wpe_toplevel_set_title(window, "zebkit");
        g_signal_connect(view, "notify::title", G_CALLBACK(title_changed), window);
        g_signal_connect(surface, "closed", G_CALLBACK(window_closed), loop);
    }

    webkit_web_view_load_uri(view, url);
    g_main_loop_run(loop);

    g_object_unref(view);
    g_object_unref(content);
    g_main_loop_unref(loop);
    return 0;
}
