/*
 * Views with a window and off-screen views live in one process: frames and input
 * of each, a scale of each, no display or scale of one reaching another, the
 * off-screen ones with no window of the system's (on X11, seen from another
 * connection to the server). Under Xvfb on Linux; on macOS with real windows.
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <zebkit/offscreen.h>
#if defined(__APPLE__)
#include <IOSurface/IOSurfaceRef.h>
#endif
#if defined(MIXED_X11)
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <dlfcn.h>
#endif

static int failures, passed;
static void check(int ok, const char *label, const char *detail) {
    passed += ok;
    printf("%s %s%s%s\n", ok ? "ok  " : "FAIL", label, detail ? ": " : "", detail ? detail : "");
    fflush(stdout);
    if (!ok)
        ++failures;
}
static double now(void) {
    struct timespec value;
    clock_gettime(CLOCK_MONOTONIC, &value);
    return value.tv_sec + value.tv_nsec / 1e9;
}
static void step(void) {
    struct timespec pause = {0, 2000000};
    if (!zebkit_pump())
        ++failures;
    nanosleep(&pause, NULL);
}
static void steps(int count) {
    for (int i = 0; i < count; ++i)
        step();
}

/* What frames of a view and of its popup were last. */
struct picture {
    int count, width, height, popups;
    float scale;
    unsigned center; /* 0xRRGGBB */
};

static unsigned center_of(const zebkit_frame *frame) {
#if defined(__APPLE__)
    if (frame->kind == ZEBKIT_FRAME_IOSURFACE) {
        IOSurfaceRef surface = (IOSurfaceRef)frame->iosurface;
        IOSurfaceLock(surface, kIOSurfaceLockReadOnly, NULL);
        const unsigned char *pixel = (const unsigned char *)IOSurfaceGetBaseAddress(surface) +
                                     (size_t)(frame->height / 2) * IOSurfaceGetBytesPerRow(surface) +
                                     (size_t)(frame->width / 2) * 4;
        const unsigned value = (unsigned)pixel[2] << 16 | (unsigned)pixel[1] << 8 | pixel[0];
        IOSurfaceUnlock(surface, kIOSurfaceLockReadOnly, NULL);
        return value;
    }
#endif
    if (frame->kind != ZEBKIT_FRAME_SHARED_MEMORY || !frame->pixels)
        return 0xffffffffu;
    const unsigned char *pixel =
        frame->pixels + (size_t)(frame->height / 2) * frame->stride + (size_t)(frame->width / 2) * 4;
    return (unsigned)pixel[2] << 16 | (unsigned)pixel[1] << 8 | pixel[0];
}
static void on_frame(zebkit_view *view, const zebkit_frame *frame, void *data) {
    (void)view;
    struct picture *picture = data;
    if (frame->layer == ZEBKIT_LAYER_VIEW) {
        ++picture->count;
        picture->width = frame->width;
        picture->height = frame->height;
        picture->scale = frame->scale;
        picture->center = center_of(frame);
    } else {
        ++picture->popups;
    }
    zebkit_frame_release(frame);
}
/* ZEBKIT_TEST_WRONG_COLOUR (a run that must fail, to see that the check can):
   the page's colours expected swapped. */
static int same_colour(unsigned a, unsigned b) {
    static const char *wrong = (const char *)1;
    if (wrong == (const char *)1)
        wrong = getenv("ZEBKIT_TEST_WRONG_COLOUR");
    if (wrong && wrong != (const char *)1) {
        static int said;
        if (!said++) {
            printf("narrowed: ZEBKIT_TEST_WRONG_COLOUR=%s, colours expected swapped: this run must fail\n", wrong);
            fflush(stdout);
        }
    }
    if (wrong)
        b = (b & 0xff) << 16 | (b & 0xff00) | b >> 16;
    /* sRGB everywhere, the same bytes: one step for rounding. */
    for (int shift = 0; shift <= 16; shift += 8) {
        const int d = (int)((a >> shift) & 255) - (int)((b >> shift) & 255);
        if (d > 1 || d < -1)
            return 0;
    }
    return 1;
}
static void wait_picture(struct picture *picture, int width, int height, unsigned center) {
    double end = now() + 15;
    while (now() < end) {
        if (picture->count && picture->width == width && picture->height == height &&
            same_colour(picture->center, center))
            return;
        step();
    }
}
static int shown(const struct picture *picture, int width, int height, unsigned center, float scale) {
    const int same = picture->width == width && picture->height == height && picture->scale == scale &&
                     same_colour(picture->center, center);
    if (!same)
        printf("     %dx%d at %g, %06x; expected %dx%d at %g, %06x\n", picture->width, picture->height,
               picture->scale, picture->center, width, height, scale, center);
    return same;
}

struct result {
    int done;
    char value[512];
};
static void evaluated(zebkit_view *view, const char *json, const char *error, void *data) {
    (void)view;
    struct result *result = data;
    snprintf(result->value, sizeof(result->value), "%s", error ? error : json ? json : "null");
    result->done = 1;
}
static void read_page(zebkit_view *view, const char *script, struct result *result) {
    result->done = 0;
    zebkit_view_evaluate(view, script, evaluated, result);
    double end = now() + 15;
    while (!result->done && now() < end)
        step();
}
static void expect(zebkit_view *view, const char *script, const char *expected, const char *label) {
    struct result result = {0};
    read_page(view, script, &result);
    check(result.done && !strcmp(result.value, expected), label, result.value);
}
/* Until the page says it, for what input causes. */
static void expect_soon(zebkit_view *view, const char *script, const char *expected, const char *label) {
    struct result result = {0};
    double end = now() + 10;
    while (now() < end) {
        read_page(view, script, &result);
        if (result.done && !strcmp(result.value, expected))
            break;
        steps(5);
    }
    check(result.done && !strcmp(result.value, expected), label, result.value);
}
static void click_at(zebkit_view *view, float x, float y) {
    zebkit_view_send_mouse(view, ZEBKIT_MOUSE_MOVE, x, y, ZEBKIT_BUTTON_NONE, 0, 0);
    zebkit_view_send_mouse(view, ZEBKIT_MOUSE_DOWN, x, y, ZEBKIT_BUTTON_LEFT, 1, 0);
    zebkit_view_send_mouse(view, ZEBKIT_MOUSE_UP, x, y, ZEBKIT_BUTTON_LEFT, 1, 0);
}
static void wait_loaded(zebkit_view *view) {
    struct result result = {0};
    double end = now() + 20;
    while (now() < end) {
        read_page(view, "document.readyState", &result);
        if (result.done && !strcmp(result.value, "\"complete\""))
            return;
        steps(5);
    }
}

static zebkit_view *opened;
static void on_open(zebkit_view *opener, zebkit_view *view, const char *url, void *data) {
    (void)opener, (void)url, (void)data;
    opened = view;
}

#define PAGE(color)                                                                              \
    "data:text/html,<!doctype html><style>body{margin:0;background:" color "}"                   \
    "a{position:absolute;left:110px;top:4px}</style><style>"                                     \
    "select{position:absolute;left:10px;top:60px}</style>"                                       \
    "<script>window.clicked=0;window.keys='';"                                                   \
    "addEventListener('keydown',e=>{window.keys+=e.key})</script>"                               \
    "<button id=b style='position:absolute;left:20px;top:20px;width:60px;height:30px' "          \
    "onclick='++window.clicked'>click</button>"                                                  \
    "<a id=l href='about:blank'>x</a>"                                                           \
    "<select id=s><option>one<option>two<option>three</select>"

#define KEY_A(view, kind)                                       \
    do {                                                        \
        zebkit_key_event event = {0};                           \
        event.type = kind;                                      \
        event.key_code = 65;                                    \
        event.key = "a";                                        \
        event.code = "KeyA";                                    \
        event.text = "a";                                       \
        zebkit_view_send_key(view, &event);                     \
    } while (0)

#if defined(MIXED_X11)
/* From a connection of the test's own, as another program sees the screen. */
static Display *x_display;
static int count_windows(Window window) {
    Window root, parent, *children = NULL;
    unsigned count = 0;
    int total = 1;
    if (XQueryTree(x_display, window, &root, &parent, &children, &count)) {
        for (unsigned i = 0; i < count; ++i)
            total += count_windows(children[i]);
        if (children)
            XFree(children);
    }
    return total;
}
static int all_windows(void) {
    XSync(x_display, False);
    return count_windows(DefaultRootWindow(x_display));
}
/* The view's window: viewable and its size, the outermost found first. */
static Window find_sized(Window window, int width, int height) {
    XWindowAttributes attributes;
    if (XGetWindowAttributes(x_display, window, &attributes) && attributes.map_state == IsViewable &&
        attributes.width == width && attributes.height == height)
        return window;
    Window root, parent, *children = NULL, found = 0;
    unsigned count = 0;
    if (XQueryTree(x_display, window, &root, &parent, &children, &count)) {
        for (unsigned i = 0; i < count && !found; ++i)
            found = find_sized(children[i], width, height);
        if (children)
            XFree(children);
    }
    return found;
}
static int has_title(Window window, const char *name) {
    Atom type;
    int format, same = 0;
    unsigned long count, after;
    unsigned char *value = NULL;
    if (XGetWindowProperty(x_display, window, XInternAtom(x_display, "_NET_WM_NAME", False), 0, 256, False,
                           AnyPropertyType, &type, &format, &count, &after, &value) == Success && value) {
        same = !strcmp((const char *)value, name);
        XFree(value);
    }
    return same;
}
static unsigned window_center(Window window) {
    XWindowAttributes attributes;
    if (!XGetWindowAttributes(x_display, window, &attributes) || attributes.map_state != IsViewable)
        return 0;
    XImage *image = XGetImage(x_display, window, attributes.width / 2, attributes.height / 2, 1, 1,
                              AllPlanes, ZPixmap);
    if (!image)
        return 0;
    const unsigned pixel = (unsigned)XGetPixel(image, 0, 0) & 0xffffff;
    XDestroyImage(image);
    return pixel;
}
static unsigned wait_window_center(Window window, unsigned expected) {
    double end = now() + 10;
    unsigned pixel = 0;
    while (now() < end) {
        pixel = window_center(window);
        if (pixel == expected)
            break;
        steps(10);
    }
    return pixel;
}
typedef int (*fake_button_func)(Display *, unsigned, int, unsigned long);
typedef int (*fake_motion_func)(Display *, int, int, int, unsigned long);
static void click_window(Window window, int x, int y) {
    void *test = dlopen("libXtst.so.6", RTLD_NOW);
    fake_button_func button = test ? (fake_button_func)dlsym(test, "XTestFakeButtonEvent") : NULL;
    fake_motion_func motion = test ? (fake_motion_func)dlsym(test, "XTestFakeMotionEvent") : NULL;
    Window child;
    int rx, ry;
    XTranslateCoordinates(x_display, window, DefaultRootWindow(x_display), x, y, &rx, &ry, &child);
    if (button && motion) {
        motion(x_display, -1, rx, ry, 0);
        XSync(x_display, False);
        steps(20);
        button(x_display, 1, 1, 0);
        button(x_display, 1, 0, 0);
        XSync(x_display, False);
    } else {
        check(0, "XTEST to click the window with", NULL);
    }
}
#endif

int main(int argc, char **argv) {
    if (zebkit_init(argc, argv))
        return 2;
    check(!zebkit_is_headless(), "the process has a display", NULL);
    /* MIXED_SCALED: the screen's scale is not 1 (--force-device-scale-factor). */
    if (getenv("MIXED_SCALED")) {
        struct picture one = {0}, half = {0};
        zebkit_view *plain = zebkit_view_new(PAGE("rgb(1,1,1)"), 200, 100);
        zebkit_view *s1 = zebkit_view_new_offscreen(PAGE("rgb(200,10,20)"), 200, 100, 1);
        zebkit_view *s2 = zebkit_view_new_offscreen(PAGE("rgb(20,200,10)"), 200, 100, 1.5f);
        zebkit_view_on_frame(s1, on_frame, &one);
        zebkit_view_on_frame(s2, on_frame, &half);
        wait_loaded(plain);
        wait_loaded(s1);
        wait_loaded(s2);
        wait_picture(&one, 200, 100, 0xc80a14);
        wait_picture(&half, 300, 150, 0x14c80a);
        expect(plain, "devicePixelRatio", getenv("MIXED_SCALED"), "the window has the screen's scale");
        expect(s1, "[devicePixelRatio,innerWidth]", "[1,200]", "an off-screen view its own, 1");
        expect(s2, "[devicePixelRatio,innerWidth]", "[1.5,200]", "another its own, 1.5");
        check(shown(&one, 200, 100, 0xc80a14, 1), "frames at 1", NULL);
        check(shown(&half, 300, 150, 0x14c80a, 1.5f), "frames at 1.5", NULL);
        zebkit_view_close(plain);
        zebkit_view_close(s1);
        zebkit_view_close(s2);
        steps(50);
        check(zebkit_shutdown(), "the engine shuts down", NULL);
        printf("%d of %d checks passed\n", passed, passed + failures);
        return failures ? 1 : 0;
    }
#if defined(MIXED_X11)
    x_display = XOpenDisplay(NULL);
    check(x_display != NULL, "another connection to the display", NULL);
    if (!x_display)
        return 2;
    const int windows_before = all_windows();
#endif

    /* A window, and then the off-screen views while it is there. */
    zebkit_view *window = zebkit_view_new(PAGE("rgb(10,20,200)"), 300, 200);
    check(window != NULL, "a view with a window", NULL);
    if (!window)
        return 2;
    zebkit_view_set_title(window, "mixed-window");
    wait_loaded(window);
    struct result screen = {0}, density = {0};
    read_page(window, "[screen.width,screen.height]", &screen);
    read_page(window, "devicePixelRatio", &density);
    check(!zebkit_view_is_offscreen(window), "it is not off-screen", NULL);
#if defined(MIXED_X11)
    const int windows_with_one = all_windows();
    check(windows_with_one > windows_before, "it has a window on the display", NULL);
    Window x_window = 0;
    for (double end = now() + 10; !x_window && now() < end; steps(10))
        x_window = find_sized(DefaultRootWindow(x_display), 300, 200);
    check(x_window != 0, "found by its size", NULL);
    if (!x_window)
        return 1;
    steps(50);
    XSync(x_display, False);
    check(has_title(x_window, "mixed-window"), "with the title the application gave", NULL);
    char detail[64];
    unsigned pixel = wait_window_center(x_window, 0x0a14c8);
    snprintf(detail, sizeof detail, "%06x", pixel);
    check(pixel == 0x0a14c8, "the window shows its page", detail);
#endif

    struct picture first = {0}, second = {0}, third = {0};
    char third_detail[96];
    zebkit_view *a = zebkit_view_new_offscreen(PAGE("rgb(200,10,20)"), 200, 100, 1);
    zebkit_view *b = zebkit_view_new_offscreen(PAGE("rgb(20,200,10)"), 200, 100, 2);
    check(a && b, "off-screen views in a process with a window", NULL);
    if (!a || !b)
        return 2;
    check(zebkit_view_is_offscreen(a) && zebkit_view_is_offscreen(b), "they are off-screen", NULL);
    zebkit_view_on_frame(a, on_frame, &first);
    zebkit_view_on_frame(b, on_frame, &second);
    wait_loaded(a);
    wait_loaded(b);
    wait_picture(&first, 200, 100, 0xc80a14);
    wait_picture(&second, 400, 200, 0x14c80a);
    check(shown(&first, 200, 100, 0xc80a14, 1), "the first's frames: its size and colour at scale 1", NULL);
    check(shown(&second, 400, 200, 0x14c80a, 2), "the second's frames: its size and colour at scale 2", NULL);
    expect(a, "[devicePixelRatio,innerWidth,innerHeight]", "[1,200,100]", "the first's layout");
    expect(b, "[devicePixelRatio,innerWidth,innerHeight]", "[2,200,100]", "the second's layout, a scale of its own");
    expect(window, "devicePixelRatio", density.value, "the window keeps its scale");
    expect(window, "[screen.width,screen.height]", screen.value, "the window keeps its screen");
    expect(a, "document.visibilityState", "\"visible\"", "the off-screen page is visible");
    expect(window, "document.visibilityState", "\"visible\"", "the window's page is visible");
#if defined(MIXED_X11)
    check(all_windows() == windows_with_one, "off-screen views add no window to the display", NULL);
    pixel = wait_window_center(x_window, 0x0a14c8);
    snprintf(detail, sizeof detail, "%06x", pixel);
    check(pixel == 0x0a14c8, "the window still shows its page", detail);
#endif

    /* In a new document too, which may have a new renderer. */
    zebkit_view_load_url(b, PAGE("rgb(90,60,30)"));
    wait_picture(&second, 400, 200, 0x5a3c1e);
    check(shown(&second, 400, 200, 0x5a3c1e, 2), "the second's frames after a navigation", NULL);
    zebkit_view_load_url(b, PAGE("rgb(20,200,10)"));
    wait_loaded(b);
    wait_picture(&second, 400, 200, 0x14c80a);

    /* Input of each reaches it alone. */
    zebkit_view_set_focus(a, true);
    click_at(a, 40, 35);
    expect_soon(a, "clicked", "1", "a click in the first");
    zebkit_view_set_focus(b, true);
    click_at(b, 40, 35);
    expect_soon(b, "clicked", "1", "a click in the second, in CSS pixels at scale 2");
    expect(a, "clicked", "1", "the first has only its own");
    expect(window, "clicked", "0", "the window has none of theirs");
    KEY_A(b, ZEBKIT_KEY_DOWN);
    KEY_A(b, ZEBKIT_KEY_UP);
    expect_soon(b, "keys", "\"a\"", "a key to the second");
    expect(a, "keys", "\"\"", "not to the first");
    expect(window, "keys", "\"\"", "nor to the window");
#if defined(MIXED_X11)
    click_window(x_window, 40, 35);
    expect_soon(window, "clicked", "1", "the display's click in the window");
    expect(a, "clicked", "1", "no click of the display's in the first");
    expect(b, "clicked", "1", "nor in the second");
#endif

    /* Frames go on in each as the page changes. */
    const int first_frames = first.count;
    zebkit_view_evaluate(a, "document.body.style.background='rgb(30,40,50)'", NULL, NULL);
    wait_picture(&first, 200, 100, 0x1e2832);
    check(first.count > first_frames && shown(&first, 200, 100, 0x1e2832, 1), "a change in the first's page: a new frame", NULL);
    check(shown(&second, 400, 200, 0x14c80a, 2), "the second's frames as they were", NULL);
#if defined(MIXED_X11)
    zebkit_view_evaluate(window, "document.body.style.background='rgb(60,70,80)'", NULL, NULL);
    pixel = wait_window_center(x_window, 0x3c4650);
    snprintf(detail, sizeof detail, "%06x", pixel);
    check(pixel == 0x3c4650, "a change in the window's page: on the display", detail);
#endif

    /* A size and a scale of one touch nothing else. */
    zebkit_view_set_size(a, 320, 180, 1.5f);
    wait_picture(&first, 480, 270, 0x1e2832);
    check(shown(&first, 480, 270, 0x1e2832, 1.5f), "the first at a new size and a fractional scale", NULL);
    expect(a, "[devicePixelRatio,innerWidth,innerHeight]", "[1.5,320,180]", "its layout");
    expect(b, "[devicePixelRatio,innerWidth,innerHeight]", "[2,200,100]", "the second's unchanged");
    expect(window, "devicePixelRatio", density.value, "the window's unchanged");
    zebkit_view_resize(window, 420, 260);
    steps(100);
    expect(a, "[devicePixelRatio,innerWidth,innerHeight]", "[1.5,320,180]", "the first's unchanged by the window's size");
    zebkit_view_set_size(a, 3000, 90, 1);
    wait_picture(&first, 3000, 90, 0x1e2832);
    check(shown(&first, 3000, 90, 0x1e2832, 1), "the first larger than any screen", NULL);
    expect(a, "screen.width >= innerWidth && screen.height >= innerHeight", "true", "with a screen around it, as a window has");
    zebkit_view_set_size(a, 200, 100, 1);
    wait_picture(&first, 200, 100, 0x1e2832);

    /* A <select>'s list is a layer of the view's frames, not a window. */
    zebkit_view_set_focus(a, true);
    click_at(a, 30, 68);
    double end = now() + 10;
    while (!first.popups && now() < end)
        step();
    check(first.popups > 0, "a <select>'s list in the first's frames", NULL);
#if defined(MIXED_X11)
    check(all_windows() == windows_with_one, "with no window on the display", NULL);
#endif
    zebkit_view_send_key(a, &(zebkit_key_event){.type = ZEBKIT_KEY_DOWN, .key_code = 27, .key = "Escape", .code = "Escape"});
    zebkit_view_send_key(a, &(zebkit_key_event){.type = ZEBKIT_KEY_UP, .key_code = 27, .key = "Escape", .code = "Escape"});
    steps(50);

    /* What a window would show has nobody to show it. */
    expect(a, "alert('nobody sees this'),confirm('nor this')", "false", "alert() and confirm() in an off-screen page go on");
    /* What it opens is off-screen too, from a click. */
    zebkit_view_on_window_open(a, on_open, NULL);
    zebkit_view_evaluate(a, "document.getElementById('b').onclick=()=>{window.w=open('about:blank','_blank','width=150,height=120')}", NULL, NULL);
    steps(20);
    click_at(a, 40, 35);
    end = now() + 10;
    while (!opened && now() < end)
        step();
    check(opened && zebkit_view_is_offscreen(opened), "a window it opens is off-screen", NULL);
#if defined(MIXED_X11)
    check(all_windows() == windows_with_one, "and adds no window to the display", NULL);
#endif
    if (opened)
        zebkit_view_close(opened);
    steps(30);

    /* A click with a modifier opens without window.open: a view, off-screen, too. */
    opened = NULL;
    {
#if defined(__APPLE__)
        const unsigned modifier = ZEBKIT_MODIFIER_META;
#else
        const unsigned modifier = ZEBKIT_MODIFIER_CONTROL;
#endif
        zebkit_view_send_mouse(a, ZEBKIT_MOUSE_MOVE, 113, 12, ZEBKIT_BUTTON_NONE, 0, modifier);
        zebkit_view_send_mouse(a, ZEBKIT_MOUSE_DOWN, 113, 12, ZEBKIT_BUTTON_LEFT, 1, modifier);
        zebkit_view_send_mouse(a, ZEBKIT_MOUSE_UP, 113, 12, ZEBKIT_BUTTON_LEFT, 1, modifier);
    }
    end = now() + 10;
    while (!opened && now() < end)
        step();
    check(opened && zebkit_view_is_offscreen(opened), "a link opened with a modifier is an off-screen view", NULL);
#if defined(MIXED_X11)
    check(all_windows() == windows_with_one, "and adds no window to the display", NULL);
#endif
    if (opened) {
        expect(opened, "alert('nobody sees this'),1", "1", "with no dialog of the system's");
        zebkit_view_close(opened);
    }
    steps(30);

    /* The microphone behind speech recognition is asked for like any: refused here. */
    expect_soon(b,
                "(window.speech || (window.speech = new Promise(done => {"
                "const r = new webkitSpeechRecognition(); r.onerror = e => done(e.error);"
                "r.onstart = () => done('started'); r.start(); })), window.speechResult ||"
                " (window.speech.then(v => window.speechResult = v), 'waiting'))",
                "\"not-allowed\"", "speech recognition without the microphone's permission");

    /* devtools:// is a handled scheme with nothing behind it, the front end not
     * being in the package: an error page, and the page goes on. */
    {
        zebkit_view *tools = zebkit_view_new_offscreen("devtools://devtools/bundled/inspector.html", 200, 100, 1);
        steps(200);
        expect(tools, "1+1", "2", "devtools:// goes nowhere and harms nothing");
        zebkit_view_close(tools);
        steps(20);
    }

    /* Off-screen views go, again and again, with their roots. */
    for (int i = 0; i < 20; ++i) {
        struct picture brief = {0};
        zebkit_view *gone = zebkit_view_new_offscreen(PAGE("rgb(9,9,9)"), 200, 100, 1 + i % 3);
        zebkit_view_on_frame(gone, on_frame, &brief);
        for (int j = 0; j < 40 && !brief.count; ++j)
            steps(5);
        /* With a popup open every other time, which has a root too. */
        if (i % 2) {
            zebkit_view_set_focus(gone, true);
            click_at(gone, 30, 68);
            for (int j = 0; j < 40 && !brief.popups; ++j)
                steps(5);
        }
        zebkit_view_close(gone);
        steps(5);
    }
    check(1, "off-screen views made and closed twenty times", NULL);
    expect(b, "devicePixelRatio", "2", "the second as it was");

    /* Another window while they live; either end may be first. */
    zebkit_view *window2 = zebkit_view_new(PAGE("rgb(90,100,110)"), 200, 150);
    wait_loaded(window2);
    expect(window2, "devicePixelRatio", density.value, "a new window has the screen's scale");
    expect(b, "devicePixelRatio", "2", "the second's scale after a new window");
    zebkit_view_close(window);
    steps(50);
    zebkit_view_evaluate(b, "document.body.style.background='rgb(1,2,3)'", NULL, NULL);
    wait_picture(&second, 400, 200, 0x010203);
    check(shown(&second, 400, 200, 0x010203, 2), "frames go on after a window closes", NULL);
    zebkit_view_close(a);
    steps(50);
    expect(window2, "document.visibilityState", "\"visible\"", "the other window goes on after a view closes");
    zebkit_view *c = zebkit_view_new_offscreen(PAGE("rgb(5,6,7)"), 200, 100, 3);
    zebkit_view_on_frame(c, on_frame, &third);
    wait_picture(&third, 600, 300, 0x050607);
    snprintf(third_detail, sizeof third_detail, "%dx%d at %g, %06x, %d frames", third.width, third.height, third.scale, third.center, third.count);
    check(shown(&third, 600, 300, 0x050607, 3), "a new off-screen view after them", third_detail);
    zebkit_view_close(window2);
    zebkit_view_close(c);
    zebkit_view_close(b);
    steps(50);
#if defined(MIXED_X11)
    /* Chromium keeps a few windows of its own for the display (the clipboard's). */
    steps(100);
    check(all_windows() < windows_with_one, "the windows gone from the display", NULL);
    check(!find_sized(DefaultRootWindow(x_display), 420, 260), "the first window with them", NULL);
    XCloseDisplay(x_display);
#endif
    check(zebkit_shutdown(), "the engine shuts down", NULL);
    printf("%d of %d checks passed\n", passed, passed + failures);
    return failures ? 1 : 0;
}
