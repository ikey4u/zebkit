/*
 * cdphost for Windows: the smallest application that embeds the engine and
 * can be driven over the Chrome DevTools Protocol. It opens one page and
 * serves CDP until it is killed. It shows what an embedding application
 * has to do: create a view and call zebkit_cdp_listen().
 *
 *   cdphost [--port N] [--address IP] [--private] [--headless] [--size WxH] [URL]
 *
 * --private serves no discovery pages: a client needs the address printed
 * on standard error, which only this process and whoever reads its output
 * know.
 * --headless shows nothing: no entry in the taskbar, and a window that is
 * off every screen.
 *
 * Build against a built profile (scripts/cdp.sh does):
 *
 *   clang-cl cdphost-win.c -I src/cdp -I out/windows/<profile>/WebKit/Headers \
 *       out/windows/<profile>/lib/WebKit2.lib user32.lib
 */

/* WebKit's headers say bool and do not include this. */
#include <stdbool.h>

#include <WebKit/WebKit2_C.h>
#include <WebKit/WKContextConfigurationRef.h>
#include <WebKit/WKWebsiteDataStoreRef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <zebkit-cdp.h>

static WKViewRef view;

static LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message) {
    case WM_SIZE:
        /* The view is all there is in the window. */
        if (view)
            SetWindowPos(WKViewGetWindow(view), NULL, 0, 0, LOWORD(lParam), HIWORD(lParam), SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

int main(int argc, char **argv)
{
    const char *url = "about:blank";
    const char *address = NULL;
    unsigned port = 9222;
    int headless = 0, private = 0;
    int width = 1280, height = 720;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--port") && i + 1 < argc)
            port = (unsigned)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--address") && i + 1 < argc)
            address = argv[++i];
        else if (!strcmp(argv[i], "--headless"))
            headless = 1;
        else if (!strcmp(argv[i], "--private"))
            private = 1;
        else if (!strcmp(argv[i], "--size") && i + 1 < argc) {
            if (sscanf(argv[++i], "%dx%d", &width, &height) != 2) {
                fprintf(stderr, "cdphost: --size takes WIDTHxHEIGHT\n");
                return 2;
            }
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "usage: %s [--port N] [--address IP] [--private] [--headless] [--size WxH] [URL]\n", argv[0]);
            return 2;
        } else
            url = argv[i];
    }

    WNDCLASSEXW windowClass = { 0 };
    windowClass.cbSize = sizeof(windowClass);
    windowClass.hInstance = GetModuleHandleW(NULL);
    windowClass.lpszClassName = L"ZebkitCDPHost";
    windowClass.lpfnWndProc = windowProc;
    windowClass.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    RegisterClassExW(&windowClass);

    /* The size asked for is the page's, and a window has a frame around it. */
    DWORD style = headless ? WS_POPUP : WS_OVERLAPPEDWINDOW;
    DWORD extendedStyle = headless ? WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE : 0;
    RECT frame = { 0, 0, width, height };
    AdjustWindowRectEx(&frame, style, FALSE, extendedStyle);
    HWND window = CreateWindowExW(extendedStyle, windowClass.lpszClassName, L"zebkit", style,
        headless ? -20000 : CW_USEDEFAULT, headless ? -20000 : CW_USEDEFAULT, frame.right - frame.left, frame.bottom - frame.top,
        NULL, NULL, windowClass.hInstance, NULL);
    if (!window) {
        fprintf(stderr, "cdphost: cannot create a window\n");
        return 1;
    }

    /* Nothing is kept from one run to the next, and nothing is written to
     * the user's profile: cookies and caches are in memory. */
    WKContextConfigurationRef contextConfiguration = WKContextConfigurationCreate();
    WKContextRef context = WKContextCreateWithConfiguration(contextConfiguration);
    WKWebsiteDataStoreRef dataStore = WKWebsiteDataStoreCreateNonPersistentDataStore();
    WKPageConfigurationRef configuration = WKPageConfigurationCreate();
    WKPageConfigurationSetContext(configuration, context);
    WKPageConfigurationSetWebsiteDataStore(configuration, dataStore);

    RECT client = { 0, 0, width, height };
    view = WKViewCreate(client, configuration, window);
    /* What is hidden is not painted; a window off the screen is. */
    ShowWindow(window, headless ? SW_SHOWNOACTIVATE : SW_SHOW);
    SetFocus(WKViewGetWindow(view));

    if (private)
        zebkit_cdp_set_discovery_enabled(false);
    char *error = NULL;
    if (!zebkit_cdp_listen(address, port, &error)) {
        fprintf(stderr, "cdphost: %s\n", error ? error : "cannot listen");
        free(error);
        return 1;
    }

    WKURLRef location = WKURLCreateWithUTF8CString(url);
    WKPageLoadURL(WKViewGetPage(view), location);
    WKRelease(location);

    MSG message;
    while (GetMessageW(&message, NULL, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return 0;
}
