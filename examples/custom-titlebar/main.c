/*
 * A window whose title bar is the page's (<zebkit/window.h>): frame
 * "overlay", the window's buttons over the page, the page's app-region where
 * it is dragged from, granted to the page's own origin only.
 *
 *   cc -std=c11 -I$PKG/include examples/custom-titlebar/main.c -L$PKG -l:libzebkit.so.1 \
 *       -Wl,-rpath,$PKG -o custom-titlebar
 *   ./custom-titlebar file://$PWD/examples/custom-titlebar/page.html
 */

#include <stdio.h>

#include <zebkit/window.h>
#include <zebkit/zebkit.h>

static const char *url;

static void closed(zebkit_view *view, void *data) {
    (void)view, (void)data;
    zebkit_quit();
}

static void state(zebkit_view *view, const char *json, void *data) {
    (void)view, (void)data;
    printf("window: %s\n", json);
}

static void ready(void *data) {
    (void)data;
    char *error = NULL;
    zebkit_view *view = zebkit_view_new_with_options(NULL, url,
        "{\"width\": 900, \"height\": 600, \"frame\": \"overlay\", \"title\": \"Custom title bar\","
        " \"background\": \"#1b1d2a\", \"appearance\": \"dark\","
        " \"controls\": {\"height\": 40, \"color\": \"#252838\", \"symbolColor\": \"#e6e8f2\"},"
        " \"dragRegions\": {\"source\": \"css\", \"origins\": [\"file://\"]},"
        " \"fallback\": \"system\"}",
        &error);
    if (!view) {
        fprintf(stderr, "custom-titlebar: %s\n", error ? error : "no view");
        zebkit_free(error);
        zebkit_quit();
        return;
    }
    zebkit_view_on_close(view, closed, NULL);
    zebkit_view_on_window_state(view, state, NULL);
}

int main(int argc, char **argv) {
    url = argc > 1 ? argv[1] : "about:blank";
    return zebkit_main(argc, argv, ready, NULL);
}
