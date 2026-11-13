/* A Windows application with its own window and its own loop: zebkit_init()
 * and zebkit_pump() under the application's message loop, and a view inside
 * the application's HWND (zebkit_view_attach_native()). The page is pure red
 * on the left and pure blue on the right; what is on the screen is read back
 * with GDI, so the picture checked is the one a person would see.
 *
 * Exit 0 when every check passes. */

#include "harness.h"
#include <windows.h>
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "advapi32.lib")
#include <tlhelp32.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zebkit/zebkit.h>
#ifdef ZEBKIT_TEST_SANDBOX
#include <zebkit/win_sandbox.h>
#endif

static const char page[] =
    "data:text/html,<!doctype html><title>host</title><style>"
    "html,body{margin:0;height:100%25;background:%23ff0000}"
    "div{position:fixed;left:50%25;top:0;right:0;bottom:0;background:%230000ff}"
    "</style><div></div>";

static HWND window;
static HANDLE wake;
static int done;
static zebkit_view *view;

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

/* The engine's children and how far each is shut in: its token's integrity
 * level, and whether a job holds it. Returns the lowest level. */
static DWORD children(void) {
    DWORD lowest = 0xffffffff;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32W entry = {sizeof(entry)};
    for (BOOL more = Process32FirstW(snapshot, &entry); more; more = Process32NextW(snapshot, &entry)) {
        if (entry.th32ParentProcessID != GetCurrentProcessId())
            continue;
        HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID);
        HANDLE token = NULL;
        BYTE buffer[256];
        DWORD size = 0, level = 0xffffffff;
        BOOL job = FALSE;
        if (process && OpenProcessToken(process, TOKEN_QUERY, &token) &&
            GetTokenInformation(token, TokenIntegrityLevel, buffer, sizeof(buffer), &size)) {
            PSID sid = ((TOKEN_MANDATORY_LABEL *)buffer)->Label.Sid;
            level = *GetSidSubAuthority(sid, *GetSidSubAuthorityCount(sid) - 1);
        }
        if (process)
            IsProcessInJob(process, NULL, &job);
        printf("child %lu %ls: integrity 0x%lx, in a job %d\n", entry.th32ProcessID, entry.szExeFile, level, (int)job);
        if (level < lowest)
            lowest = level;
        if (token)
            CloseHandle(token);
        if (process)
            CloseHandle(process);
    }
    CloseHandle(snapshot);
    fflush(stdout);
    return lowest;
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
    /* The parent's pixels, at the display's scale (zebkit_view_attach_native). */
    check(json && !strcmp(json, "780"), "the page is the attached size, the client area less its margins", NULL);
    done = 1;
}

static void loaded(zebkit_view *v, zebkit_load_event event, const char *url, void *data) {
    (void)url;
    (void)data;
    if (event == ZEBKIT_LOAD_FAILED) {
        check(0, "the page loads", NULL);
        done = 2;
    } else if (event == ZEBKIT_LOAD_FINISHED) {
        check(1, "the page loads", NULL);
        zebkit_view_evaluate(v,
                             "new Promise(r => requestAnimationFrame(() => requestAnimationFrame(() => "
                             "setTimeout(() => r(Math.round(innerWidth * devicePixelRatio)), 1000))))",
                             evaluated, NULL);
    }
}

int main(int argc, char **argv) {
#ifdef ZEBKIT_TEST_SANDBOX
    /* Built with zebkit_sandbox.lib: this executable is the engine's sandboxed
     * children too. */
    int child_code;
    if (zebkit_win_startup(&child_code))
        return child_code;
#endif
    wake = CreateEventW(NULL, FALSE, FALSE, NULL);
    check(zebkit_set_wakeup(wakeup, NULL), "zebkit_set_wakeup() before the engine", NULL);

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
    check(window != NULL, "the application's window", NULL);

    char *error = NULL;
    const int code = zebkit_init(argc, argv);
    check(code == 0, "zebkit_init()", NULL);
    if (code)
        return code;

    view = zebkit_view_new(page, 780, 580);
    check(view != NULL, "zebkit_view_new()", NULL);
    zebkit_view_on_load(view, loaded, NULL);
    const bool attached = zebkit_view_attach_native(view, (uintptr_t)window, 10, 10, 780, 580, &error);
    check(attached, error ? error : "zebkit_view_attach_native()", NULL);

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
    check(done == 1, "the page answered, under the application's loop", NULL);

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
    check(margin == RGB(0, 0, 0), "the application's own window shows at the margin", NULL);
    /* Within what a display's colour handling takes away. */
    check(GetRValue(left) >= 250 && GetGValue(left) <= 5 && GetBValue(left) <= 5,
          "red on the screen, inside the application's window", NULL);
    check(GetRValue(right) <= 5 && GetGValue(right) <= 5 && GetBValue(right) >= 250,
          "blue on the screen, inside the application's window", NULL);

    const DWORD lowest = children();
#ifdef ZEBKIT_TEST_SANDBOX
    /* A renderer is at untrusted, the lowest there is. */
    check(lowest == 0, "a child at untrusted integrity: the sandbox is on", NULL);
#else
    (void)lowest;
#endif

    zebkit_view_close(view);
    view = NULL;
    for (int i = 0; i < 20; ++i) {
        Sleep(10);
        zebkit_pump();
    }
    check(zebkit_shutdown(), "zebkit_shutdown()", NULL);
    return summary();
}
