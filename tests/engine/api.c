/* The engine's C API, used as an application uses it: start the engine,
 * serve CDP, load a page, run script in it, follow its events, post tasks
 * from another thread, close the view and quit. Prints one line per check
 * and exits 0 when all of them pass. scripts/engine-test.sh runs it. */

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zebkit/zebkit.h>

static int failures;
static int checks;

static void check(int ok, const char *what, const char *detail) {
    ++checks;
    if (!ok)
        ++failures;
    printf("%s %s%s%s\n", ok ? "ok  " : "FAIL", what, detail ? ": " : "", detail ? detail : "");
    fflush(stdout);
}

static zebkit_view *view;
static int loaded;
static int titled;
static int posted;

static void finish(void *data) {
    (void)data;
    check(posted == 1, "zebkit_post_task from another thread", NULL);
    zebkit_view_close(view);
}

static void on_close(zebkit_view *closed, void *data) {
    (void)data;
    check(closed == view, "on_close", NULL);
    view = NULL;
    zebkit_quit();
}

static void from_thread(void *data) {
    (void)data;
    ++posted;
}

static void *thread_main(void *data) {
    (void)data;
    zebkit_post_task(from_thread, NULL);
    return NULL;
}

static void after_error(zebkit_view *v, const char *json, const char *error, void *data);
static void after_title(zebkit_view *v, const char *json, const char *error, void *data);
static void after_promise(zebkit_view *v, const char *json, const char *error, void *data);

static void after_capture(zebkit_view *v, const uint8_t *png, size_t size, const char *error, void *data) {
    (void)v, (void)data;
    char detail[64];
    snprintf(detail, sizeof(detail), "%zu bytes", size);
    check(png && size > 100 && memcmp(png, "\x89PNG", 4) == 0, "zebkit_view_capture: a PNG", error ? error : detail);
    check(!zebkit_view_is_loading(view), "zebkit_view_is_loading", NULL);
    pthread_t thread;
    pthread_create(&thread, NULL, thread_main, NULL);
    pthread_join(thread, NULL);
    zebkit_post_delayed_task(finish, NULL, 200);
}

static void after_error(zebkit_view *v, const char *json, const char *error, void *data) {
    (void)v, (void)data;
    check(json == NULL && error && strstr(error, "boom"), "evaluate: an exception", error);
    zebkit_view_evaluate(view, "document.title = 'second'", after_title, NULL);
}

static void after_promise(zebkit_view *v, const char *json, const char *error, void *data) {
    (void)v, (void)data;
    check(json && strcmp(json, "{\"a\":[1,\"x\"]}") == 0, "evaluate: a promise", json ? json : error);
    zebkit_view_evaluate(view, "throw new Error('boom')", after_error, NULL);
}

static void after_sum(zebkit_view *v, const char *json, const char *error, void *data) {
    (void)v, (void)data;
    check(json && strcmp(json, "3") == 0, "evaluate: 1 + 2", json ? json : error);
    zebkit_view_evaluate(view, "new Promise(r => setTimeout(() => r({a: [1, 'x']}), 50))", after_promise, NULL);
}

static void after_added(zebkit_view *v, const char *json, const char *error, void *data) {
    (void)v, (void)data;
    check(json && strcmp(json, "[\"yes\",\"zebkit-test/1\"]") == 0,
          "zebkit_view_add_script before the page's, and zebkit_view_set_user_agent", json ? json : error);
    zebkit_view_capture(view, after_capture, NULL);
}

static void after_title(zebkit_view *v, const char *json, const char *error, void *data) {
    (void)v, (void)json, (void)data;
    check(error == NULL, "evaluate: assignment", error);
    char *url = zebkit_view_get_url(view);
    check(url && strncmp(url, "data:text/html", 14) == 0, "zebkit_view_get_url", url);
    zebkit_free(url);
    /* A script for every new document, and another User-Agent, then a load. */
    zebkit_view_add_script(view, "window.__added = 'yes'");
    zebkit_view_set_user_agent(view, "zebkit-test/1");
    /* The page's own first script sees what the added one set. */
    zebkit_view_load_url(view, "data:text/html,<script>window.__seen = window.__added</script><p>third</p>");
}

static void on_load(zebkit_view *v, zebkit_load_event event, const char *url, void *data) {
    (void)v, (void)data;
    if (event == ZEBKIT_LOAD_FINISHED && loaded == 1) {
        loaded++;
        zebkit_view_evaluate(view, "[window.__seen, navigator.userAgent]", after_added, NULL);
        return;
    }
    if (event == ZEBKIT_LOAD_FINISHED && !loaded++) {
        check(1, "on_load: finished", url);
        zebkit_view_evaluate(view, "1 + 2", after_sum, NULL);
    } else if (event == ZEBKIT_LOAD_FAILED) {
        check(0, "on_load", url);
        zebkit_quit();
    }
}

static void on_title(zebkit_view *v, const char *title, void *data) {
    (void)v, (void)data;
    if (strcmp(title, "second") == 0 && !titled++)
        check(1, "on_title", title);
}

static void ready(void *data) {
    (void)data;
    check(strncmp(zebkit_engine_version(), "Chromium ", 9) == 0, "zebkit_engine_version", zebkit_engine_version());
    char *error = NULL;
    unsigned port = zebkit_cdp_listen(NULL, 0, &error);
    check(port > 0 && port == zebkit_cdp_get_port(), "zebkit_cdp_listen", error);
    zebkit_free(error);
    char *url = zebkit_cdp_get_url();
    check(url && strncmp(url, "ws://127.0.0.1:", 15) == 0 && strstr(url, "/devtools/browser/"), "zebkit_cdp_get_url", url);
    /* For the script that runs this, which checks the endpoint over HTTP. */
    if (url && getenv("ZEBKIT_TEST_CDP_FILE")) {
        FILE *file = fopen(getenv("ZEBKIT_TEST_CDP_FILE"), "w");
        if (file) {
            fprintf(file, "%u\n", port);
            fclose(file);
        }
    }
    zebkit_free(url);
    view = zebkit_view_new("data:text/html,<title>first</title><p>hello</p>", 800, 600);
    check(view != NULL, "zebkit_view_new", NULL);
    zebkit_view_on_load(view, on_load, NULL);
    zebkit_view_on_title(view, on_title, NULL);
    zebkit_view_on_close(view, on_close, NULL);
}

int main(int argc, char **argv) {
    int code = zebkit_main(argc, argv, ready, NULL);
    check(code == 0, "zebkit_main returned 0", NULL);
    check(titled == 1, "the title changed", NULL);
    printf("%d of %d checks passed\n", checks - failures, checks);
    return failures ? 1 : 0;
}
