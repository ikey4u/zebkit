/* HTTP authentication through the C API, one mode an engine; run by
 * http_auth.py, which has the modes and the server (ZEBKIT_HTTP_AUTH_*), also
 * with the CentOS loader (scripts/glibc-test.sh). What only C has: a NULL
 * request, and the same function set again while a request waits. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zebkit/hooks.h>

static zebkit_view *view;
static zebkit_http_auth_request *saved;
static const char *mode, *user, *password;
static int failures, challenges, results, provided = -1, timed_out, title_ok, title_denied, load_failed,
    closed, acted, ticks, complete, changed;
#define IS(name) (!strcmp(mode, name))
static int held(void) {
    return IS("invalid") || IS("timeout") || IS("stop") || IS("navigation") || IS("remove") || IS("replace");
}

static void check(int ok, const char *message) {
    if (!ok) { ++failures; fprintf(stderr, "FAIL %s %s\n", mode, message); }
}
static int live(zebkit_http_auth_request *request) {
    char *info = zebkit_http_auth_request_info(request);
    int pending = info && strstr(info, "pending");
    zebkit_free(info);
    return pending;
}
static void another(zebkit_view *v, zebkit_http_auth_request *request, const char *json, void *data) {
    (void)v; (void)request; (void)json; (void)data;
}
static void challenge(zebkit_view *v, zebkit_http_auth_request *request, const char *json, void *data) {
    (void)data;
    saved = request;
    ++challenges;
    check(v == view && json && strstr(json, "challenger") && !strstr(json, password), "challenge metadata");
    check(live(request), "live request");
    check(!zebkit_http_auth_respond(request, "invalid", NULL), "a user without a password refused");
    if (held()) return;
    if (IS("cancel-handler")) {
        zebkit_view_on_http_auth(v, NULL, NULL);
        check(!live(request) && !zebkit_http_auth_respond(request, user, password), "removed: the request is gone");
        return;
    }
    if (IS("close-request")) { zebkit_view_close(v); return; }
    int allow = !IS("deny");
    check(zebkit_http_auth_respond(request, allow ? user : NULL, allow ? password : NULL), "single reply accepted");
    check(!zebkit_http_auth_respond(request, NULL, NULL), "second reply rejected");
    zebkit_view_on_http_auth(v, NULL, NULL);
}
static void outcome(zebkit_view *v, const char *json, void *data) {
    (void)data;
    check(json && !strstr(json, password), "no password in the outcome");
    if (json && strstr(json, "\"state\":\"provided\"")) provided = 1;
    else if (json && strstr(json, "\"state\":\"cancelled\"")) provided = 0;
    if (json && strstr(json, "\"reason\":\"timeout\"")) timed_out = 1;
    if (json && strstr(json, "\"reason\":\"handler-changed\"")) changed = 1;
    ++results;
    if (IS("close-result")) { zebkit_view_on_http_auth_result(v, NULL, NULL); zebkit_view_close(v); }
}
static void load(zebkit_view *v, zebkit_load_event event, const char *url, void *data) {
    (void)url; (void)data;
    if (event == ZEBKIT_LOAD_FAILED) load_failed = 1;
    if (event == ZEBKIT_LOAD_FINISHED) {
        char *title = zebkit_view_get_title(v);
        title_ok = title && !strcmp(title, "auth-ok");
        title_denied = title && !strcmp(title, "auth-denied");
        zebkit_free(title);
    }
}
static void on_close(zebkit_view *v, void *data) { (void)v; (void)data; closed = 1; }

/* The mode's own step once its request waits; whether the mode is done. */
static int act(void) {
    if (!acted) {
        acted = 1;
        if (IS("invalid")) {
            check(!zebkit_http_auth_respond(saved, "user", NULL), "a user alone refused");
            check(!zebkit_http_auth_respond(saved, "\xff", "password"), "a user that is not UTF-8 refused");
            static char long_user[65538];
            memset(long_user, 'x', sizeof long_user - 1);
            check(!zebkit_http_auth_respond(saved, long_user, "password"), "a user of 65537 bytes refused");
            check(!zebkit_http_auth_request_info(NULL) && !zebkit_http_auth_respond(NULL, NULL, NULL),
                  "a NULL request: no info, no reply");
            zebkit_view_on_http_auth(view, challenge, NULL);
            check(live(saved), "the same function set again keeps the request");
            check(zebkit_http_auth_respond(saved, user, password), "a valid reply after invalid ones");
        } else if (IS("stop")) {
            zebkit_view_stop(view);
        } else if (IS("navigation")) {
            zebkit_view_load_url(view, "about:blank");
        } else if (IS("remove")) {
            zebkit_view_on_http_auth(view, NULL, NULL);
        } else if (IS("replace")) {
            zebkit_view_on_http_auth(view, another, NULL);
        }
    }
    if (IS("invalid")) return title_ok;
    if (IS("timeout")) return !live(saved) && timed_out;
    if (IS("remove") || IS("replace")) return !live(saved) && changed;
    return !live(saved);
}
static int done(void) {
    if (IS("default")) return title_denied || load_failed;
    if (IS("close-request") || IS("close-result")) return closed;
    if (held()) return challenges && act();
    if (IS("allow") || IS("proxy")) return title_ok && results == 1;
    return (title_denied || load_failed) && results == 1;
}
static void poll(void *data) {
    (void)data;
    if (done() || ++ticks > 2000) {
        check(done(), "the mode finished");
        if (IS("default")) check(!challenges && !results, "without a function no challenge is told");
        if (IS("allow") || IS("proxy") || IS("close-result") || IS("invalid")) check(provided == 1, "credentials provided");
        if (IS("deny") || IS("cancel-handler")) check(provided == 0, "no credentials provided");
        if (saved) check(!live(saved) && !zebkit_http_auth_respond(saved, NULL, NULL), "stale request rejected");
        complete = 1;
        if (!closed) zebkit_view_close(view);
        zebkit_quit();
        return;
    }
    zebkit_post_delayed_task(poll, NULL, 10);
}
static void ready(void *data) {
    (void)data;
    char url[2048];
    view = zebkit_view_new(NULL, 320, 200);
    check(!zebkit_view_set_http_auth_timeout(view, 0) && !zebkit_view_set_http_auth_timeout(view, 300001),
          "invalid deadlines rejected");
    check(zebkit_view_set_http_auth_timeout(view, IS("timeout") ? 50 : 5000), "deadline accepted");
    if (!IS("default")) zebkit_view_on_http_auth(view, challenge, NULL);
    zebkit_view_on_http_auth_result(view, outcome, NULL);
    zebkit_view_on_load(view, load, NULL);
    zebkit_view_on_close(view, on_close, NULL);
    snprintf(url, sizeof url, "%s/auth/c-%s", getenv("ZEBKIT_HTTP_AUTH_URL"), mode);
    zebkit_view_load_url(view, url);
    poll(NULL);
}
int main(int argc, char **argv) {
    mode = getenv("ZEBKIT_HTTP_AUTH_CASE");
    user = getenv("ZEBKIT_HTTP_AUTH_USER");
    password = getenv("ZEBKIT_HTTP_AUTH_PASSWORD");
    if (!mode || !getenv("ZEBKIT_HTTP_AUTH_URL") || !user || !password) return 2;
    int status = zebkit_main(argc, argv, ready, NULL);
    if (status || failures || !complete) return 1;
    printf("ok C HTTP auth %s\n", mode);
    return 0;
}
