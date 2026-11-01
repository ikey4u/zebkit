/* The first thing to run on Windows: the engine starts, a view loads a page,
 * script runs in it, and a picture of it is written to the file named by the
 * first argument that does not start with "--", for the pixels to be looked at.
 * The rest of the arguments are the engine's.
 *
 * Exit 0 when each step happened. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zebkit/zebkit.h>

static const char *picture = "win_smoke.png";
static int result = 1;

/* A page of known colours: the left half pure red, the right half pure blue. */
static const char page[] =
    "data:text/html,<!doctype html><title>smoke</title><style>"
    "html,body{margin:0;height:100%25;background:%23ff0000}"
    "div{position:fixed;left:50%25;top:0;right:0;bottom:0;background:%230000ff}"
    "</style><div></div>";

static void captured(zebkit_view *view, const uint8_t *png, size_t size, const char *error, void *data) {
    (void)data;
    printf("capture: %zu bytes, error %s\n", size, error ? error : "none");
    if (png && size) {
        FILE *file = fopen(picture, "wb");
        if (file) {
            fwrite(png, 1, size, file);
            fclose(file);
            printf("wrote %s\n", picture);
            result = 0;
        }
    }
    fflush(stdout);
    zebkit_view_close(view);
    zebkit_quit();
}

static void evaluated(zebkit_view *view, const char *json, const char *error, void *data) {
    (void)data;
    printf("evaluate: %s, error %s\n", json ? json : "null", error ? error : "none");
    fflush(stdout);
    zebkit_view_capture(view, captured, NULL);
}

static void loaded(zebkit_view *view, zebkit_load_event event, const char *url, void *data) {
    (void)url;
    (void)data;
    printf("load event %d\n", (int)event);
    fflush(stdout);
    if (event == ZEBKIT_LOAD_FINISHED)
        /* Two frames on, so that what is drawn is the page. */
        zebkit_view_evaluate(view,
                             "new Promise(r => requestAnimationFrame(() => requestAnimationFrame(() => "
                             "r(navigator.userAgent + ' | ' + document.title))))",
                             evaluated, NULL);
    else if (event == ZEBKIT_LOAD_FAILED) {
        zebkit_view_close(view);
        zebkit_quit();
    }
}

static void ready(void *data) {
    (void)data;
    printf("engine %s, %s; headless %d\n", zebkit_version(), zebkit_engine_version(), (int)zebkit_is_headless());
    fflush(stdout);
    zebkit_view *view = zebkit_view_new(page, 800, 600);
    printf("view %p\n", (void *)view);
    fflush(stdout);
    if (!view) {
        zebkit_quit();
        return;
    }
    zebkit_view_on_load(view, loaded, NULL);
}

int main(int argc, char **argv) {
    char **engine = calloc((size_t)argc + 1, sizeof(char *));
    int count = 0;
    for (int i = 0; i < argc; ++i) {
        if (i && strncmp(argv[i], "--", 2))
            picture = argv[i];
        else
            engine[count++] = argv[i];
    }
    const int code = zebkit_main(count, engine, ready, NULL);
    printf("zebkit_main returned %d\n", code);
    return code ? code : result;
}
