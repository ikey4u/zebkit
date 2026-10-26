/* A human-facing off-screen host for the localhost WebAuthn tests.
 * The UI and focus flags are opt-in; default invocation stays windowless. */
#include <stdio.h>
#include <string.h>

#include <zebkit/zebkit.h>
#include <zebkit/offscreen.h>

static const char *url = "about:blank";
static int allow_ui;
static int focused;

static void closed(zebkit_view *view, void *data) {
    (void)view;
    (void)data;
    zebkit_quit();
}

static void ready(void *data) {
    (void)data;
    zebkit_view *view = zebkit_view_new_offscreen(NULL, 800, 600, 1);
    if (!view) {
        fputs("FAIL creating the off-screen authentication fixture\n", stderr);
        zebkit_quit();
        return;
    }
    if (!zebkit_view_set_authentication_ui(view, allow_ui != 0)) {
        fputs("FAIL setting the authentication UI policy\n", stderr);
        zebkit_quit();
        return;
    }
    zebkit_view_set_focus(view, focused != 0);
    zebkit_view_on_close(view, closed, NULL);
    zebkit_view_load_url(view, url);
}

int main(int argc, char **argv) {
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--test-authentication-ui") == 0)
            allow_ui = 1;
        else if (strcmp(argv[i], "--test-focused") == 0)
            focused = 1;
        else if (strncmp(argv[i], "--", 2) != 0)
            url = argv[i];
    }
    return zebkit_main(argc, argv, ready, NULL);
}
