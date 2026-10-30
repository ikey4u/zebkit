/* Real TLS policy through the public C ABI, also run with the CentOS loader. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zebkit/hooks.h>

static zebkit_view *allowed, *denied;
static int failures, replies, results, loaded, rejected, ticks, started, complete;

static void check(int ok, const char *message) {
    if (!ok) { ++failures; fprintf(stderr, "FAIL %s\n", message); }
}
static void certificate(zebkit_view *view, zebkit_certificate_request *request,
                        const char *json, void *data) {
    (void)data;
    check(view == allowed && json && strstr(json, "certificateChain"), "certificate metadata");
    char *info = zebkit_certificate_request_info(request);
    check(info && strstr(info, "pending"), "live request info");
    zebkit_free(info);
    check(zebkit_certificate_respond(request, true), "allow this request");
    check(!zebkit_certificate_respond(request, true), "reply is single use");
    ++replies;
    zebkit_view_on_certificate(view, NULL, NULL);
}
static void result(zebkit_view *view, const char *json, void *data) {
    (void)data;
    check(json && strstr(json, "continued"), "continued result");
    ++results;
    zebkit_view_on_certificate_result(view, NULL, NULL);
}
static void load(zebkit_view *view, zebkit_load_event event, const char *url, void *data) {
    (void)url; (void)data;
    if (view == allowed && event == ZEBKIT_LOAD_FINISHED) {
        char *title = zebkit_view_get_title(view);
        loaded = title && !strcmp(title, "certificate-ok");
        zebkit_free(title);
    }
    if (view == denied && event == ZEBKIT_LOAD_FAILED) rejected = 1;
}
static void poll(void *data) {
    (void)data;
    if (loaded && replies == 1 && results == 1 && !started) {
        char url[1024];
        started = 1;
        snprintf(url, sizeof(url), "%s/c-denied", getenv("ZEBKIT_CERTIFICATE_TEST_URL"));
        zebkit_view_load_url(denied, url);
    }
    if ((started && rejected) || ++ticks > 2000) {
        check(loaded && replies == 1 && results == 1 && rejected, "allowed document and isolated denial finish");
        complete = 1;
        zebkit_view_close(allowed); zebkit_view_close(denied); zebkit_quit();
        return;
    }
    zebkit_post_delayed_task(poll, NULL, 10);
}
static void ready(void *data) {
    (void)data;
    allowed = zebkit_view_new(NULL, 320, 200);
    denied = zebkit_view_new(NULL, 320, 200);
    zebkit_view_on_certificate(allowed, certificate, NULL);
    zebkit_view_on_certificate_result(allowed, result, NULL);
    zebkit_view_on_load(allowed, load, NULL);
    zebkit_view_on_load(denied, load, NULL);
    char url[1024];
    snprintf(url, sizeof(url), "%s/c-allowed", getenv("ZEBKIT_CERTIFICATE_TEST_URL"));
    zebkit_view_load_url(allowed, url);
    poll(NULL);
}
int main(int argc, char **argv) {
    if (!getenv("ZEBKIT_CERTIFICATE_TEST_URL")) return 2;
    int status = zebkit_main(argc, argv, ready, NULL);
    if (status || failures || !complete) return 1;
    puts("ok C certificate policy: TLS allowed once and another view denied");
    return 0;
}
