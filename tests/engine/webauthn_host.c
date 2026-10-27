/* A human-facing off-screen host for the localhost WebAuthn tests.
 * The UI and focus flags are opt-in; default invocation stays windowless. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include <zebkit/zebkit.h>
#include <zebkit/offscreen.h>

static const char *url = "about:blank";
static int allow_ui;
static int focused;
static int host_ui;
static int normal_view;
static char *last_authentication;
static unsigned event_count;
static void authentication(zebkit_view *view, const char *json, void *data) {
    (void)view; (void)data;
    free(last_authentication);
    last_authentication = strdup(json);
    ++event_count;
}
static void state(zebkit_view *view, const char *origin, const char *args, zebkit_call *call, void *data) {
    (void)view; (void)origin; (void)args; (void)data;
    zebkit_call_resolve(call, last_authentication ? last_authentication : "null");
}
static void action(zebkit_view *view, const char *origin, const char *args, zebkit_call *call, void *data) {
    (void)origin; (void)data;
    size_t length = strlen(args);
    char *json = strdup(length >= 2 ? args + 1 : "");
    if (length >= 2) json[length - 2] = 0;
    bool accepted = zebkit_view_authentication_action(view, json);
    free(json);
    zebkit_call_resolve(call, accepted ? "true" : "false");
}
static void detach(zebkit_view *view, const char *origin, const char *args, zebkit_call *call, void *data) {
    (void)origin; (void)args; (void)data;
    zebkit_view_on_authentication(view, NULL, NULL);
    zebkit_call_resolve(call, "true");
}

static void closed(zebkit_view *view, void *data) {
    (void)view;
    (void)data;
    zebkit_quit();
}

static void ready(void *data) {
    (void)data;
    zebkit_view *view = normal_view ? zebkit_view_new(NULL, 800, 600)
                                  : zebkit_view_new_offscreen(NULL, 800, 600, 1);
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
    if (host_ui) {
        zebkit_view_on_authentication(view, authentication, NULL);
        /* Deliberately test-only: grants apply only to the owned fixture's
         * localhost origin, never arbitrary websites or a real account. */
        zebkit_view_grant(view, url, ZEBKIT_ALLOW_APP, NULL);
        zebkit_view_bind(view, "authenticationState", state, NULL);
        zebkit_view_bind(view, "authenticationAction", action, NULL);
        zebkit_view_bind(view, "authenticationDetach", detach, NULL);
    }
    zebkit_view_on_close(view, closed, NULL);
    zebkit_view_load_url(view, url);
}

int main(int argc, char **argv) {
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--test-normal-view") == 0) {
            normal_view = 1;
            allow_ui = 1;
        } else if (strcmp(argv[i], "--test-host-ui") == 0) {
            host_ui = 1;
            allow_ui = 1;
            focused = 1;
        } else if (strcmp(argv[i], "--test-authentication-ui") == 0)
            allow_ui = 1;
        else if (strcmp(argv[i], "--test-focused") == 0)
            focused = 1;
        else if (strncmp(argv[i], "--", 2) != 0)
            url = argv[i];
    }
    return zebkit_main(argc, argv, ready, NULL);
}
