/* Native window queries must never expose headless or off-thread internals. */
#include <zebkit/zebkit.h>
#include <zebkit/offscreen.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

static int failures;
static void check(int ok, const char *label) {
    printf("%s %s\n", ok ? "ok" : "FAIL", label);
    failures += !ok;
}
static int cleared(zebkit_native_window value) {
    return value.kind == ZEBKIT_NATIVE_WINDOW_NONE && value.handle == 0;
}
static void *worker(void *raw) {
    zebkit_native_window info = {ZEBKIT_NATIVE_WINDOW_APPKIT, 123};
    check(!zebkit_view_get_native_window(raw, &info) && cleared(info),
          "native window rejects the wrong thread and clears output");
    return NULL;
}
int main(int argc, char **argv) {
    zebkit_native_window info = {ZEBKIT_NATIVE_WINDOW_APPKIT, 123};
    check(!zebkit_view_get_native_window(NULL, &info) && cleared(info),
          "native window unavailable before initialization");
    int code = zebkit_init(argc, argv);
    if (code) return code;
    zebkit_view *view = zebkit_view_new("about:blank", 200, 100);
    check(view != NULL, "native window test view exists");
    check(!zebkit_view_get_native_window(view, NULL), "native window rejects NULL output");
    info = (zebkit_native_window){ZEBKIT_NATIVE_WINDOW_APPKIT, 123};
    if (getenv("ZEBKIT_TEST_NATIVE_WINDOW_X11")) {
        check(zebkit_view_get_native_window(view, &info) && info.kind == ZEBKIT_NATIVE_WINDOW_X11 && info.handle,
              "headed view returns a real X11 window");
        zebkit_native_window parent = info, own = {ZEBKIT_NATIVE_WINDOW_NONE, 0};
        zebkit_view *child = zebkit_view_new("about:blank", 100, 80);
        check(child && zebkit_view_get_native_window(child, &own) && own.handle != parent.handle,
              "a second standalone page has its own system window");
        zebkit_view_attach(child, view, 0, 0, 100, 80);
        check(zebkit_view_get_native_window(child, &own) && own.kind == parent.kind && own.handle == parent.handle,
              "embedded page reports actual containing Widget, not hidden original");
        if (child) zebkit_view_close(child);
    } else {
        check(!zebkit_view_get_native_window(view, &info) && cleared(info),
              "headless window unavailable with cleared output");
    }
    pthread_t thread;
    int started = pthread_create(&thread, NULL, worker, view) == 0;
    check(started, "native window worker starts");
    if (started) pthread_join(thread, NULL);
    zebkit_view *offscreen = zebkit_view_new_offscreen("about:blank", 200, 100, 1.0f);
    info = (zebkit_native_window){ZEBKIT_NATIVE_WINDOW_APPKIT, 123};
    check(offscreen && !zebkit_view_get_native_window(offscreen, &info) && cleared(info),
          "off-screen window unavailable with cleared output");
    if (offscreen) zebkit_view_close(offscreen);
    if (view) zebkit_view_close(view);
    zebkit_shutdown();
    info = (zebkit_native_window){ZEBKIT_NATIVE_WINDOW_APPKIT, 123};
    check(!zebkit_view_get_native_window(NULL, &info) && cleared(info),
          "native window unavailable after shutdown");
    return failures ? 1 : 0;
}
