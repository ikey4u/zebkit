/* The first thing to run on Windows: the engine starts, a view loads a page,
 * script runs in it, and a picture of it is written to the file named by the
 * first argument that does not start with "--", for the pixels to be looked at.
 * The rest of the arguments are the engine's.
 *
 * Exit 0 when every check passes. */

#include "harness.h"

#include <zebkit/page.h>
#ifdef ZEBKIT_TEST_SANDBOX
#include <zebkit/win_sandbox.h>
#endif

static const char *picture = "win_smoke.png";

/* A page of known colours: the left half pure red, the right half pure blue. */
static const char page[] =
    "data:text/html,<!doctype html><title>smoke</title><style>"
    "html,body{margin:0;height:100%25;background:%23ff0000}"
    "div{position:fixed;left:50%25;top:0;right:0;bottom:0;background:%230000ff}"
    "</style><div></div>";

static void printed(zebkit_view *view, const uint8_t *pdf, size_t size, const char *error, void *data) {
    (void)data;
    check(pdf && size >= 100 && !memcmp(pdf, "%PDF", 4) && !error, "print to PDF", "%zu bytes, starts %.4s, error %s",
          size, pdf ? (const char *)pdf : "", error ? error : "none");
    zebkit_view_close(view);
    zebkit_quit();
}

static void captured(zebkit_view *view, const uint8_t *png, size_t size, const char *error, void *data) {
    (void)data;
    check(png && size, "a picture of the page", "%zu bytes, error %s", size, error ? error : "none");
    if (png && size) {
        FILE *file = fopen(picture, "wb");
        int written = file && fwrite(png, 1, size, file) == size;
        if (file)
            written = !fclose(file) && written;
        check(written, "the picture written", "%s", picture);
    }
    zebkit_view_print_to_pdf(view, NULL, printed, NULL);
}

static void evaluated(zebkit_view *view, const char *json, const char *error, void *data) {
    (void)data;
    check(json && !error && strstr(json, " | smoke"), "script runs in the page", "%s, error %s", json ? json : "null",
          error ? error : "none");
    zebkit_view_capture(view, captured, NULL);
}

static void loaded(zebkit_view *view, zebkit_load_event event, const char *url, void *data) {
    (void)url;
    (void)data;
    if (event == ZEBKIT_LOAD_FINISHED || event == ZEBKIT_LOAD_FAILED)
        check(event == ZEBKIT_LOAD_FINISHED, "the page loads", "load event %d", (int)event);
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
    check(view != NULL, "a view is made", NULL);
    if (!view) {
        zebkit_quit();
        return;
    }
    zebkit_view_on_load(view, loaded, NULL);
}

int main(int argc, char **argv) {
#ifdef ZEBKIT_TEST_SANDBOX
    /* Built with zebkit_sandbox.lib: this executable is the engine's sandboxed
     * children too. */
    int child_code;
    if (zebkit_win_startup(&child_code))
        return child_code;
#endif
    char **engine = calloc((size_t)argc + 1, sizeof(char *));
    int count = 0;
    for (int i = 0; i < argc; ++i) {
        if (i && strncmp(argv[i], "--", 2))
            picture = argv[i];
        else
            engine[count++] = argv[i];
    }
    const int code = zebkit_main(count, engine, ready, NULL);
    check(code == 0, "zebkit_main returns 0", "%d", code);
    free(engine);
    return summary();
}
