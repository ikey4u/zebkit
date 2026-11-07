/* A view's window inside a window of the application's (zebkit_view_attach_native),
 * as an Xlib host has it: the window tree, the page's pixels in the host's window,
 * real input through XTEST (XSendEvent where the server has no XTEST), keyboard focus,
 * bounds, visibility, the page's idea of the screen, a <select>'s popup, detaching, the
 * view closed first and the parent destroyed first. X11 is this program's: it is linked
 * here, as a host links it. Under --headless or ZEBKIT_TEST_REFUSED (a Wayland compositor),
 * the refusal and what it names. */
#define _DEFAULT_SOURCE
#include <zebkit/zebkit.h>
#include <zebkit/offscreen.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PAGE_COLOR 0x12a456ul
#define HOST_COLOR 0x202020ul

static const char PAGE[] =
    "data:text/html,<!doctype html><title>t</title>"
    "<body style='margin:0;background:%2312a456'>"
    "<select id=s style='position:fixed;left:8px;top:8px;width:100px;height:24px'>"
    "<option>a<option>b<option>c<option>d</select>"
    "<script>window.clicks=[];window.keys=[];"
    "addEventListener('click',e=>clicks.push(e.clientX,e.clientY));"
    "addEventListener('keydown',e=>keys.push(e.key));</script>";

static int failures, x_errors;
static uintptr_t standalone_window;
static Display *display;
static Window root;
static zebkit_view *view, *second, *third;
static Window host, second_host, third_host, third_panel, child, second_child;
static int closed_first, closed_second, closed_third;

typedef int (*fake_button_func)(Display *, unsigned, int, unsigned long);
typedef int (*fake_motion_func)(Display *, int, int, int, unsigned long);
typedef int (*fake_key_func)(Display *, unsigned, int, unsigned long);
static fake_button_func fake_button;
static fake_motion_func fake_motion;
static fake_key_func fake_key;
static int pointer_x, pointer_y;

static void check(int ok, const char *what, const char *detail) {
    printf("%s %s%s%s\n", ok ? "ok  " : "FAIL", what, detail ? ": " : "", detail ? detail : "");
    fflush(stdout);
    failures += !ok;
}

static int on_x_error(Display *d, XErrorEvent *event) {
    (void)d, (void)event;
    ++x_errors;
    return 0;
}

static Window make_host(int x, int y) {
    XSetWindowAttributes attributes;
    attributes.override_redirect = True; /* there is no window manager */
    attributes.background_pixel = HOST_COLOR;
    Window made = XCreateWindow(display, root, x, y, 640, 480, 0, CopyFromParent, InputOutput, CopyFromParent,
                                CWOverrideRedirect | CWBackPixel, &attributes);
    XMapWindow(display, made);
    XSync(display, False);
    return made;
}

/* The single child of @parent, or 0. */
static Window only_child(Window parent, unsigned *count) {
    Window r, p, *children = NULL, found = 0;
    unsigned n = 0;
    if (XQueryTree(display, parent, &r, &p, &children, &n) && n == 1)
        found = children[0];
    if (children)
        XFree(children);
    if (count)
        *count = n;
    return found;
}

static Window parent_of(Window window) {
    Window r, p = 0, *children = NULL;
    unsigned n = 0;
    XQueryTree(display, window, &r, &p, &children, &n);
    if (children)
        XFree(children);
    return p;
}

static int viewable(Window window) {
    XWindowAttributes a;
    return XGetWindowAttributes(display, window, &a) && a.map_state == IsViewable;
}

static int geometry_is(Window window, int x, int y, int width, int height) {
    XWindowAttributes a;
    return XGetWindowAttributes(display, window, &a) && a.x == x && a.y == y && a.width == width && a.height == height;
}

/* A pixel read from the host's window, what is drawn in its children included. */
static unsigned long pixel(Window window, int x, int y) {
    XImage *image = XGetImage(display, window, x, y, 1, 1, AllPlanes, ZPixmap);
    if (!image)
        return ~0ul;
    unsigned long value = XGetPixel(image, 0, 0) & 0xffffff;
    XDestroyImage(image);
    return value;
}

static void move_pointer(int x, int y) {
    pointer_x = x, pointer_y = y;
    if (fake_motion)
        fake_motion(display, 0, x, y, 0);
    XFlush(display);
}

static void send_button(int press) {
    if (fake_button) {
        fake_button(display, 1, press, 0);
    } else {
        Window window = child, unused;
        XEvent event;
        memset(&event, 0, sizeof event);
        event.xbutton.type = press ? ButtonPress : ButtonRelease;
        event.xbutton.window = window;
        event.xbutton.root = root;
        event.xbutton.x_root = pointer_x;
        event.xbutton.y_root = pointer_y;
        XTranslateCoordinates(display, root, window, pointer_x, pointer_y, &event.xbutton.x, &event.xbutton.y, &unused);
        event.xbutton.button = Button1;
        event.xbutton.same_screen = True;
        XSendEvent(display, window, True, press ? ButtonPressMask : ButtonReleaseMask, &event);
    }
    XFlush(display);
}

static void send_key(KeySym symbol) {
    unsigned code = XKeysymToKeycode(display, symbol);
    if (fake_key) {
        fake_key(display, code, 1, 0);
        fake_key(display, code, 0, 0);
    } else {
        Window focus;
        int revert;
        XGetInputFocus(display, &focus, &revert);
        XEvent event;
        memset(&event, 0, sizeof event);
        event.xkey.type = KeyPress;
        event.xkey.window = focus;
        event.xkey.root = root;
        event.xkey.keycode = code;
        event.xkey.same_screen = True;
        XSendEvent(display, focus, True, KeyPressMask, &event);
        event.xkey.type = KeyRelease;
        XSendEvent(display, focus, True, KeyReleaseMask, &event);
    }
    XFlush(display);
}

/* Script's answer, asked again until it says @expected. */
static zebkit_view *asked_view;
static char *answer;
static int waiting;
static void on_answer(zebkit_view *v, const char *json, const char *error, void *data) {
    (void)v, (void)data;
    free(answer);
    answer = strdup(json ? json : error ? error : "");
    waiting = 0;
}
static int says(zebkit_view *v, const char *script, const char *expected) {
    if (waiting)
        return 0;
    if (answer && asked_view == v && strcmp(answer, expected) == 0) {
        free(answer);
        answer = NULL;
        return 1;
    }
    asked_view = v;
    waiting = 1;
    zebkit_view_evaluate(v, script, on_answer, NULL);
    return 0;
}

static void on_close(zebkit_view *v, void *data) {
    (void)v;
    ++*(int *)data;
}

static int stage, tries, loaded;
static void on_load(zebkit_view *v, zebkit_load_event event, const char *url, void *data) {
    (void)v, (void)url;
    if (event == ZEBKIT_LOAD_FINISHED)
        ++*(int *)data;
}

static Window focused(void) {
    Window focus;
    int revert;
    XGetInputFocus(display, &focus, &revert);
    return focus;
}

/* How many top-level windows are shown (the engine's popup among them). */
static int shown_at_root(void) {
    Window r, p, *children = NULL;
    unsigned n = 0;
    int shown = 0;
    XQueryTree(display, root, &r, &p, &children, &n);
    for (unsigned i = 0; i < n; i++)
        shown += viewable(children[i]);
    if (children)
        XFree(children);
    return shown;
}
static int shown_before;

/* Beyond the views' own windows, which start at the corner of the screen. */
#define HOST_X 700
#define HOST_Y 80
#define CHECK_THEN(condition, what) \
    if (!(condition)) break; \
    check(1, what, NULL); \
    next = 1

static void step(void *data);
static void later(int ms) { zebkit_post_delayed_task(step, NULL, (uint32_t)ms); }

static void step(void *data) {
    (void)data;
    char *error = NULL;
    int next = 0;
    XSync(display, False);
    switch (stage) {
    case 0: /* The refusals that need no display of its own. */
        if (!loaded) break;
        zebkit_native_window window = {ZEBKIT_NATIVE_WINDOW_NONE, 0};
        check(zebkit_view_get_native_window(view, &window) && window.kind == ZEBKIT_NATIVE_WINDOW_X11 && window.handle,
              "native window: real standalone X11 window", NULL);
        standalone_window = window.handle;
        XWindowAttributes native_attributes;
        check(window.handle && window.handle <= UINT32_MAX &&
              XGetWindowAttributes(display, (Window)window.handle, &native_attributes),
              "native window: standalone handle is a valid X11 resource", NULL);

        check(!zebkit_view_get_native_window(view, NULL), "native window: NULL output refused", NULL);

        check(!zebkit_view_attach_native(view, 0, 0, 0, 100, 100, &error) && error, "no window: refused", error);
        zebkit_free(error), error = NULL;
        check(!zebkit_view_attach_native(view, 0x7ffffff0u, 0, 0, 100, 100, &error) && error, "a window that is not there: refused", error);
        zebkit_free(error), error = NULL;
        check(!zebkit_view_attach_native(view, host, 0, 0, 0, 100, &error) && error, "empty bounds: refused", error);
        zebkit_free(error), error = NULL;
        check(zebkit_view_attach_native(view, host, 40, 30, 400, 300, &error), "attached in the host's window", error);
        check(zebkit_view_get_native_window(view, &window) && window.kind == ZEBKIT_NATIVE_WINDOW_X11 && window.handle == host,
              "native window: attached page reports host window", NULL);

        check(!zebkit_view_detach_native(second), "a view in no window of the host's does not detach", NULL);
        next = 1;
        break;
    case 1:
        child = only_child(host, NULL);
        CHECK_THEN(child && geometry_is(child, 40, 30, 400, 300), "XQueryTree: the view's window is the host's child, where it was put");
        break;
    case 2:
        CHECK_THEN(says(view, "JSON.stringify([innerWidth,innerHeight])", "\"[400,300]\""), "Aura's root follows: the page is 400 by 300");
        break;
    case 3:
        CHECK_THEN(pixel(host, 240, 180) == PAGE_COLOR && pixel(host, 40, 299) == PAGE_COLOR && pixel(host, 439, 329) == PAGE_COLOR,
                   "XGetImage: the page's pixels in the host's window, to its corners");
        break;
    case 4:
        if (!tries) check(pixel(host, 39, 30) == HOST_COLOR && pixel(host, 440, 329) == HOST_COLOR && pixel(host, 40, 330) == HOST_COLOR,
              "XGetImage: the host's own pixels around it", NULL);
        CHECK_THEN(says(view, "JSON.stringify([screenX,screenY])", "\"[740,110]\""), "the page's screen position is the true one");
        break;
    case 5: /* The host has the keyboard, as a window manager gives it. */
        XSetInputFocus(display, host, RevertToParent, CurrentTime);
        move_pointer(HOST_X + 40 + 50, HOST_Y + 30 + 60);
        next = 1;
        break;
    case 6:
        send_button(1);
        send_button(0);
        next = 1;
        break;
    case 7:
        CHECK_THEN(says(view, "JSON.stringify(clicks)", "\"[50,60]\""), "a click: the page saw it where it was");
        break;
    case 8:
        CHECK_THEN(focused() == child, "the click took the keyboard from the host");
        break;
    case 9:
        move_pointer(650, 700); /* out of the view: only focus can bring the key */
        send_key(XK_a);
        next = 1;
        break;
    case 10:
        CHECK_THEN(says(view, "JSON.stringify([keys,document.hasFocus()])", "\"[[\\\"a\\\"],true]\""), "a key with the pointer elsewhere reaches the page");
        break;
    case 11:
        XSetInputFocus(display, host, RevertToParent, CurrentTime);
        next = 1;
        break;
    case 12:
        CHECK_THEN(says(view, "String(document.hasFocus())", "\"false\""), "the host took the keyboard back: the page knows");
        break;
    case 13:
        zebkit_view_focus(view);
        next = 1;
        break;
    case 14:
        CHECK_THEN(focused() == child, "zebkit_view_focus: the view's window has the keyboard");
        break;
    case 15:
        zebkit_view_set_bounds(view, 10, 20, 300, 200);
        next = 1;
        break;
    case 16:
        CHECK_THEN(geometry_is(child, 10, 20, 300, 200) && pixel(host, 150, 100) == PAGE_COLOR && pixel(host, 400, 300) == HOST_COLOR,
                   "zebkit_view_set_bounds: moved and resized, pixels too");
        break;
    case 17:
        CHECK_THEN(says(view, "JSON.stringify([innerWidth,innerHeight,screenX,screenY])", "\"[300,200,710,100]\""), "the page's new size and position");
        break;
    case 18:
        zebkit_view_resize(view, 320, 240);
        next = 1;
        break;
    case 19:
        CHECK_THEN(geometry_is(child, 10, 20, 320, 240), "zebkit_view_resize keeps it in place");
        break;
    case 20:
        CHECK_THEN(says(view, "JSON.stringify([innerWidth,innerHeight])", "\"[320,240]\""), "the page's size after zebkit_view_resize");
        break;
    case 21:
        XMoveWindow(display, host, HOST_X + 60, HOST_Y + 50);
        next = 1;
        break;
    case 22:
        CHECK_THEN(says(view, "JSON.stringify([screenX,screenY])", "\"[770,150]\""), "the host moved: the page's screen position follows");
        break;
    case 23:
        move_pointer(HOST_X + 60 + 10 + 200, HOST_Y + 50 + 20 + 100);
        next = 1;
        break;
    case 24:
        send_button(1);
        send_button(0);
        next = 1;
        break;
    case 25:
        CHECK_THEN(says(view, "JSON.stringify(clicks)", "\"[50,60,200,100]\""), "a click after both moved: where it was");
        break;
    case 26: /* A <select>'s list opens as a window of the root's, under the control. */
        shown_before = shown_at_root();
        move_pointer(HOST_X + 60 + 10 + 30, HOST_Y + 50 + 20 + 16);
        next = 1;
        break;
    case 27:
        send_button(1);
        send_button(0);
        next = 1;
        break;
    case 28:
        CHECK_THEN(shown_at_root() > shown_before, "a <select>'s list opens");
        break;
    case 29:
        send_key(XK_Escape);
        next = 1;
        break;
    case 30:
        CHECK_THEN(shown_at_root() == shown_before, "and closes");
        break;
    case 31:
        zebkit_view_set_visible(view, false);
        next = 1;
        break;
    case 32:
        CHECK_THEN(!viewable(child) && pixel(host, 150, 100) == HOST_COLOR, "zebkit_view_set_visible(false): the host's pixels only");
        break;
    case 33:
        zebkit_view_set_visible(view, true);
        next = 1;
        break;
    case 34:
        CHECK_THEN(viewable(child) && pixel(host, 150, 100) == PAGE_COLOR, "zebkit_view_set_visible(true): the page again");
        break;
    case 35: /* Another view and another host at once. */
        if (loaded < 2) break;
        check(zebkit_view_attach_native(second, second_host, 0, 0, 200, 150, &error), "a second view in a second host", error);
        next = 1;
        break;
    case 36:
        second_child = only_child(second_host, NULL);
        CHECK_THEN(second_child && pixel(second_host, 100, 100) == PAGE_COLOR, "its pixels are there");
        break;
    case 37:
        if (zebkit_view_detach_native(second)) {
            check(1, "zebkit_view_detach_native", NULL);
            zebkit_native_window detached = {ZEBKIT_NATIVE_WINDOW_NONE, 0};
            check(zebkit_view_get_native_window(second, &detached) && detached.kind == ZEBKIT_NATIVE_WINDOW_X11 &&
                  detached.handle && detached.handle != second_host && detached.handle != standalone_window,
                  "native window: detaching restores own window", NULL);
            next = 1;
        }

        break;
    case 38: {
        unsigned count;
        only_child(second_host, &count);
        CHECK_THEN(count == 0 && parent_of(second_child) == root && viewable(second_child), "detached: a window of the root's again, shown");
        break;
    }
    case 39:
        check(zebkit_view_attach_native(second, second_host, 5, 5, 200, 150, &error), "attached again", error);
        next = 1;
        break;
    case 40:
        CHECK_THEN(only_child(second_host, NULL) && pixel(second_host, 100, 100) == PAGE_COLOR, "and draws there");
        break;
    case 41: /* The view closed first. */
        zebkit_view_close(view);
        next = 1;
        break;
    case 42: {
        unsigned count;
        only_child(host, &count);
        CHECK_THEN(closed_first == 1 && count == 0 && pixel(host, 150, 100) == HOST_COLOR, "the view closed first: gone from a host that goes on");
        break;
    }
    case 43: /* The parent destroyed first. */
        XDestroyWindow(display, second_host);
        second_host = 0;
        next = 1;
        break;
    case 44:
        CHECK_THEN(closed_second == 1, "the parent destroyed first: the view closes");
        break;
    case 45: /* The engine and its GPU process go on. */
        if (loaded < 3) break;
        third_host = make_host(1000, 500);
        /* Attached to a panel inside the application's window, as an
         * application with toolbars around the page attaches it. */
        third_panel = XCreateSimpleWindow(display, third_host, 0, 0, 640, 480, 0, 0, HOST_COLOR);
        XMapWindow(display, third_panel);
        XSync(display, False);
        check(zebkit_view_attach_native(third, third_panel, 0, 0, 300, 200, &error), "a view attached after both", error);
        {
            zebkit_native_window native = {ZEBKIT_NATIVE_WINDOW_NONE, 0};
            check(zebkit_view_get_native_window(third, &native) && native.kind == ZEBKIT_NATIVE_WINDOW_X11 &&
                  native.handle == third_host, "native window: the top-level window, not the panel it is in", NULL);
        }
        next = 1;
        break;
    case 46:
        child = only_child(third_panel, NULL);
        CHECK_THEN(child && pixel(third_host, 150, 150) == PAGE_COLOR, "it draws");
        break;
    case 47:
        move_pointer(1000 + 150, 500 + 100);
        next = 1;
        break;
    case 48:
        send_button(1);
        send_button(0);
        next = 1;
        break;
    case 49:
        CHECK_THEN(says(third, "JSON.stringify(clicks)", "\"[150,100]\""), "and takes a click");
        break;
    case 50:
        check(x_errors == 0, "the host's connection saw no X error", NULL);
        zebkit_view_close(third);
        next = 1;
        break;
    case 51:
        CHECK_THEN(closed_third == 1, "closed");
        break;
    default:
        printf("%d failures\n", failures);
        fflush(stdout);
        zebkit_quit();
        return;
    }
    if (next) {
        stage++;
        tries = 0;
        later(stage == 6 || stage == 24 || stage == 27 || stage == 48 ? 300 : 20);
        return;
    }
    if (++tries > 200) {
        char what[64];
        snprintf(what, sizeof what, "step %d timed out", stage);
        check(0, what, answer);
        free(answer);
        answer = NULL;
        waiting = 0;
        printf("%d failures\n", failures);
        fflush(stdout);
        zebkit_quit();
        return;
    }
    later(100);
}

static void refused_ready(void *data) {
    (void)data;
    char *error = NULL;
    zebkit_view *plain = zebkit_view_new("about:blank", 200, 100);
    zebkit_native_window window = {ZEBKIT_NATIVE_WINDOW_APPKIT, 123};
    check(!zebkit_view_get_native_window(plain, &window) && window.kind == ZEBKIT_NATIVE_WINDOW_NONE && !window.handle,
          "native window: unavailable under headless or Wayland, output cleared", NULL);
    window = (zebkit_native_window){ZEBKIT_NATIVE_WINDOW_APPKIT, 123};
    check(!zebkit_view_get_native_window(NULL, &window) && window.kind == ZEBKIT_NATIVE_WINDOW_NONE && !window.handle,
          "native window: NULL view clears output", NULL);

    int refused = !zebkit_view_attach_native(plain, 1, 0, 0, 100, 100, &error);
    check(refused && error && strstr(error, "off-screen"), "the window cannot be put in another: refused, naming off-screen views", error);
    zebkit_free(error), error = NULL;
    check(!zebkit_view_detach_native(plain), "nothing to detach", NULL);
    zebkit_view_close(plain);
    zebkit_view *offscreen = zebkit_view_new_offscreen("about:blank", 200, 100, 1.0f);
    check(offscreen != NULL, "an off-screen view", NULL);
    check(!zebkit_view_get_native_window(offscreen, &window) && window.kind == ZEBKIT_NATIVE_WINDOW_NONE && !window.handle,
          "native window: off-screen view has no system window", NULL);

    refused = offscreen && !zebkit_view_attach_native(offscreen, 1, 0, 0, 100, 100, &error);
    check(refused && error && strstr(error, "off-screen"), "an off-screen view: refused", error);
    zebkit_free(error);
    if (offscreen)
        zebkit_view_close(offscreen);
    zebkit_quit();
}

static void ready(void *data) {
    (void)data;
    if (zebkit_is_headless() || getenv("ZEBKIT_TEST_REFUSED")) {
        refused_ready(NULL);
        return;
    }
    display = XOpenDisplay(NULL);
    if (!display) {
        check(0, "the display", NULL);
        zebkit_quit();
        return;
    }
    XSetErrorHandler(on_x_error);
    root = DefaultRootWindow(display);
    void *test = dlopen("libXtst.so.6", RTLD_NOW);
    int event_base, error_base, major, minor;
    typedef int (*query_func)(Display *, int *, int *, int *, int *);
    query_func query = test ? (query_func)dlsym(test, "XTestQueryExtension") : NULL;
    if (query && query(display, &event_base, &error_base, &major, &minor)) {
        fake_button = (fake_button_func)dlsym(test, "XTestFakeButtonEvent");
        fake_motion = (fake_motion_func)dlsym(test, "XTestFakeMotionEvent");
        fake_key = (fake_key_func)dlsym(test, "XTestFakeKeyEvent");
        check(1, "input through XTEST", NULL);
    } else {
        check(1, "no XTEST: input through XSendEvent", NULL);
    }
    host = make_host(HOST_X, HOST_Y);
    second_host = make_host(1400, 60);
    view = zebkit_view_new(PAGE, 640, 480);
    zebkit_view_on_load(view, on_load, &loaded);
    zebkit_view_on_close(view, on_close, &closed_first);
    second = zebkit_view_new(PAGE, 320, 240);
    zebkit_view_on_load(second, on_load, &loaded);
    zebkit_view_on_close(second, on_close, &closed_second);
    third = zebkit_view_new(PAGE, 320, 240);
    zebkit_view_on_load(third, on_load, &loaded);
    zebkit_view_on_close(third, on_close, &closed_third);
    later(100);
}

int main(int argc, char **argv) {
    int code = zebkit_main(argc, argv, ready, NULL);
    if (display)
        XCloseDisplay(display);
    return code || failures ? 1 : 0;
}
