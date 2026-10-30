/* Real HTTP challenges through the public C ABI, including old-libc runs. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zebkit/hooks.h>

static zebkit_view *view;
static zebkit_http_auth_request *saved;
static int failures, replies, results, loaded, ticks, complete, allow;
static void check(int ok, const char *message) {
    if (!ok) { ++failures; fprintf(stderr, "FAIL %s\n", message); }
}
static void challenge(zebkit_view *v, zebkit_http_auth_request *request,
                      const char *json, void *data) {
    (void)data;
    saved = request;
    check(v == view && json && strstr(json, "challenger"), "challenge metadata");
    char *info = zebkit_http_auth_request_info(request);
    check(info && strstr(info, "pending"), "live token");
    zebkit_free(info);
    check(!zebkit_http_auth_respond(request, "invalid", NULL), "invalid pair rejected");
    check(zebkit_http_auth_respond(request,
          allow ? getenv("ZEBKIT_HTTP_AUTH_USER") : NULL,
          allow ? getenv("ZEBKIT_HTTP_AUTH_PASSWORD") : NULL), "single reply accepted");
    check(!zebkit_http_auth_respond(request, NULL, NULL), "second reply rejected");
    ++replies;
    zebkit_view_on_http_auth(v, NULL, NULL);
}
static void outcome(zebkit_view *v, const char *json, void *data) {
    (void)data;
    check(json && strstr(json, allow ? "\"provided\":true" : "\"provided\":false"), "reply outcome");
    ++results;
    zebkit_view_on_http_auth_result(v, NULL, NULL);
}
static void load(zebkit_view *v, zebkit_load_event event, const char *url, void *data) {
    (void)url; (void)data;
    if (event == ZEBKIT_LOAD_FINISHED) {
        char *title = zebkit_view_get_title(v);
        loaded = title && !strcmp(title, allow ? "auth-ok" : "auth-denied");
        zebkit_free(title);
    }
}
static void poll(void *data) {
    (void)data;
    if ((loaded && replies == 1 && results == 1) || ++ticks > 2000) {
        check(loaded && replies == 1 && results == 1, "challenge and document finish");
        char *info = zebkit_http_auth_request_info(saved);
        check(!info && !zebkit_http_auth_respond(saved, NULL, NULL), "stale token rejected");
        zebkit_free(info);
        complete = 1;
        zebkit_view_close(view); zebkit_quit();
        return;
    }
    zebkit_post_delayed_task(poll, NULL, 10);
}
static void ready(void *data) {
    (void)data;
    view = zebkit_view_new(NULL, 320, 200);
    check(!zebkit_view_set_http_auth_timeout(view, 0), "invalid deadline rejected");
    check(zebkit_view_set_http_auth_timeout(view, 5000), "deadline accepted");
    zebkit_view_on_http_auth(view, challenge, NULL);
    zebkit_view_on_http_auth_result(view, outcome, NULL);
    zebkit_view_on_load(view, load, NULL);
    zebkit_view_load_url(view, getenv("ZEBKIT_HTTP_AUTH_URL"));
    poll(NULL);
}
int main(int argc, char **argv) {
    const char *mode = getenv("ZEBKIT_HTTP_AUTH_CASE");
    if (!mode || !getenv("ZEBKIT_HTTP_AUTH_URL") ||
        !getenv("ZEBKIT_HTTP_AUTH_USER") || !getenv("ZEBKIT_HTTP_AUTH_PASSWORD")) return 2;
    allow = !strcmp(mode, "allow");
    int status = zebkit_main(argc, argv, ready, NULL);
    if (status || failures || !complete) return 1;
    printf("ok C HTTP auth %s\n", mode);
    return 0;
}
