/* Off-screen rendering for a human-facing host must not, by itself,
 * enable Chromium's automation flag. Explicit automation switches must
 * still do so. Run against a real engine, including its child renderer. */

#include "harness.h"
#include <stdio.h>
#include <string.h>

#include <zebkit/zebkit.h>
#include <zebkit/offscreen.h>

static int expect_automation;

static void evaluated(zebkit_view *view, const char *json, const char *error, void *data) {
    (void)view, (void)error, (void)data;
    check(json && strcmp(json, expect_automation ? "true" : "false") == 0,
          "navigator.webdriver agrees with the explicit automation mode", NULL);
    zebkit_quit();
}

static void loaded(zebkit_view *view, zebkit_load_event event, const char *url, void *data) {
    (void)url, (void)data;
    if (event == ZEBKIT_LOAD_FINISHED) {
        zebkit_view_on_load(view, NULL, NULL);
        zebkit_view_evaluate(view, "navigator.webdriver", evaluated, NULL);
    }
}

static void ready(void *data) {
    (void)data;
    check(zebkit_is_headless(), "the engine creates no native windows", NULL);
    char *error = NULL;
    check(zebkit_cdp_listen("127.0.0.1", 0, &error) != 0,
          "the host can serve CDP through the API", NULL);
    zebkit_free(error);
    zebkit_view *view = zebkit_view_new_offscreen(NULL, 800, 600, 1.0);
    check(view != NULL, "the host can create an off-screen view", NULL);
    if (!view) {
        zebkit_quit();
        return;
    }
    zebkit_view_on_load(view, loaded, NULL);
    zebkit_view_load_url(view, "data:text/html,<title>rendering mode</title>");
}

int main(int argc, char **argv) {
    for (int i = 1; i < argc; ++i)
        if (strcmp(argv[i], "--expect-automation") == 0)
            expect_automation = 1;
    int result = zebkit_main(argc, argv, ready, NULL);
    check(result == 0, "zebkit_main returned 0", "%d", result);
    return summary();
}
