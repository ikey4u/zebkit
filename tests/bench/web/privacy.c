/*
 * privacy URL: load tests/bench/web/local/privacy.html in the engine and
 * print what it found, the JSON its title becomes, for scripts/web-bench.sh
 * --check to hold against zebkit's privacy defaults.
 */
#include <stdio.h>
#include <string.h>

#include <zebkit/zebkit.h>

static const char *url;

static void titled(zebkit_view *view, const char *title, void *data) {
    (void)data;
    if (title && title[0] == '{') {
        printf("%s\n", title);
        fflush(stdout);
        zebkit_view_close(view);
        zebkit_quit();
    }
}

static void give_up(void *data) {
    (void)data;
    printf("{\"error\": \"no result in 20 s\"}\n");
    zebkit_quit();
}

static void ready(void *data) {
    (void)data;
    zebkit_view *view = zebkit_view_new(url, 800, 600);
    zebkit_view_on_title(view, titled, NULL);
    zebkit_post_delayed_task(give_up, NULL, 20000);
}

int main(int argc, char **argv) {
    url = argv[argc - 1];
    return zebkit_main(argc - 1, argv, ready, NULL);
}
