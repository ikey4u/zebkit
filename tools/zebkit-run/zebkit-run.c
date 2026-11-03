/* zebkit-run: open each URL given in a view, and run until killed or until
 * the last of them is closed. Chromium's switches are passed through, so
 *
 *     zebkit-run --no-sandbox --remote-debugging-port=9222 https://example.com/
 *
 * opens a page and serves CDP, and
 *
 *     zebkit-run --headless --playwright-port=0
 *
 * serves Playwright's own protocol (<zebkit/automation.h>), printing
 * "Playwright listening on ws://..." to stderr. Serving it, the engine gets the
 * switches Playwright launches Chromium with (chromiumSwitches.ts and
 * chromium.ts of the pinned release: no back-forward cache, no popup blocker,
 * no third-party storage partitioning, a desktop pointer and no scrollbars
 * when headless, ...), so that a page behaves as under Playwright's own
 * server; the switches given after them win. The scripts use it to load
 * pages in a package (scripts/bench.sh, scripts/engine-test.sh,
 * scripts/automation-bench.sh, scripts/playwright-suite.sh). */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zebkit/automation.h>
#include <zebkit/zebkit.h>

static int url_count;
static char **urls;
static int open_views;
static int playwright_port = -1;

static void closed(zebkit_view *view, void *data) {
    (void)view, (void)data;
    if (--open_views == 0)
        zebkit_quit();
}

static void open_view(const char *url) {
    zebkit_view *view = zebkit_view_new(url, 0, 0);
    if (view) {
        ++open_views;
        zebkit_view_on_close(view, closed, NULL);
    }
}

static void listening(const char *url, const char *error, void *data) {
    (void)data;
    if (url)
        fprintf(stderr, "Playwright listening on %s\n", url);
    else
        fprintf(stderr, "zebkit-run: %s\n", error);
}

static int no_startup_window;

static void ready(void *data) {
    (void)data;
    if (no_startup_window) {
        /* A browser launched by a CDP client over its pipe, as Chrome is by
         * Playwright: no window until it asks, and it ends with Browser.close. */
        ++open_views;
        return;
    }
    if (playwright_port >= 0) {
        /* The client opens its own pages. */
        zebkit_automation_listen(playwright_port, listening, NULL);
        ++open_views;
        return;
    }
    if (url_count == 0)
        open_view("about:blank");
    for (int i = 0; i < url_count; ++i)
        open_view(urls[i]);
}

/* Playwright's chromiumSwitches() that the content layer knows, and what
 * chromium.ts adds for a headless browser. */
static const char *const playwright_switches[] = {
    "--disable-back-forward-cache",
    "--disable-background-timer-throttling",
    "--disable-backgrounding-occluded-windows",
    "--disable-renderer-backgrounding",
    "--disable-ipc-flooding-protection",
    "--disable-hang-monitor",
    "--disable-popup-blocking",
    "--disable-prompt-on-repost",
    "--allow-pre-commit-input",
    "--force-color-profile=srgb",
    "--export-tagged-pdf",
    "--disable-features=AvoidUnnecessaryBeforeUnloadCheckSync,PaintHolding,"
    "ThirdPartyStoragePartitioning,BlockOriginHeaderModificationOnRedirect,"
    "AvoidCorsURLLoaderRestartOnRedirect,HttpsUpgrades",
};
static const char *const playwright_headless_switches[] = {
    "--hide-scrollbars",
    "--mute-audio",
    "--blink-settings=primaryHoverType=2,availableHoverTypes=2,primaryPointerType=4,availablePointerTypes=4",
};

#define COUNT(a) ((int)(sizeof(a) / sizeof((a)[0])))

int main(int argc, char **argv) {
    char *found[64];
    int headless = 0;
    urls = found;
    for (int i = 1; i < argc; ++i) {
        if (strncmp(argv[i], "--playwright-port=", 18) == 0)
            playwright_port = atoi(argv[i] + 18);
        else if (strcmp(argv[i], "--headless") == 0 || strncmp(argv[i], "--headless=", 11) == 0)
            headless = 1;
        else if (strcmp(argv[i], "--no-startup-window") == 0)
            no_startup_window = 1;
        else if (strncmp(argv[i], "--", 2) != 0 && url_count < 64)
            found[url_count++] = argv[i];
    }
    if (playwright_port < 0) {
        /* Served over a pipe, the client launched it: its Browser.close ends it. */
        int pipe = 0;
        for (int i = 1; i < argc; ++i)
            pipe |= strcmp(argv[i], "--remote-debugging-pipe") == 0;
        if (!pipe)
            return zebkit_main(argc, argv, ready, NULL);
        char **with = malloc(sizeof(char *) * (size_t)(argc + 2));
        int count = 0;
        for (int i = 0; i < argc; ++i)
            with[count++] = argv[i];
        with[count++] = "--zebkit-quit-on-browser-close";
        with[count] = NULL;
        return zebkit_main(count, with, ready, NULL);
    }
    /* The program's name, Playwright's switches, then the caller's. */
    char **with = malloc(sizeof(char *) * (size_t)(argc + COUNT(playwright_switches) +
                                                    COUNT(playwright_headless_switches) + 1));
    int count = 0;
    with[count++] = argv[0];
    for (int i = 0; i < COUNT(playwright_switches); ++i)
        with[count++] = (char *)playwright_switches[i];
    for (int i = 0; headless && i < COUNT(playwright_headless_switches); ++i)
        with[count++] = (char *)playwright_headless_switches[i];
    for (int i = 1; i < argc; ++i)
        with[count++] = argv[i];
    with[count] = NULL;
    return zebkit_main(count, with, ready, NULL);
}
