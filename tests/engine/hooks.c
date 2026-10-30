/* Runtime settings and popup policy are independent between live views. */
#include <zebkit/hooks.h>
#include <stdio.h>
#include <string.h>

static zebkit_view *views[2];
static int failures, step, allowed[2], blocked[2], decisions[2], console_messages[2];
static int polls, evaluation_done;
static zebkit_popup_policy initial_policy = ZEBKIT_POPUP_POLICY_BLOCK_WITHOUT_GESTURE;

static void check(int ok, const char *what) {
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    fflush(stdout);
    failures += !ok;
}

static void opened(zebkit_view *view, const char *url, void *data) {
    int n = (int)(intptr_t)data;
    check(view == views[n] && strstr(url, "popup.invalid"), "allowed attempt goes to its own view's handler");
    ++allowed[n];
}

static void denied(zebkit_view *view, const char *json, void *data) {
    int n = (int)(intptr_t)data;
    check(view == views[n] && strstr(json, "popup.invalid") && strstr(json, "frameName"),
          "blocked attempt reports URL and frame name to its own view");
    ++blocked[n];
}

static void window_event(zebkit_view *view, const char *json, void *data) {
    int n = (int)(intptr_t)data;
    check(view == views[n] && strstr(json, "\"phase\":\"decision\"") &&
          strstr(json, "\"features\"") && strstr(json, "\"disposition\"") && strstr(json, "\"openerUrl\""),
          "unified window event reports its decision and Chromium parameters");
    ++decisions[n];
}

static void console_message(zebkit_view *view, const char *json, void *data) {
    if (!strstr(json, "native-hook-console")) return;
    int n = (int)(intptr_t)data;
    check(view == views[n] && strstr(json, "\"level\":\"info\"") &&
          strstr(json, "\"mainFrame\":true") && strstr(json, "\"frameId\"") && strstr(json, "\"origin\""),
          "native console message identifies its actual C view and frame");
    ++console_messages[n];
}

static void next(void *data);

static void evaluated(zebkit_view *view, const char *json, const char *error, void *data) {
    (void)view; (void)data;
    check(!error && json, "popup attempt completes");
    evaluation_done = 1;
}

static void next(void *data) {
    (void)data;
    /* Load notifications and popup IPC can arrive in either order. Advance
     * only once the preceding decision and its event have both arrived. */
    static const int expected[][4] = {
        {0, 0, 0, 0}, {1, 0, 0, 0}, {1, 0, 0, 1}, {1, 0, 1, 1},
        {1, 1, 1, 1}, {1, 1, 2, 1}, {2, 1, 2, 1}
    };
    if (step < 7 && (allowed[0] != expected[step][0] || allowed[1] != expected[step][1] ||
                     blocked[0] != expected[step][2] || blocked[1] != expected[step][3] ||
                     decisions[0] != expected[step][0] + expected[step][2] ||
                     decisions[1] != expected[step][1] + expected[step][3] ||
                     console_messages[0] != expected[step][0] + expected[step][2] ||
                     console_messages[1] != expected[step][1] + expected[step][3] ||
                     (step == 6 && !evaluation_done))) {
        if (++polls < 1000) {
            zebkit_post_delayed_task(next, NULL, 10);
            return;
        }
        check(0, "popup decision and event arrive before the deadline");
        zebkit_quit();
        return;
    }
    int n = 0;
    int gesture = 0;
    switch (step++) {
    case 0:
        check(zebkit_view_set_popup_policy(views[0], ZEBKIT_POPUP_POLICY_ALLOW), "allow policy accepted");
        check(zebkit_view_set_popup_policy(views[1], ZEBKIT_POPUP_POLICY_BLOCK), "block policy accepted");
        break;
    case 1:
        n = 1;
        break;
    case 2:
        check(allowed[0] == 1 && allowed[1] == 0 && blocked[0] == 0 && blocked[1] == 1,
              "two simultaneous views have independent policies");
        zebkit_view_set_popup_policy(views[0], ZEBKIT_POPUP_POLICY_BLOCK);
        break;
    case 3:
        n = 1;
        zebkit_view_set_popup_policy(views[1], ZEBKIT_POPUP_POLICY_ALLOW);
        break;
    case 4:
        check(allowed[0] == 1 && allowed[1] == 1 && blocked[0] == 1 && blocked[1] == 1,
              "runtime policy changes affect the next attempts");
        zebkit_view_set_popup_policy(views[0], ZEBKIT_POPUP_POLICY_BLOCK_WITHOUT_GESTURE);
        check(!zebkit_view_set_popup_policy(views[0], (zebkit_popup_policy)99) &&
              zebkit_view_get_popup_policy(views[0]) == ZEBKIT_POPUP_POLICY_BLOCK_WITHOUT_GESTURE,
              "invalid policy leaves the current policy unchanged");
        break;
    case 5:
        check(blocked[0] == 2, "default policy blocks a page's popup without a gesture");
        /* The application's evaluate uses Chromium's userGesture flag. */
        gesture = 1;
        break;
    default:
        check(allowed[0] == 2 && allowed[1] == 1 && blocked[0] == 2 && blocked[1] == 1,
              "default policy permits a user gesture and keeps the other view's state");
        zebkit_view_close(views[0]);
        zebkit_view_close(views[1]);
        zebkit_quit();
        return;
    }
    if (gesture)
        zebkit_view_evaluate(views[n], "console.log('native-hook-console'); window.open('https://popup.invalid', 'target') === null", evaluated, NULL);
    else
        zebkit_view_load_url(views[n], "data:text/html,<script>console.log('native-hook-console');window.open('https://popup.invalid','target')</script>");
    zebkit_post_delayed_task(next, NULL, 10);
}

static void ready(void *data) {
    (void)data;
    for (int i = 0; i < 2; ++i) {
        views[i] = zebkit_view_new(NULL, 320, 200);
        zebkit_view_on_new_window(views[i], opened, (void *)(intptr_t)i);
        zebkit_view_on_popup_blocked(views[i], denied, (void *)(intptr_t)i);
        zebkit_view_on_window_open_event(views[i], window_event, (void *)(intptr_t)i);
        zebkit_view_on_console(views[i], console_message, (void *)(intptr_t)i);
    }
    char *error = NULL;
    char *opening = zebkit_view_get_window_open_info(views[0]);
    check(opening && strcmp(opening, "{}") == 0 && !zebkit_view_get_window_open_info(NULL),
          "ordinary views have no popup creation metadata");
    zebkit_free(opening);
    check(zebkit_view_set_permission(views[0], "https://permissions.test/path", "geolocation", ZEBKIT_PERMISSION_GRANTED, &error),
          "permission accepts and canonicalizes a concrete origin");
    check(zebkit_view_get_permission(views[0], "https://permissions.test", "geolocation") == ZEBKIT_PERMISSION_GRANTED &&
          zebkit_view_get_permission(views[1], "https://permissions.test", "geolocation") == ZEBKIT_PERMISSION_PROMPT,
          "permission policy is independent between C views");
    check(!zebkit_view_set_permission(views[0], "data:text/plain,opaque", "geolocation", ZEBKIT_PERMISSION_DENIED, &error) && error,
          "permission rejects opaque rules");
    zebkit_free(error); error = NULL;
    check(zebkit_view_get_permission(views[0], "https://permissions.test", "geolocation") == ZEBKIT_PERMISSION_GRANTED,
          "invalid permission update preserves policy");
    check(zebkit_view_reset_permissions(views[0], NULL, &error) &&
          zebkit_view_get_permission(views[0], "https://permissions.test", "geolocation") == ZEBKIT_PERMISSION_PROMPT,
          "C permission reset removes view overrides");
    check(!zebkit_view_set_permission_timeout(views[0], 0) && zebkit_view_set_permission_timeout(views[0], 30000),
          "C permission deadlines reject zero");
    check(!zebkit_permission_respond(NULL, ZEBKIT_PERMISSION_GRANTED, false) && !zebkit_permission_request_info(NULL),
          "C permission replies reject invalid tokens");
    check(!zebkit_view_set_certificate_timeout(views[0], 0) &&
          zebkit_view_set_certificate_timeout(views[0], 30000),
          "C certificate deadlines reject zero");
    check(!zebkit_certificate_request_info(NULL) && !zebkit_certificate_respond(NULL, true),
          "C certificate replies reject invalid tokens");
    check(!zebkit_view_set_navigation_timeout(views[0], 0) && zebkit_view_set_navigation_timeout(views[0], 30000),
          "C navigation deadlines reject zero");
    check(!zebkit_navigation_request_info(NULL) &&
          !zebkit_navigation_respond(NULL, ZEBKIT_NAVIGATION_CONTINUE, NULL, &error) && error,
          "C navigation replies reject invalid tokens");
    zebkit_free(error); error = NULL;
    check(!zebkit_view_set_settings(NULL, "{}", &error) && error && !zebkit_view_get_settings(NULL),
          "settings reject a missing view");
    zebkit_free(error); error = NULL;
    check(zebkit_view_set_settings(views[0], "{\"colorScheme\":\"dark\",\"defaultFontSize\":24}", &error) && !error,
          "native settings accept a validated batch");
    char *before = zebkit_view_get_settings(views[0]);
    char *other = zebkit_view_get_settings(views[1]);
    check(before && strstr(before, "dark") && strstr(before, "24") && other && !strcmp(other, "{}"),
          "native settings are independent per view");
    zebkit_free(other);
    check(!zebkit_view_set_settings(views[0], "{\"colorScheme\":\"light\",\"javaScript\":5}", &error) && error,
          "invalid settings batch is refused");
    zebkit_free(error); error = NULL;
    char *after = zebkit_view_get_settings(views[0]);
    check(before && after && !strcmp(before, after), "invalid batch leaves every prior setting intact");
    zebkit_free(before); zebkit_free(after);
    check(zebkit_view_set_settings(views[0], "{\"colorScheme\":null}", &error), "one setting can be reset");
    after = zebkit_view_get_settings(views[0]);
    check(after && !strstr(after, "colorScheme") && strstr(after, "24"), "reset preserves unrelated settings");
    zebkit_free(after);
    check(zebkit_view_set_settings(views[0], "{\"defaultFontSize\":null}", &error), "all overrides can be reset");
    after = zebkit_view_get_settings(views[0]);
    check(after && !strcmp(after, "{}"), "reset returns to native defaults");
    zebkit_free(after);
    check(zebkit_view_get_popup_policy(views[0]) == initial_policy,
          "initial policy honors the startup switch");
    next(NULL);
}

int main(int argc, char **argv) {
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--disable-popup-blocking") == 0)
            initial_policy = ZEBKIT_POPUP_POLICY_ALLOW;
    }
    int result = zebkit_main(argc, argv, ready, NULL);
    return result || failures || step < 7 ? 1 : 0;
}
