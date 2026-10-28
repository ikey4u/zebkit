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
 * "Playwright listening on ws://..." to stderr. The scripts use it to load
 * pages in a package (scripts/bench.sh, scripts/engine-test.sh,
 * scripts/automation-bench.sh). */

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

static void ready(void *data) {
    (void)data;
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

int main(int argc, char **argv) {
    char *found[64];
    urls = found;
    for (int i = 1; i < argc && url_count < 64; ++i) {
        if (strncmp(argv[i], "--playwright-port=", 18) == 0)
            playwright_port = atoi(argv[i] + 18);
        else if (strncmp(argv[i], "--", 2) != 0)
            found[url_count++] = argv[i];
    }
    return zebkit_main(argc, argv, ready, NULL);
}
