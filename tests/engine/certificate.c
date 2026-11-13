/* A document's TLS errors through the C API, one mode an engine; run by
 * certificate.py, which has the modes and the server (ZEBKIT_CERTIFICATE_*),
 * also with the CentOS loader (scripts/glibc-test.sh). */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zebkit/hooks.h>

static zebkit_view *view, *other;
static zebkit_certificate_request *saved;
static const char *mode;
static char url[1024], other_url[1100];
static int failures, requests, results, continued, answered, changed, title_ok, failed, other_failed,
    closed, started, ticks, complete;
#define IS(name) (!strcmp(mode, name))

static void check(int ok, const char *message) {
    if (!ok) { ++failures; fprintf(stderr, "FAIL %s %s\n", mode, message); }
}
static int live(zebkit_certificate_request *request) {
    char *info = zebkit_certificate_request_info(request);
    int pending = info && strstr(info, "pending");
    zebkit_free(info);
    return pending;
}
static void certificate(zebkit_view *v, zebkit_certificate_request *request, const char *json, void *data) {
    (void)data;
    saved = request;
    ++requests;
    check(v == view && json && strstr(json, "certificateChain"), "certificate metadata");
    check(live(request), "live request info");
    if (IS("strict")) {
        check(strstr(json, "\"strictEnforcement\":true") && strstr(json, "\"canIgnore\":false"), "a pinned host: strict");
        check(!zebkit_certificate_respond(request, true) && live(request), "its error cannot be allowed");
        check(zebkit_certificate_respond(request, false), "only refused");
        return;
    }
    if (IS("cancel-handler")) {
        zebkit_view_on_certificate(v, NULL, NULL);
        check(!live(request) && !zebkit_certificate_respond(request, true), "removed: the request is gone");
        return;
    }
    if (IS("close-request")) { zebkit_view_close(v); return; }
    int allow = IS("allow");
    /* A token the engine never gave (the request's own, moved): no info, no
     * reply, and the real request still waits. */
    zebkit_certificate_request *forged = (zebkit_certificate_request *)((uintptr_t)request + 0x10001);
    char *forged_info = zebkit_certificate_request_info(forged);
    check(!forged_info && !zebkit_certificate_respond(forged, allow) && live(request),
          "a forged request: no info, no reply, the real one still pending");
    zebkit_free(forged_info);
    check(zebkit_certificate_respond(request, allow), "the reply");
    check(!zebkit_certificate_respond(request, !allow), "reply is single use");
    zebkit_view_on_certificate(v, NULL, NULL);
}
static void result(zebkit_view *v, const char *json, void *data) {
    (void)data;
    ++results;
    if (json && strstr(json, "continued")) continued = 1;
    if (json && strstr(json, "\"reason\":\"answered\"")) answered = 1;
    if (json && strstr(json, "\"reason\":\"handler-changed\"")) changed = 1;
    if (IS("close-result")) { zebkit_view_on_certificate_result(v, NULL, NULL); zebkit_view_close(v); }
}
static void load(zebkit_view *v, zebkit_load_event event, const char *where, void *data) {
    (void)where; (void)data;
    if (v == view && event == ZEBKIT_LOAD_FINISHED) {
        char *title = zebkit_view_get_title(v);
        title_ok = title && !strcmp(title, "certificate-ok");
        zebkit_free(title);
    }
    if (event == ZEBKIT_LOAD_FAILED) *(v == view ? &failed : &other_failed) = 1;
}
static void on_close(zebkit_view *v, void *data) { (void)v; (void)data; closed = 1; }
static int done(void) {
    if (IS("allow")) {
        /* Once only: another view of the profile is refused, without being asked. */
        if (title_ok && continued && !started) { started = 1; zebkit_view_load_url(other, other_url); }
        return started && other_failed;
    }
    if (IS("close-request") || IS("close-result")) return closed;
    if (IS("default")) return failed;
    if (IS("cancel-handler")) return failed && changed;
    return failed && answered;
}
static void poll(void *data) {
    (void)data;
    if (done() || ++ticks > 2000) {
        check(done(), "the mode finished");
        check(IS("default") ? !requests : requests == 1, "asked once, or never without a function");
        if (saved) check(!live(saved) && !zebkit_certificate_respond(saved, false), "stale request rejected");
        complete = 1;
        if (!closed) zebkit_view_close(view);
        zebkit_view_close(other);
        zebkit_quit();
        return;
    }
    zebkit_post_delayed_task(poll, NULL, 10);
}
static void ready(void *data) {
    (void)data;
    view = zebkit_view_new(NULL, 320, 200);
    other = zebkit_view_new(NULL, 320, 200);
    check(!zebkit_view_set_certificate_timeout(view, 0) && !zebkit_view_set_certificate_timeout(view, 300001),
          "invalid deadlines rejected");
    check(zebkit_view_set_certificate_timeout(view, 5000), "deadline accepted");
    check(!zebkit_certificate_request_info(NULL) && !zebkit_certificate_respond(NULL, true),
          "a NULL request: no info, no reply");
    if (!IS("default")) zebkit_view_on_certificate(view, certificate, NULL);
    zebkit_view_on_certificate_result(view, result, NULL);
    zebkit_view_on_load(view, load, NULL);
    zebkit_view_on_load(other, load, NULL);
    zebkit_view_on_close(view, on_close, NULL);
    zebkit_view_load_url(view, url);
    poll(NULL);
}
int main(int argc, char **argv) {
    const char *base = getenv("ZEBKIT_CERTIFICATE_TEST_URL");
    mode = getenv("ZEBKIT_CERTIFICATE_CASE");
    if (!base || !mode) return 2;
    snprintf(url, sizeof url, "%s/c-%s", base, mode);
    snprintf(other_url, sizeof other_url, "%s-other", url);
    int status = zebkit_main(argc, argv, ready, NULL);
    if (status || failures || !complete) return 1;
    printf("ok C certificate policy %s\n", mode);
    return 0;
}
