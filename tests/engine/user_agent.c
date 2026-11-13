/* A view's user agent and client hints through the C API
 * (zebkit_view_set_user_agent_override), one mode an engine; run by
 * user_agent.py, which has the modes and the server (ZEBKIT_UA_*). */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zebkit/zebkit.h>

#define VALID "{\"userAgent\":\"zebkit-binding-UA/1\",\"acceptLanguage\":\"en-US\",\"platform\":\"NativePlatform\"," \
    "\"userAgentMetadata\":{\"brands\":[{\"brand\":\"Native\",\"version\":\"1\"}],\"platform\":\"Linux\"," \
    "\"platformVersion\":\"1\",\"architecture\":\"x86\",\"model\":\"\",\"mobile\":false}}"
/* What the page sees of it: true when all of it is the override's. */
#define STATE "navigator.userAgentData.getHighEntropyValues(['platformVersion','architecture']).then(h => " \
    "navigator.userAgent === 'zebkit-binding-UA/1' && navigator.platform === 'NativePlatform' && " \
    "h.architecture === 'x86' && h.platformVersion === '1' && " \
    "navigator.userAgentData.brands.some(b => b.brand === 'Native'))"
static const char *INVALID[] = {
    "{\"userAgent\":\"bad\\r\\nagent\"}", "{\"userAgent\":42}", "{\"userAgent\":\"changed\",\"unknown\":true}", "{",
};
enum { COUNT = sizeof INVALID / sizeof *INVALID };

static zebkit_view *view;
static const char *mode;
static int failures, answered, ok_answers, state_true, state_answers, loaded, ticks, complete;

static void check(int ok, const char *message) {
    if (!ok) { ++failures; fprintf(stderr, "FAIL %s %s\n", mode, message); }
}
/* zebkit.h's rule: on a live view, on the engine's thread, the call returns
 * true and func is told exactly once, after it returns; an invalid override
 * is told as an error. False, with func never called, only for a NULL or
 * closed view or off the engine's thread. Each invalid one: whether it was
 * queued, and its error answers and its other answers. */
static int invalid_queued[COUNT], invalid_errors[COUNT], invalid_others[COUNT];
/* Calls that must return false: their answers, which must never come. */
enum { REFUSED_NULL = 100, REFUSED_CLOSED };
static int refused_answers;
/* Inside a call of zebkit_view_set_user_agent_override(), and answers told
 * there (none may be: bad characters, refused by the session at once, too). */
static int in_call, answers_inside;
static void completed(zebkit_view *v, const char *json, const char *error, void *data) {
    (void)v;
    if (in_call) ++answers_inside;
    intptr_t index = (intptr_t)data;
    if (index >= REFUSED_NULL) { ++refused_answers; return; }
    if (index) {
        if (error && !json) ++invalid_errors[index - 1]; else ++invalid_others[index - 1];
        return;
    }
    ++answered;
    if (!error && json && !strcmp(json, "{}")) ++ok_answers;
}
static int settled(int index) { return invalid_errors[index] + invalid_others[index] > 0; }
static void evaluated(zebkit_view *v, const char *json, const char *error, void *data) {
    (void)v; (void)data;
    check(!error, "the page's state evaluated");
    if (json && !strcmp(json, "true")) ++state_true;
    ++state_answers;
}
static void load(zebkit_view *v, zebkit_load_event event, const char *url, void *data) {
    (void)v; (void)url; (void)data;
    if (event == ZEBKIT_LOAD_FINISHED) loaded = 1;
}
/* The steps: the override with the first load, then (rejection) each invalid
 * one in turn; the state read after each; then (rejection) the calls that
 * must return false, and a while for any late or second answer. */
static int started, sent, evaluating, draining, other_closed;
static zebkit_view *closed_view;
static void on_other_close(zebkit_view *v, void *data) { (void)v; (void)data; other_closed = 1; }
static void poll(void *data) {
    (void)data;
    int invalid = !strcmp(mode, "rejection");
    if (!started) {
        if (answered == 1 && loaded) {
            check(ok_answers == 1, "the valid override completed with {}");
            started = evaluating = 1;
            zebkit_view_evaluate(view, STATE, evaluated, NULL);
        }
    } else if (draining == 1) {
        /* zebkit_view_close() posts the closing: the view is closed once told. */
        if (other_closed) {
            check(!zebkit_view_set_user_agent_override(closed_view, VALID, completed, (void *)(intptr_t)REFUSED_CLOSED),
                  "a closed view: false");
            draining = 2;
        }
    } else if (draining) {
        /* 300 ms for an answer that must not come, or a second one. */
        if (++draining > 32) {
            for (int i = 0; i < COUNT; ++i) {
                char what[160];
                snprintf(what, sizeof what, "invalid override %d: queued (true), then exactly one error answer", i);
                check(invalid_queued[i] && invalid_errors[i] == 1 && !invalid_others[i], what);
            }
            check(!refused_answers, "a call that returned false (NULL view, closed view) never called back");
            check(!answers_inside, "every answer came after its call returned, bad characters' too");
            complete = 1;
            zebkit_view_close(view);
            zebkit_quit();
            return;
        }
    } else if (evaluating && state_answers == sent + 1) {
        evaluating = 0;
        if (!invalid || sent == COUNT) {
            check(state_true == state_answers, invalid ? "every refused override left the state as it was"
                                                       : "the page has the override: agent, platform, hints, brands");
            if (!invalid) {
                complete = 1;
                zebkit_view_close(view);
                zebkit_quit();
                return;
            }
            check(!zebkit_view_set_user_agent_override(NULL, VALID, completed, (void *)(intptr_t)REFUSED_NULL),
                  "a NULL view: false");
            closed_view = zebkit_view_new(NULL, 64, 64);
            zebkit_view_on_close(closed_view, on_other_close, NULL);
            zebkit_view_close(closed_view);
            draining = 1;
        } else {
            in_call = 1;
            invalid_queued[sent] = zebkit_view_set_user_agent_override(view, INVALID[sent], completed,
                                                                       (void *)(intptr_t)(sent + 1));
            in_call = 0;
            check(invalid_queued[sent], "an invalid override on a live view: true (told later, as an error)");
            ++sent;
        }
    } else if (!evaluating && sent && (!invalid_queued[sent - 1] || settled(sent - 1))) {
        evaluating = 1;
        zebkit_view_evaluate(view, STATE, evaluated, NULL);
    }
    if (++ticks > 3000) {
        check(0, "the mode finished");
        zebkit_quit();
        return;
    }
    zebkit_post_delayed_task(poll, NULL, 10);
}
static void ready(void *data) {
    (void)data;
    view = zebkit_view_new(NULL, 320, 200);
    zebkit_view_on_load(view, load, NULL);
    check(zebkit_view_set_user_agent_override(view, VALID, completed, NULL), "the override queued");
    /* The load waits for it to complete. */
    zebkit_view_load_url(view, getenv("ZEBKIT_UA_TEST_URL"));
    poll(NULL);
}
int main(int argc, char **argv) {
    mode = getenv("ZEBKIT_UA_CASE");
    if (!mode || !getenv("ZEBKIT_UA_TEST_URL")) return 2;
    int status = zebkit_main(argc, argv, ready, NULL);
    if (status || failures || !complete) return 1;
    printf("ok C UA/client hints %s\n", mode);
    return 0;
}
