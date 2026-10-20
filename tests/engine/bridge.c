/* zebkit.* in a page: what the application grants and binds
 * (zebkit_view_grant, zebkit_view_bind), used by the page's script
 * (pages/bridge.html), refused to a page of another origin; and
 * zebkit_fetch() from C. Needs the pages served:
 *
 *   ZEBKIT_TEST_BASE    http://127.0.0.1:<port>, serving tests/engine/pages
 *   ZEBKIT_TEST_OTHER   the same pages at another origin, http://localhost:<port>
 *   ZEBKIT_TEST_FILES   an empty directory for zebkit.fs
 *
 * scripts/engine-test.sh sets them. Exit 0 when every check passes. */

#define _GNU_SOURCE /* strcasestr */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zebkit/services.h>
#include <zebkit/zebkit.h>

static int failures, checks;
static char base[256], other[256], files[1024];

static void check(int ok, const char *what, const char *detail) {
    ++checks;
    if (!ok)
        ++failures;
    printf("%s %s%s%s\n", ok ? "ok  " : "FAIL", what, detail ? ": " : "", detail ? detail : "");
    fflush(stdout);
}

/* zebkit.app.add(a, b) */
static void add(zebkit_view *view, const char *origin, const char *args, zebkit_call *call, void *data) {
    (void)view, (void)data;
    double a = 0, b = 0;
    if (sscanf(args, "[%lf,%lf]", &a, &b) != 2 || strcmp(origin, base) != 0) {
        zebkit_call_reject(call, "add takes two numbers");
        return;
    }
    char result[64];
    snprintf(result, sizeof(result), "%g", a + b);
    zebkit_call_resolve(call, result);
}

/* zebkit.app.later("x"): answered from a task, after the call returned. */
struct later {
    zebkit_call *call;
    char text[64];
};

static void answer_later(void *data) {
    struct later *later = data;
    char result[80];
    snprintf(result, sizeof(result), "\"%s!\"", later->text);
    zebkit_call_resolve(later->call, result);
    free(later);
}

static void later(zebkit_view *view, const char *origin, const char *args, zebkit_call *call, void *data) {
    (void)view, (void)origin, (void)data;
    struct later *pending = calloc(1, sizeof(*pending));
    pending->call = call;
    sscanf(args, "[\"%63[^\"]\"]", pending->text);
    zebkit_post_delayed_task(answer_later, pending, 50);
}

static void fail(zebkit_view *view, const char *origin, const char *args, zebkit_call *call, void *data) {
    (void)view, (void)origin, (void)args, (void)data;
    zebkit_call_reject(call, "no");
}

static zebkit_view *granted, *stranger;

static void fetched(const zebkit_response *response, void *data) {
    (void)data;
    check(response->status == 200 && response->body_size == 6 && memcmp(response->body, "hello\n", 6) == 0,
          "zebkit_fetch from C", response->error ? response->error : response->url);
    check(strcasestr(response->headers, "content-type: text/plain") != NULL, "zebkit_fetch: response headers",
          response->headers);
    zebkit_quit();
}

static void refused(zebkit_view *view, const char *json, const char *error, void *data) {
    (void)view, (void)data;
    check(json && strstr(json, "is not allowed for http://localhost:"), "a page of another origin is refused",
          json ? json : error);
    char url[300];
    snprintf(url, sizeof(url), "%s/hello.txt", base);
    zebkit_fetch(NULL, url, "Accept: text/plain", NULL, 0, fetched, NULL);
}

static void on_stranger_load(zebkit_view *view, zebkit_load_event event, const char *url, void *data) {
    (void)url, (void)data;
    if (event == ZEBKIT_LOAD_FINISHED)
        zebkit_view_evaluate(view, "zebkit.app.add(1, 2).then(() => 'resolved', (e) => e.message)", refused, NULL);
}

static void results(zebkit_view *view, const char *json, const char *error, void *data) {
    (void)view, (void)data;
    if (!json) {
        check(0, "runBridgeTests()", error);
    } else {
        /* {"name": true, ...}: each value that is not true failed. */
        printf("     %s\n", json);
        const char *at = json;
        int total = 0, bad = 0;
        while ((at = strstr(at, "\":"))) {
            at += 2;
            ++total;
            if (strncmp(at, "true", 4) != 0)
                ++bad;
        }
        char summary[64];
        snprintf(summary, sizeof(summary), "%d of %d", total - bad, total);
        check(total >= 15 && bad == 0, "zebkit.* from the granted page", summary);
    }
    char url[300];
    snprintf(url, sizeof(url), "%s/bridge.html", other);
    /* Granted for the first origin, showing a page of another. */
    stranger = zebkit_view_new(url, 0, 0);
    zebkit_view_grant(stranger, base, ZEBKIT_ALLOW_ALL, files);
    zebkit_view_bind(stranger, "add", add, NULL);
    zebkit_view_on_load(stranger, on_stranger_load, NULL);
}

static int ran;
static void on_load(zebkit_view *view, zebkit_load_event event, const char *url, void *data) {
    (void)data;
    if (event == ZEBKIT_LOAD_FINISHED && !ran++)
        zebkit_view_evaluate(view, "runBridgeTests()", results, NULL);
    else if (event == ZEBKIT_LOAD_FAILED)
        check(0, "loading the page", url);
}

static void ready(void *data) {
    (void)data;
    char url[300];
    snprintf(url, sizeof(url), "%s/bridge.html", base);
    granted = zebkit_view_new(url, 0, 0);
    zebkit_view_grant(granted, base, ZEBKIT_ALLOW_ALL, files);
    zebkit_view_bind(granted, "add", add, NULL);
    zebkit_view_bind(granted, "later", later, NULL);
    zebkit_view_bind(granted, "fail", fail, NULL);
    zebkit_view_on_load(granted, on_load, NULL);
}

int main(int argc, char **argv) {
    const char *b = getenv("ZEBKIT_TEST_BASE"), *o = getenv("ZEBKIT_TEST_OTHER"), *f = getenv("ZEBKIT_TEST_FILES");
    if (!b || !o || !f) {
        fprintf(stderr, "set ZEBKIT_TEST_BASE, ZEBKIT_TEST_OTHER and ZEBKIT_TEST_FILES\n");
        return 2;
    }
    snprintf(base, sizeof(base), "%s", b);
    snprintf(other, sizeof(other), "%s", o);
    snprintf(files, sizeof(files), "%s", f);
    zebkit_main(argc, argv, ready, NULL);
    printf("%d of %d checks passed\n", checks - failures, checks);
    return failures || checks < 4 ? 1 : 0;
}
