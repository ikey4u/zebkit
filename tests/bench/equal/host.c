/* The zebkit host of scripts/electron-compare.sh: what electron/main.js does,
 * in zebkit's API. One window, its content 1280 by 800, showing BENCH_URL; with
 * BENCH_BIND set, the page can call echo(text) on the host. */

#define _POSIX_C_SOURCE 200809L
#include <stdlib.h>
#include <string.h>

#include <zebkit/zebkit.h>

static void closed(zebkit_view *view, void *data) {
    (void)view, (void)data;
    zebkit_quit();
}

/* The arguments are a JSON array of one value, and the answer is that value. */
static void echo(zebkit_view *view, const char *origin, const char *args_json, zebkit_call *call, void *data) {
    (void)view, (void)origin, (void)data;
    char *value = strdup(args_json + 1);
    value[strlen(value) - 1] = '\0';
    zebkit_call_resolve(call, value);
    free(value);
}

static void ready(void *data) {
    (void)data;
    zebkit_view *view = zebkit_view_new(getenv("BENCH_URL"), 1280, 800);
    zebkit_view_on_close(view, closed, NULL);
    /* As Electron's window has it: without a window manager, nothing else gives it. */
    zebkit_view_focus(view);
    if (getenv("BENCH_BIND")) {
        zebkit_view_grant(view, "*", ZEBKIT_ALLOW_APP, NULL);
        zebkit_view_bind(view, "echo", echo, NULL);
    }
}

int main(int argc, char **argv) {
    return zebkit_main(argc, argv, ready, NULL);
}
