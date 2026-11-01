/* A view inside an HWND of the application's, with real input: Windows' own
 * SendInput(), which goes where a person's mouse and keyboard would. The refusals of
 * zebkit_view_attach_native(), a click and keys reaching the page, moving and
 * sizing, detaching, and a parent destroyed first.
 *
 * The test takes the mouse and the keyboard for a moment. Exit 0 when every check
 * passes. */

#include <windows.h>
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zebkit/zebkit.h>
#ifdef ZEBKIT_TEST_SANDBOX
#include <zebkit/win_sandbox.h>
#endif

static const char page[] =
    "data:text/html,<!doctype html><title>input</title><style>"
    "html,body{margin:0;height:100%25;background:%23ffffff}"
    "input{position:fixed;left:50px;top:50px;width:200px;height:30px}</style><input id=i>"
    "<script>window.log=[];"
    "addEventListener('click',e=>log.push(['click',e.clientX,e.clientY,e.isTrusted]));"
    "addEventListener('wheel',e=>log.push(['wheel',Math.sign(e.deltaY),e.isTrusted]));"
    "addEventListener('keydown',e=>log.push(['key',e.key,e.isTrusted]));</script>";

static HWND window, other;
static HANDLE wake;
static int failures, checks;
static zebkit_view *view;
static char answer[4096];
static int answered, closed;

static void check(int ok, const char *what) {
    ++checks;
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    fflush(stdout);
    if (!ok)
        ++failures;
}

static void wakeup(int64_t delay_ms, void *data) {
    (void)delay_ms;
    (void)data;
    SetEvent(wake);
}

static void spin(DWORD ms) {
    const ULONGLONG deadline = GetTickCount64() + ms;
    while (GetTickCount64() < deadline) {
        MsgWaitForMultipleObjects(1, &wake, FALSE, 5, QS_ALLINPUT);
        MSG message;
        while (PeekMessageW(&message, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        zebkit_pump();
    }
}

static void result(zebkit_view *v, const char *json, const char *error, void *data) {
    (void)v;
    (void)data;
    snprintf(answer, sizeof(answer), "%s", json ? json : error ? error : "");
    answered = 1;
}

static const char *ask(const char *script) {
    answered = 0;
    zebkit_view_evaluate(view, script, result, NULL);
    for (int i = 0; i < 2000 && !answered; ++i)
        spin(5);
    return answered ? answer : "(no answer)";
}

static LRESULT CALLBACK proc(HWND hwnd, UINT message, WPARAM w, LPARAM l) {
    return DefWindowProcW(hwnd, message, w, l);
}

static void foreground(HWND hwnd) {
    /* Windows gives the foreground to the process with the last input event. */
    INPUT alt[2] = {{0}};
    alt[0].type = alt[1].type = INPUT_KEYBOARD;
    /* Not Alt, which would put the window in its menu's modal loop. */
    alt[0].ki.wVk = alt[1].ki.wVk = VK_SHIFT;
    alt[1].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(2, alt, sizeof(INPUT));
    SetForegroundWindow(hwnd);
    spin(300);
}

/* A real click at client coordinates of @hwnd. */
static void click(HWND hwnd, int x, int y) {
    POINT point = {x, y};
    ClientToScreen(hwnd, &point);
    SetCursorPos(point.x, point.y);
    spin(100);
    INPUT input[2] = {{0}};
    input[0].type = input[1].type = INPUT_MOUSE;
    input[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
    input[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
    SendInput(2, input, sizeof(INPUT));
    spin(500);
}

static void key(WORD code) {
    INPUT input[2] = {{0}};
    input[0].type = input[1].type = INPUT_KEYBOARD;
    input[0].ki.wVk = input[1].ki.wVk = code;
    input[1].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(2, input, sizeof(INPUT));
    spin(150);
}

static void wheel(int amount) {
    INPUT input = {0};
    input.type = INPUT_MOUSE;
    input.mi.dwFlags = MOUSEEVENTF_WHEEL;
    input.mi.mouseData = (DWORD)amount;
    SendInput(1, &input, sizeof(INPUT));
    spin(400);
}

static void loaded(zebkit_view *v, zebkit_load_event event, const char *url, void *data) {
    (void)v, (void)url;
    if (event == ZEBKIT_LOAD_FINISHED || event == ZEBKIT_LOAD_FAILED)
        *(int *)data = event == ZEBKIT_LOAD_FINISHED ? 1 : 2;
}

static void on_close(zebkit_view *v, void *data) {
    (void)v, (void)data;
    closed = 1;
}

static HWND make_window(const wchar_t *title, int x, int y, int width, int height) {
    RECT frame = {0, 0, width, height};
    AdjustWindowRect(&frame, WS_OVERLAPPEDWINDOW, FALSE);
    return CreateWindowW(L"zebkit-win-parent", title, WS_OVERLAPPEDWINDOW | WS_VISIBLE, x, y, frame.right - frame.left,
                         frame.bottom - frame.top, NULL, NULL, GetModuleHandleW(NULL), NULL);
}

int main(int argc, char **argv) {
#ifdef ZEBKIT_TEST_SANDBOX
    int child_code;
    if (zebkit_win_startup(&child_code))
        return child_code;
#endif
    wake = CreateEventW(NULL, FALSE, FALSE, NULL);
    zebkit_set_wakeup(wakeup, NULL);
    WNDCLASSW type = {0};
    type.lpfnWndProc = proc;
    type.hInstance = GetModuleHandleW(NULL);
    type.lpszClassName = L"zebkit-win-parent";
    type.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    RegisterClassW(&type);
    window = make_window(L"zebkit parent", 40, 40, 800, 600);
    if (zebkit_init(argc, argv))
        return 1;

    view = zebkit_view_new(page, 600, 400);
    int state = 0;
    zebkit_view_on_load(view, loaded, &state);
    zebkit_view_on_close(view, on_close, NULL);

    char *error = NULL;
    check(!zebkit_view_attach_native(view, 0, 0, 0, 100, 100, &error) && error, "no window: refused");
    zebkit_free(error), error = NULL;
    check(!zebkit_view_attach_native(view, 0x7ffffff0u, 0, 0, 100, 100, &error) && error, "a window that is not there: refused");
    zebkit_free(error), error = NULL;
    check(!zebkit_view_attach_native(view, (uintptr_t)window, 0, 0, 0, 100, &error) && error, "empty bounds: refused");
    zebkit_free(error), error = NULL;
    check(zebkit_view_attach_native(view, (uintptr_t)window, 40, 30, 600, 400, &error), "attached in the application's window");
    for (int i = 0; i < 3000 && !state; ++i)
        spin(5);
    check(state == 1, "the page loads");
    spin(1500);

    HWND child = GetWindow(window, GW_CHILD);
    check(child != NULL && (GetWindowLongPtrW(child, GWL_STYLE) & WS_CHILD), "the view's window is a child of the application's");
    RECT box;
    GetClientRect(child, &box);
    check(box.right == 600 && box.bottom == 400, "at the size asked for");

    foreground(window);
    /* (40,30) is where the view starts in the client area. */
    click(window, 40 + 60, 30 + 60);
    printf("after the click: %s\n", ask("JSON.stringify(log)"));
    check(strstr(answer, "click\\\",60,60,true") != NULL, "a real click reaches the page where it was, trusted");
    check(strstr(ask("document.activeElement.id"), "i") != NULL, "the click focused the page's field");
    check(GetFocus() != NULL && (GetFocus() == child || IsChild(child, GetFocus())), "the keyboard is in the view's window");
    key('Z');
    key('K');
    printf("after the keys: %s\n", ask("document.getElementById('i').value"));
    check(!strcmp(answer, "\"zk\""), "real keys type into the page");
    wheel(-WHEEL_DELTA);
    printf("after the wheel: %s\n", ask("JSON.stringify(log.filter(e=>e[0]=='wheel'))"));
    check(strstr(answer, "wheel\\\",1,true") != NULL, "the real wheel reaches the page");

    /* Moved and sized by the application. */
    zebkit_view_set_bounds(view, 10, 10, 500, 300);
    spin(800);
    GetClientRect(child, &box);
    check(box.right == 500 && box.bottom == 300, "zebkit_view_set_bounds()");
    check(!strcmp(ask("innerWidth+'x'+innerHeight"), "\"500x300\""), "the page has the new size");

    /* A click in the application's own part stays the application's. */
    click(window, 5, 5);
    check(strstr(ask("JSON.stringify(log.filter(e=>e[0]=='click').length)"), "1") != NULL, "a click beside the view does not reach the page");

    /* Detached: its own window again, input still works. */
    check(zebkit_view_detach_native(view), "zebkit_view_detach_native()");
    spin(500);
    check(GetParent(child) == NULL && !(GetWindowLongPtrW(child, GWL_STYLE) & WS_CHILD), "its own window again");
    check(IsWindowVisible(child), "shown");

    /* In a second window, whose end takes the view with it. */
    other = make_window(L"zebkit parent 2", 100, 100, 400, 300);
    check(zebkit_view_attach_native(view, (uintptr_t)other, 0, 0, 200, 150, &error), "attached again, in another window");
    spin(500);
    check(GetParent(child) == other, "with that window as its parent");
    DestroyWindow(other);
    for (int i = 0; i < 1000 && !closed; ++i)
        spin(5);
    check(closed, "the parent destroyed first takes the view with it: on_close");

    DestroyWindow(window);
    spin(200);
    check(zebkit_shutdown(), "zebkit_shutdown()");
    printf("%d of %d checks passed\n", checks - failures, checks);
    fflush(stdout);
    return failures ? 1 : 0;
}
