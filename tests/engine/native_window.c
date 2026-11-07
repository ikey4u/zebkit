/* Native window queries and exports must never expose headless or off-thread
 * internals; an export answers once, after the call. */
#include <zebkit/zebkit.h>
#include <zebkit/offscreen.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int failures;
static void check(int ok, const char *label) {
    printf("%s %s\n", ok ? "ok" : "FAIL", label);
    failures += !ok;
}
static int cleared(zebkit_native_window value) {
    return value.kind == ZEBKIT_NATIVE_WINDOW_NONE && value.handle == 0;
}
/* zebkit_view_export_native_window's answer, waited for with the loop. */
static int exported;
static char exported_handle[256];
static void on_export(void *data, const char *handle) {
    (*(int *)data)++;
    exported++;
    snprintf(exported_handle, sizeof exported_handle, "%s", handle ? handle : "(null)");
}
static int export_handle(zebkit_view *view, int *calls) {
    exported = 0;
    exported_handle[0] = 0;
    if (!zebkit_view_export_native_window(view, on_export, calls)) return 0;
    if (exported) return -1; /* it ran before the call returned */
    struct timespec start, now;
    clock_gettime(CLOCK_MONOTONIC, &start);
    do {
        zebkit_pump();
        struct timespec pause = {0, 5 * 1000 * 1000};
        nanosleep(&pause, NULL);
        clock_gettime(CLOCK_MONOTONIC, &now);
    } while (!exported && now.tv_sec - start.tv_sec < 10);
    return exported;
}
static void *worker(void *raw) {
    zebkit_native_window info = {ZEBKIT_NATIVE_WINDOW_APPKIT, 123};
    check(!zebkit_view_get_native_window(raw, &info) && cleared(info),
          "native window rejects the wrong thread and clears output");
    check(!zebkit_view_export_native_window(raw, on_export, &(int){0}),
          "native window export refused on the wrong thread");
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
        int calls = 0;
        char expected[64];
        snprintf(expected, sizeof expected, "x11:%#lx", (unsigned long)parent.handle);
        check(export_handle(view, &calls) == 1 && calls == 1 && !strcmp(exported_handle, expected),
              "export: the portal's x11:<XID> of the same window, once, after the call");
        printf("     %s\n", exported_handle);
    } else if (getenv("ZEBKIT_TEST_NATIVE_WINDOW_WAYLAND")) {
        check(!zebkit_view_get_native_window(view, &info) && cleared(info),
              "Wayland: no X11/HWND/NSWindow handle, output cleared");
        int calls = 0;
        check(export_handle(view, &calls) == 1 && calls == 1 && !strncmp(exported_handle, "wayland:", 8) &&
                  strlen(exported_handle) > 8,
              "Wayland: export gives wayland:<handle> through xdg-foreign, once, after the call");
        printf("     %s\n", exported_handle);
    } else {
        check(!zebkit_view_get_native_window(view, &info) && cleared(info),
              "headless window unavailable with cleared output");
        int calls = 0;
        check(export_handle(view, &calls) == 1 && calls == 1 && !strcmp(exported_handle, "(null)"),
              "headless: export answers NULL, once, after the call");
    }
    check(!zebkit_view_export_native_window(view, NULL, NULL), "native window export refuses a NULL callback");
    check(!zebkit_view_export_native_window(NULL, on_export, &(int){0}), "native window export refuses a NULL view");
    pthread_t thread;
    int started = pthread_create(&thread, NULL, worker, view) == 0;
    check(started, "native window worker starts");
    if (started) pthread_join(thread, NULL);
    zebkit_view *offscreen = zebkit_view_new_offscreen("about:blank", 200, 100, 1.0f);
    if (getenv("ZEBKIT_TEST_NATIVE_WINDOW_WAYLAND"))
        check(!offscreen, "Wayland: an off-screen view beside its windows is refused, not made (not done there yet)");
    info = (zebkit_native_window){ZEBKIT_NATIVE_WINDOW_APPKIT, 123};
    check((offscreen || getenv("ZEBKIT_TEST_NATIVE_WINDOW_WAYLAND")) &&
              !zebkit_view_get_native_window(offscreen, &info) && cleared(info),
          "off-screen window unavailable with cleared output");
    if (offscreen) {
        int calls = 0;
        check(export_handle(offscreen, &calls) == 1 && calls == 1 && !strcmp(exported_handle, "(null)"),
              "off-screen: export answers NULL");
    }
    if (offscreen) zebkit_view_close(offscreen);
    if (view) zebkit_view_close(view);
    zebkit_shutdown();
    info = (zebkit_native_window){ZEBKIT_NATIVE_WINDOW_APPKIT, 123};
    check(!zebkit_view_get_native_window(NULL, &info) && cleared(info),
          "native window unavailable after shutdown");
    return failures ? 1 : 0;
}
