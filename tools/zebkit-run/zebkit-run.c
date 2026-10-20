/* zebkit-run: open each URL given in a view, and run until killed or until
 * the last of them is closed. Chromium's switches are passed through, so
 *
 *     zebkit-run --no-sandbox --remote-debugging-port=9222 https://example.com/
 *
 * opens a page and serves CDP. The scripts use it to load pages in a
 * package (scripts/bench.sh, scripts/engine-test.sh). */

#include <stdio.h>
#include <string.h>

#include <zebkit/zebkit.h>

static int url_count;
static char **urls;
static int open_views;

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

static void ready(void *data) {
    (void)data;
    if (url_count == 0)
        open_view("about:blank");
    for (int i = 0; i < url_count; ++i)
        open_view(urls[i]);
}

int main(int argc, char **argv) {
    char *found[64];
    urls = found;
    for (int i = 1; i < argc && url_count < 64; ++i) {
        if (strncmp(argv[i], "--", 2) != 0)
            found[url_count++] = argv[i];
    }
    return zebkit_main(argc, argv, ready, NULL);
}
