/* Commands sent from inside the callback of a navigation that ended, while other
 * navigations wait: the list of waiters is being walked as one joins. Here after a
 * renderer's kill, where it crashed the process. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zebkit/automation.h>
#include <zebkit/offscreen.h>

static zebkit_view *view;
static int pending, phase, failures;

static void step(zebkit_view *v, const char *json, const char *error, void *data);

static void go(const char *url) {
    char params[512];
    /* Every call is answered: by its load, or by the time limit. */
    snprintf(params, sizeof params, "{\"url\": \"%s\", \"waitUntil\": \"load\", \"timeout\": 4000}", url);
    ++pending;
    zebkit_automation_call(view, "Frame.goto", params, step, NULL);
}

static void alive(zebkit_view *v, const char *json, const char *error, void *data) {
    (void)v; (void)data;
    int ok = !error && json && strstr(json, "2");
    printf("%s the engine answers afterwards\n", ok ? "ok  " : "FAIL");
    printf("%d of 1 checks passed\n", ok);
    failures = !ok;
    fflush(stdout);
    zebkit_quit();
}

static void step(zebkit_view *v, const char *json, const char *error, void *data) {
    (void)v; (void)json; (void)data;
    --pending;
    printf("answered, %s, %d waiting\n", error ? "failed" : "loaded", pending);
    fflush(stdout);
    if (!phase) {
        /* A page there: the renderer killed, with another waiting beside it. */
        phase = 1;
        go("chrome://crash");
        go("chrome://crash");
    } else if (error && phase < 5) {
        /* From inside the walk of the waiters. */
        ++phase;
        go("data:text/html,<title>a</title>");
        go("data:text/html,<title>b</title>");
    } else if (!pending) {
        zebkit_automation_call(view, "Frame.evaluateExpression", "{\"expression\": \"1 + 1\"}", alive, NULL);
    }
}

static void ready(void *data) {
    (void)data;
    view = zebkit_view_new_offscreen(NULL, 400, 300, 1);
    if (!view) { failures = 1; zebkit_quit(); return; }
    go("data:text/html,<title>first</title>");
}

int main(int argc, char **argv) {
    int code = zebkit_main(argc, argv, ready, NULL);
    return code || failures;
}
