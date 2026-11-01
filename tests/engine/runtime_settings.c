/* Runtime settings through the C API; run by runtime_settings.py. */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <zebkit/hooks.h>
#include <zebkit/page.h>

static const char *BASE;
#define FAIL(...) do { fprintf(stderr, "FAIL line %d: ", __LINE__); fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); exit(1); } while (0)
#define CHECK(c) do { if (!(c)) FAIL("%s", #c); } while (0)
#define WAIT(c) do { int t_ = 0; while (!(c)) { if (++t_ > 15000) FAIL("timed out: %s", #c); pump_once(); } } while (0)

static void pump_once(void) {
    CHECK(zebkit_pump());
    struct timespec nap = {0, 1000000};
    nanosleep(&nap, NULL);
}
static void pump_for(int ms) { for (int i = 0; i < ms; ++i) pump_once(); }

struct box { int done; char value[4096]; };
static void result(zebkit_view *view, const char *json, const char *error, void *data) {
    (void)view;
    struct box *box = data;
    if (error) FAIL("evaluate: %s", error);
    snprintf(box->value, sizeof box->value, "%s", json ? json : "");
    box->done = 1;
}
/* Its answer, while the page may wait: only for short strings. */
static const char *ev(zebkit_view *view, const char *script) {
    static struct box box;
    box.done = 0;
    zebkit_view_evaluate(view, script, result, &box);
    WAIT(box.done);
    return box.value;
}
static void loaded(zebkit_view *view, zebkit_load_event event, const char *url, void *data) {
    (void)view;
    /* Not the blank page a view begins with. */
    if (event == ZEBKIT_LOAD_FINISHED && url && strstr(url, "/top")) *(int *)data = 1;
}
static zebkit_view *opened_in(zebkit_context *context) {
    static int done[8];
    static int next;
    int *flag = &done[next++ % 8];
    *flag = 0;
    char url[1024];
    snprintf(url, sizeof url, "%s/top", BASE);
    zebkit_view *view = zebkit_view_new_in(context, NULL, 640, 480);
    zebkit_view_on_load(view, loaded, flag);
    zebkit_view_load_url(view, url);
    WAIT(*flag);
    return view;
}
static zebkit_view *opened(void) { return opened_in(NULL); }

static int names;
static void cookies_become(zebkit_view *view, const char *expected) {
    for (int i = 0; i < 60; ++i) {
        char script[128];
        snprintf(script, sizeof script, "probe('c%d')", ++names);
        if (!strcmp(ev(view, script), expected)) return;
        pump_for(50);
    }
    FAIL("third-party cookies never became %s", expected);
}
static int has(char *json, const char *text) {
    if (!json) FAIL("no settings");
    int found = strstr(json, text) != NULL;
    zebkit_free(json);
    return found;
}
static void ok(bool done, char *error) {
    if (!done) FAIL("refused: %s", error ? error : "");
}
static void set_engine(const char *json) {
    char *error = NULL;
    ok(zebkit_set_engine_settings(json, &error), error);
}
static void set_view(zebkit_view *view, const char *json) {
    char *error = NULL;
    ok(zebkit_view_set_runtime_settings(view, json, &error), error);
}
static void refused(zebkit_view *view, const char *json) {
    char *engine_before = zebkit_get_engine_settings(), *view_before = zebkit_view_get_runtime_settings(view);
    char *error = NULL;
    if (zebkit_set_engine_settings(json, &error)) FAIL("the engine took %s", json);
    CHECK(error);
    zebkit_free(error);
    error = NULL;
    if (zebkit_view_set_runtime_settings(view, json, &error)) FAIL("the view took %s", json);
    CHECK(error);
    zebkit_free(error);
    char *engine_after = zebkit_get_engine_settings(), *view_after = zebkit_view_get_runtime_settings(view);
    CHECK(!strcmp(engine_before, engine_after) && !strcmp(view_before, view_after));
    zebkit_free(engine_before); zebkit_free(view_before); zebkit_free(engine_after); zebkit_free(view_after);
}

static int complete, cancelled;
static void progress(zebkit_view *view, const char *json, void *data) {
    (void)view; (void)data;
    if (!strstr(json, "\"done\":true")) return;
    if (strstr(json, "\"state\":\"complete\"")) complete++;
    if (strstr(json, "\"state\":\"cancelled\"")) cancelled++;
}
static int asked;
static void ask(zebkit_view *view, zebkit_download_request *request, const char *json, void *data) {
    (void)view; (void)json; (void)data;
    asked++;
    zebkit_download_respond(request, ZEBKIT_DOWNLOAD_CANCEL, NULL, NULL);
}

int main(int argc, char **argv) {
    BASE = getenv("RUNTIME_BASE");
    if (zebkit_init(argc, argv)) return 1;
    zebkit_view *view = opened(), *other = opened();

    /* Today's behaviour until someone says otherwise. */
    char *start = zebkit_get_engine_settings();
    CHECK(strstr(start, "\"explicit\":{}") && strstr(start, "\"thirdPartyCookies\":\"block\"") &&
          strstr(start, "\"spareRenderer\":false") && strstr(start, "\"downloads\":\"save\"") &&
          strstr(start, "\"newWindows\":\"block-without-gesture\"") && strstr(start, "\"dialogs\":\"default\""));
    zebkit_free(start);

    /* A bad batch changes nothing. */
    set_view(view, "{\"dialogs\":\"accept\"}");
    refused(view, "not json");
    refused(view, "[]");
    refused(view, "{\"nothing\":1}");
    refused(view, "{\"thirdPartyCookies\":true}");
    refused(view, "{\"downloads\":\"deny\",\"newWindows\":\"nope\"}");
    refused(view, "{\"dialogs\":\"dismiss\",\"spareRenderer\":\"yes\"}");
    char *error = NULL;
    CHECK(!zebkit_set_engine_settings(NULL, &error) && error);
    zebkit_free(error);
    error = NULL;
    CHECK(!zebkit_view_set_runtime_settings(view, "{\"spareRenderer\":true}", &error) && error);
    zebkit_free(error);
    CHECK(!strcmp(ev(view, "confirm('still accepted')"), "true"));
    set_view(view, "{\"dialogs\":null}");

    /* Third-party cookies. */
    cookies_become(view, "\"none\"");
    set_engine("{\"thirdPartyCookies\":\"allow\"}");
    cookies_become(view, "\"cookie\"");
    CHECK(has(zebkit_view_get_runtime_settings(other), "\"thirdPartyCookies\":\"allow\""));
    set_engine("{\"thirdPartyCookies\":\"block\"}");
    cookies_become(view, "\"none\"");
    set_view(view, "{\"thirdPartyCookies\":\"allow\"}");
    cookies_become(other, "\"cookie\"");
    set_engine("{\"thirdPartyCookies\":null}");
    cookies_become(view, "\"none\"");
    /* A view's goes to its context, and no other; the engine's to both, and to one
     * made after it. */
    zebkit_context *context = zebkit_context_new();
    zebkit_view *apart = opened_in(context);
    cookies_become(apart, "\"none\"");
    set_view(apart, "{\"thirdPartyCookies\":\"allow\"}");
    cookies_become(apart, "\"cookie\"");
    CHECK(has(zebkit_view_get_runtime_settings(view), "\"thirdPartyCookies\":\"block\""));
    cookies_become(view, "\"none\"");
    set_engine("{\"thirdPartyCookies\":\"block\"}");
    cookies_become(apart, "\"none\"");
    set_engine("{\"thirdPartyCookies\":\"allow\"}");
    cookies_become(view, "\"cookie\"");
    zebkit_context *later = zebkit_context_new();
    zebkit_view *new_view = opened_in(later);
    cookies_become(new_view, "\"cookie\"");
    zebkit_view_close(new_view);
    zebkit_view_close(apart);
    zebkit_context_close(later);
    zebkit_context_close(context);
    set_engine("{\"thirdPartyCookies\":null}");
    cookies_become(view, "\"none\"");

    /* Dialogs, in two views; held, then answered. */
    zebkit_view_set_dialog_policy(view, ZEBKIT_DIALOG_POLICY_DISMISS);
    set_view(other, "{\"dialogs\":\"accept\"}");
    CHECK(!strcmp(ev(view, "confirm('q')"), "false"));
    CHECK(!strcmp(ev(other, "confirm('q')"), "true"));
    CHECK(has(zebkit_view_get_runtime_settings(view), "\"dialogs\":\"dismiss\""));
    set_view(view, "{\"dialogs\":\"hold\"}");
    static struct box held;
    zebkit_view_evaluate(view, "confirm('held')", result, &held);
    char *open = NULL;
    for (int i = 0; i < 5000; ++i) {
        open = zebkit_view_dialogs(view);
        if (strstr(open, "\"held\"")) break;
        zebkit_free(open);
        open = NULL;
        pump_once();
    }
    CHECK(open);
    const char *id = strstr(open, "\"id\":");
    CHECK(id && !held.done);
    CHECK(zebkit_view_dialog_respond(view, strtoull(id + 5, NULL, 10), true, NULL));
    zebkit_free(open);
    WAIT(held.done);
    CHECK(!strcmp(held.value, "true"));
    set_view(view, "{\"dialogs\":null}");
    set_view(other, "{\"dialogs\":null}");
    set_engine("{\"dialogs\":\"accept\"}");
    CHECK(zebkit_view_get_dialog_policy(view) == ZEBKIT_DIALOG_POLICY_ACCEPT);
    CHECK(!strcmp(ev(view, "confirm('q')"), "true"));
    set_engine("{\"dialogs\":null}");

    /* Downloads: "deny" and "save" with no function, and a function first. */
    zebkit_view_on_download_progress(view, progress, NULL);
    set_view(view, "{\"downloads\":\"deny\"}");
    ev(view, "save('denied.bin'); 1");
    WAIT(cancelled == 1);
    CHECK(complete == 0);
    zebkit_view_on_download(view, ask, NULL);
    CHECK(has(zebkit_view_get_runtime_settings(view), "\"downloads\":\"ask\""));
    ev(view, "save('denied-asked.bin'); 1");
    WAIT(asked == 1);
    zebkit_view_on_download(view, NULL, NULL);
    set_view(view, "{\"downloads\":null}");
    ev(view, "save('saved.bin'); 1");
    WAIT(complete == 1);

    /* New windows. A script of the application counts as a person's act. */
    set_view(view, "{\"newWindows\":\"block\"}");
    set_view(other, "{\"newWindows\":\"allow\"}");
    CHECK(!strcmp(ev(view, "opens()"), "false"));
    CHECK(!strcmp(ev(other, "opens()"), "true"));
    CHECK(zebkit_view_get_popup_policy(view) == ZEBKIT_POPUP_POLICY_BLOCK);
    set_view(view, "{\"newWindows\":null}");
    set_engine("{\"newWindows\":\"block\"}");
    CHECK(!strcmp(ev(view, "opens()"), "false"));
    CHECK(!strcmp(ev(other, "opens()"), "true"));
    set_engine("{\"newWindows\":null}");
    CHECK(!strcmp(ev(view, "opens()"), "true"));

    /* The spare: told, the processes are for runtime_settings_python.py. */
    set_engine("{\"spareRenderer\":true}");
    CHECK(has(zebkit_get_engine_settings(), "\"spareRendererRunning\":true"));
    set_engine("{\"spareRenderer\":false}");
    CHECK(has(zebkit_get_engine_settings(), "\"spareRendererRunning\":false"));
    set_engine("{\"spareRenderer\":null}");

    zebkit_view_close(view);
    zebkit_view_close(other);
    CHECK(!zebkit_view_get_runtime_settings(NULL));
    printf("ok c\n");
    fflush(stdout);
    zebkit_shutdown();
    return 0;
}
