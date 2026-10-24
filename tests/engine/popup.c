/* A site's sign-in through a popup, as "Sign in with Google" does it: the
 * page opens the provider's page from the person's act, the application
 * gets it as a view (zebkit_view_on_window_open()), off-screen as its opener is,
 * and the two stay linked: the provider posts to window.opener and closes,
 * and the site sees both. A popup from a timer is blocked. Prints one line
 * per check; scripts/engine-test.sh runs it with --headless. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zebkit/offscreen.h>
#include <zebkit/zebkit.h>

static int failures;
static int checks;
static zebkit_view *view;
static zebkit_view *popup;
static int popups;
static int popup_closed;
static char base[256];
static char provider[300];

static void check(int ok, const char *what, const char *detail) {
    ++checks;
    if (!ok)
        ++failures;
    printf("%s %s%s%s\n", ok ? "ok  " : "FAIL", what, detail ? ": " : "", detail ? detail : "");
    fflush(stdout);
}

static void on_view_close(zebkit_view *closed, void *data) {
    (void)closed, (void)data;
    zebkit_quit();
}

static void after_result(zebkit_view *v, const char *json, const char *error, void *data) {
    (void)v, (void)data;
    check(json && strstr(json, "\"message\":{\"code\":\"from the provider\""), "the provider's postMessage() to its opener",
          json ? json : error);
    check(json && strstr(json, "\"origin\":\"http://localhost:"), "from the provider's own origin", json ? json : error);
    check(json && strstr(json, "\"size\":[420,520]"), "the popup the size the page asked for", json ? json : error);
    check(json && strstr(json, "\"closed\":true"), "the site sees the popup closed", json ? json : error);
    check(json && strstr(json, "\"fromTimer\":false"), "a popup from a timer is blocked", json ? json : error);
    check(popups == 1, "one popup given to the application", NULL);
    zebkit_view_close(view);
}

static void read_result(void *data) {
    (void)data;
    zebkit_view_evaluate(view, "window.result", after_result, NULL);
}

static void on_popup_close(zebkit_view *closed, void *data) {
    (void)data;
    check(closed == popup, "window.close() closes the popup's view", NULL);
    popup_closed = 1;
    popup = NULL;
    zebkit_post_delayed_task(read_result, NULL, 300);
}

static void on_window_open(zebkit_view *opener, zebkit_view *made, const char *url, void *data) {
    (void)data;
    popups++;
    popup = made;
    check(opener == view && made != NULL && made != view, "zebkit_view_on_window_open: a view of its own", NULL);
    check(url && strstr(url, "/provider.html"), "with the URL the page opened", url);
    check(made && zebkit_view_is_offscreen(made), "off the screen, as its opener is", NULL);
    if (made)
        zebkit_view_on_close(made, on_popup_close, NULL);
}

static void after_open(zebkit_view *v, const char *json, const char *error, void *data) {
    (void)v, (void)data;
    check(json && strcmp(json, "true") == 0, "the application's script opens it", json ? json : error);
}

static void on_load(zebkit_view *v, zebkit_load_event event, const char *url, void *data) {
    (void)v, (void)url, (void)data;
    if (event != ZEBKIT_LOAD_FINISHED)
        return;
    zebkit_view_on_load(view, NULL, NULL);
    char script[512];
    snprintf(script, sizeof(script), "openSignIn('%s')", provider);
    zebkit_view_evaluate(view, script, after_open, NULL);
}

static void ready(void *data) {
    (void)data;
    const char *env = getenv("ZEBKIT_TEST_BASE");
    snprintf(base, sizeof(base), "%s", env ? env : "http://127.0.0.1:8796");
    /* The provider on another origin, as accounts.google.com is to a site. */
    const char *port = strrchr(base, ':');
    snprintf(provider, sizeof(provider), "http://localhost%s/provider.html", port ? port : ":8796");
    char url[300];
    snprintf(url, sizeof(url), "%s/signin.html", base);
    view = zebkit_view_new_offscreen(url, 640, 480, 1);
    check(view != NULL, "an off-screen view", NULL);
    if (!view) {
        zebkit_quit();
        return;
    }
    zebkit_view_on_window_open(view, on_window_open, NULL);
    zebkit_view_on_load(view, on_load, NULL);
    zebkit_view_on_close(view, on_view_close, NULL);
}

int main(int argc, char **argv) {
    int code = zebkit_main(argc, argv, ready, NULL);
    if (code)
        return code;
    printf("%d of %d checks passed\n", checks - failures, checks);
    return failures || checks < 12 ? 1 : 0;
}
