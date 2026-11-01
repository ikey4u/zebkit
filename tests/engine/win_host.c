/* A Windows application with its own window and its own loop: zebkit_init()
 * and zebkit_pump() under the application's message loop, and a view inside
 * the application's HWND (zebkit_view_attach_native()). The page is pure red
 * on the left and pure blue on the right; what is on the screen is read back
 * with GDI, so the picture checked is the one a person would see.
 *
 * Exit 0 when every check passes. */

#include <windows.h>
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zebkit/zebkit.h>

static const char page[] =
    "data:text/html,<!doctype html><title>host</title><style>"
    "html,body{margin:0;height:100%25;background:%23ff0000}"
    "div{position:fixed;left:50%25;top:0;right:0;bottom:0;background:%230000ff}"
    "</style><div></div>";

static HWND window;
static HANDLE wake;
static int failures, done;
static zebkit_view *view;

static void check(int ok, const char *what) {
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    fflush(stdout);
    if (!ok)
        ++failures;
}

/* From any thread: the application's loop looks again. */
static void wakeup(int64_t delay_ms, void *data) {
    (void)delay_ms;
    (void)data;
    SetEvent(wake);
}

static COLORREF pixel(int x, int y) {
    POINT point = {x, y};
    ClientToScreen(window, &point);
    HDC screen = GetDC(NULL);
    COLORREF color = GetPixel(screen, point.x, point.y);
    ReleaseDC(NULL, screen);
    return color;
}

static LRESULT CALLBACK proc(HWND hwnd, UINT message, WPARAM w, LPARAM l) {
    if (message == WM_SIZE && view)
        zebkit_view_set_bounds(view, 10, 10, LOWORD(l) - 20, HIWORD(l) - 20);
    return DefWindowProcW(hwnd, message, w, l);
}

static void evaluated(zebkit_view *v, const char *json, const char *error, void *data) {
    (void)v;
    (void)data;
    printf("page says %s, error %s\n", json ? json : "null", error ? error : "none");
    check(json && !strcmp(json, "780"), "the page is the attached size, the client area less its margins");
    done = 1;
}

static void loaded(zebkit_view *v, zebkit_load_event event, const char *url, void *data) {
    (void)url;
    (void)data;
    if (event == ZEBKIT_LOAD_FAILED) {
        check(0, "the page loads");
        done = 2;
    } else if (event == ZEBKIT_LOAD_FINISHED) {
        check(1, "the page loads");
        zebkit_view_evaluate(v,
                             "new Promise(r => requestAnimationFrame(() => requestAnimationFrame(() => "
                             "setTimeout(() => r(innerWidth), 1000))))",
                             evaluated, NULL);
    }
}

int main(int argc, char **argv) {
    wake = CreateEventW(NULL, FALSE, FALSE, NULL);
    check(zebkit_set_wakeup(wakeup, NULL), "zebkit_set_wakeup() before the engine");

    WNDCLASSW type = {0};
    type.lpfnWndProc = proc;
    type.hInstance = GetModuleHandleW(NULL);
    type.lpszClassName = L"zebkit-win-host";
    type.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    RegisterClassW(&type);
    RECT frame = {0, 0, 800, 600};
    AdjustWindowRect(&frame, WS_OVERLAPPEDWINDOW, FALSE);
    window = CreateWindowW(L"zebkit-win-host", L"zebkit host", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 50, 50,
                           frame.right - frame.left, frame.bottom - frame.top, NULL, NULL, type.hInstance, NULL);
    check(window != NULL, "the application's window");

    char *error = NULL;
    const int code = zebkit_init(argc, argv);
    check(code == 0, "zebkit_init()");
    if (code)
        return code;

    view = zebkit_view_new(page, 780, 580);
    check(view != NULL, "zebkit_view_new()");
    zebkit_view_on_load(view, loaded, NULL);
    const bool attached = zebkit_view_attach_native(view, (uintptr_t)window, 10, 10, 780, 580, &error);
    check(attached, error ? error : "zebkit_view_attach_native()");

    ULONGLONG deadline = GetTickCount64() + 60000;
    while (!done && GetTickCount64() < deadline) {
        MsgWaitForMultipleObjects(1, &wake, FALSE, 10, QS_ALLINPUT);
        MSG message;
        while (PeekMessageW(&message, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        zebkit_pump();
    }
    check(done == 1, "the page answered, under the application's loop");

    /* A moment of the application's own loop, for the compositor's frame. */
    const char *hold = getenv("WIN_HOST_HOLD_MS");
    deadline = GetTickCount64() + 2000 + (hold ? atoi(hold) : 0);
    while (GetTickCount64() < deadline) {
        MsgWaitForMultipleObjects(1, &wake, FALSE, 10, QS_ALLINPUT);
        MSG message;
        while (PeekMessageW(&message, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        zebkit_pump();
    }
    /* Near the top: on a machine with a person, little covers it there. */
    const COLORREF margin = pixel(4, 40), left = pixel(200, 40), right = pixel(600, 40);
    printf("screen: margin %06lx left %06lx right %06lx (GDI's 00bbggrr)\n", margin, left, right);
    check(margin == RGB(0, 0, 0), "the application's own window shows at the margin");
    /* Within what a display's colour handling takes away. */
    check(GetRValue(left) >= 250 && GetGValue(left) <= 5 && GetBValue(left) <= 5,
          "red on the screen, inside the application's window");
    check(GetRValue(right) <= 5 && GetGValue(right) <= 5 && GetBValue(right) >= 250,
          "blue on the screen, inside the application's window");

    zebkit_view_close(view);
    view = NULL;
    for (int i = 0; i < 20; ++i) {
        Sleep(10);
        zebkit_pump();
    }
    check(zebkit_shutdown(), "zebkit_shutdown()");
    printf("%d failures\n", failures);
    fflush(stdout);
    return failures ? 1 : 0;
}
