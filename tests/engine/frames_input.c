/*
 * Input to the frames of a page in an off-screen view, as an application
 * that integrates the engine sends it (<zebkit/offscreen.h>, <zebkit/page.h>),
 * the frames as a prototype viewer has its screens: scaled by CSS transforms
 * (0.5 to 2), inside boxes that scroll; of the page's origin, from srcdoc,
 * from a blob: URL, sandboxed without allow-same-origin, of another site (in
 * a process of its own) holding one of the page's site, and of another site
 * whose pointer-events the page turned off and on
 * (tests/engine/pages/frames-input.html). In each: hover and its cursor, a
 * click, a right click, text selected by dragging, focus by a click and
 * typing, an input method's composition and commit, Tab to the next field,
 * the wheel over a scrolled box, a <select>'s list opened and chosen from, a
 * drop from outside; then focus back out to the page. What each frame saw is
 * posted to the page, which the test reads.
 *
 * Built with FRAMES_X11 and run with ZEBKIT_TEST_WINDOW=1 under an X server,
 * the page is in a view with a window of the engine's own instead, and the
 * input is the system's (XTEST), as a person's: the steps an X server cannot
 * make (an input method, a drop from outside) are left out, and those that
 * read what only an off-screen view reports (its cursor, its popup).
 * ZEBKIT_TEST_SCALE=2 gives the off-screen view twice the pixels.
 */
#include <zebkit/offscreen.h>
#include <zebkit/page.h>
#include <zebkit/zebkit.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#if defined(FRAMES_X11)
#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <dlfcn.h>
#endif

static zebkit_view *view;
static int failures, checks;

static void check(int ok, const char *what, const char *format, ...) {
    ++checks;
    printf("%s %s", ok ? "ok  " : "FAIL", what);
    if (format && !ok) {
        va_list args;
        va_start(args, format);
        printf(": ");
        vprintf(format, args);
        va_end(args);
    }
    printf("\n");
    fflush(stdout);
    if (!ok)
        ++failures;
}

static int64_t now_us(void) {
    struct timespec ts;
    timespec_get(&ts, TIME_UTC);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

/* ---- The view's callbacks ---------------------------------------------------- */

static int frames;
static void on_frame(zebkit_view *v, const zebkit_frame *frame, void *data) {
    (void)v, (void)data;
    ++frames;
    zebkit_frame_release(frame);
}

static char cursor[64] = "";
static void on_cursor(zebkit_view *v, const char *name, const zebkit_cursor_image *image, void *data) {
    (void)v, (void)image, (void)data;
    snprintf(cursor, sizeof(cursor), "%s", name ? name : "");
}

static int popup_shown;
static zebkit_rect popup_rect;
static void on_popup(zebkit_view *v, bool shown, const zebkit_rect *bounds, void *data) {
    (void)v, (void)data;
    popup_shown = shown;
    if (bounds)
        popup_rect = *bounds;
}

/* ---- The frames ------------------------------------------------------------- */

/* Where each frame is in the view, and its scale: the page's frames_(). */
struct frame {
    char name[32];
    double x, y, scale;
};
static struct frame all_frames[16];
static size_t frame_count;
static const struct frame *frame;

/* The middles of the frame's parts, in the frame's CSS pixels; TEXT is the
 * start of its line of text. */
enum part { BUTTON, FIELD, PICK, SCROLLER, DROP, TEXT };
static const double parts[][2] = {{65, 17}, {65, 44}, {65, 70}, {65, 110}, {65, 154}, {7, 183}};
static double px(enum part part) { return frame->x + parts[part][0] * frame->scale; }
static double py(enum part part) { return frame->y + parts[part][1] * frame->scale; }

/* The engine's own window, input from the X server (ZEBKIT_TEST_WINDOW=1). */
static int windowed;
#if defined(FRAMES_X11)
static Display *x_display;
static Window x_window;
typedef int (*fake_button_func)(Display *, unsigned, int, unsigned long);
typedef int (*fake_motion_func)(Display *, int, int, int, unsigned long);
typedef int (*fake_key_func)(Display *, unsigned, int, unsigned long);
static fake_button_func x_button;
static fake_motion_func x_motion;
static fake_key_func x_key;
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
static void x_move(double x, double y) {
    Window child;
    int rx, ry;
    XTranslateCoordinates(x_display, x_window, DefaultRootWindow(x_display), (int)x, (int)y, &rx, &ry, &child);
    x_motion(x_display, -1, rx, ry, 0);
    XSync(x_display, False);
}
static void x_press(unsigned button, int down) {
    x_button(x_display, button, down, 0);
    XSync(x_display, False);
}
static void x_keysym(KeySym sym) {
    const unsigned code = XKeysymToKeycode(x_display, sym);
    x_key(x_display, code, 1, 0);
    x_key(x_display, code, 0, 0);
    XSync(x_display, False);
}
#endif

static void move(double x, double y) {
#if defined(FRAMES_X11)
    if (windowed) {
        x_move(x, y);
        return;
    }
#endif
    zebkit_view_send_mouse(view, ZEBKIT_MOUSE_MOVE, x, y, ZEBKIT_BUTTON_NONE, 0, 0);
}
static void press(double x, double y, zebkit_mouse_button button) {
#if defined(FRAMES_X11)
    if (windowed) {
        x_move(x, y);
        x_press(button == ZEBKIT_BUTTON_RIGHT ? 3 : 1, 1);
        x_press(button == ZEBKIT_BUTTON_RIGHT ? 3 : 1, 0);
        return;
    }
#endif
    unsigned held = button == ZEBKIT_BUTTON_RIGHT ? ZEBKIT_MODIFIER_RIGHT_BUTTON : ZEBKIT_MODIFIER_LEFT_BUTTON;
    move(x, y);
    zebkit_view_send_mouse(view, ZEBKIT_MOUSE_DOWN, x, y, button, 1, held);
    zebkit_view_send_mouse(view, ZEBKIT_MOUSE_UP, x, y, button, 1, 0);
}
static void click(double x, double y) { press(x, y, ZEBKIT_BUTTON_LEFT); }

static void key(int code, const char *dom_code, const char *dom_key, const char *text) {
#if defined(FRAMES_X11)
    if (windowed) {
        KeySym sym = code == 0x09 ? XK_Tab : code == 0x0D ? XK_Return : code == 0x28 ? XK_Down
                     : (KeySym)(code - 'A' + 'a');
        x_keysym(sym);
        return;
    }
#endif
    zebkit_key_event event = {ZEBKIT_KEY_DOWN, 0, code, 0, dom_code, dom_key, NULL, NULL};
    zebkit_view_send_key(view, &event);
    if (text) {
        event.type = ZEBKIT_KEY_CHAR;
        event.text = event.unmodified_text = text;
        zebkit_view_send_key(view, &event);
    }
    event.type = ZEBKIT_KEY_UP;
    event.text = event.unmodified_text = NULL;
    zebkit_view_send_key(view, &event);
}

/* ---- Actions ---------------------------------------------------------------- */

static void a_hover(void) {
    cursor[0] = 0;
    move(px(BUTTON) - 20, py(BUTTON));
    move(px(BUTTON), py(BUTTON));
}
static void a_click(void) { click(px(BUTTON), py(BUTTON)); }
static void a_right_click(void) { press(px(BUTTON), py(BUTTON), ZEBKIT_BUTTON_RIGHT); }
static uint32_t drag_ms(void) {
    const char *pace = getenv("ZEBKIT_TEST_DRAG_MS");
    return pace ? (uint32_t)atoi(pace) : 8;
}
static double drag_x, drag_y, drag_to;
static int drag_step;
static void drag_next(void *data) {
    (void)data;
    if (++drag_step <= 5) {
        zebkit_view_send_mouse(view, ZEBKIT_MOUSE_MOVE, drag_x + (drag_to - drag_x) * drag_step / 5, drag_y,
                               ZEBKIT_BUTTON_NONE, 0, ZEBKIT_MODIFIER_LEFT_BUTTON);
        zebkit_post_delayed_task(drag_next, NULL, drag_ms());
        return;
    }
    zebkit_view_send_mouse(view, ZEBKIT_MOUSE_UP, drag_to, drag_y, ZEBKIT_BUTTON_LEFT, 1, 0);
}
/* A drag over the line of text, from its start to past its middle. */
static void a_select_text(void) {
    const double x = px(TEXT), y = py(TEXT), to = x + 100 * frame->scale;
    move(x, y);
#if defined(FRAMES_X11)
    if (windowed) {
        x_press(1, 1);
        for (int i = 1; i <= 5; ++i)
            x_move(x + (to - x) * i / 5, y);
        x_press(1, 0);
        return;
    }
#endif
    zebkit_view_send_mouse(view, ZEBKIT_MOUSE_DOWN, x, y, ZEBKIT_BUTTON_LEFT, 1, ZEBKIT_MODIFIER_LEFT_BUTTON);
    /* The moves 8 ms apart, as a mouse's come; ZEBKIT_TEST_DRAG_MS another pace, 0 all
     * at once, in the task of the press (on macOS a frame of another process then
     * selects only to the first move: the open case in docs/API.md). */
    const char *pace = getenv("ZEBKIT_TEST_DRAG_MS");
    if (!pace || atoi(pace) > 0) {
        drag_x = x, drag_y = y, drag_to = to, drag_step = 0;
        zebkit_post_delayed_task(drag_next, NULL, drag_ms());
        return;
    }
    for (int i = 1; i <= 5; ++i)
        zebkit_view_send_mouse(view, ZEBKIT_MOUSE_MOVE, x + (to - x) * i / 5, y, ZEBKIT_BUTTON_NONE, 0,
                               ZEBKIT_MODIFIER_LEFT_BUTTON);
    zebkit_view_send_mouse(view, ZEBKIT_MOUSE_UP, to, y, ZEBKIT_BUTTON_LEFT, 1, 0);
}
static void a_focus_field(void) { click(px(FIELD), py(FIELD)); }
static void a_type(void) {
    key(0x41, "KeyA", "a", "a");
    key(0x42, "KeyB", "b", "b");
}
static void a_compose(void) { zebkit_view_ime_set_composition(view, "ni", -1, -1); }
static void a_commit(void) { zebkit_view_ime_commit(view, "你"); }
static void a_tab(void) { key(0x09, "Tab", "Tab", NULL); }
static void a_wheel(void) {
#if defined(FRAMES_X11)
    if (windowed) {
        x_move(px(SCROLLER), py(SCROLLER));
        x_press(5, 1);
        x_press(5, 0);
        return;
    }
#endif
    zebkit_view_send_wheel(view, px(SCROLLER), py(SCROLLER), 0, 53, false, ZEBKIT_WHEEL_PHASE_NONE, 0);
}
static void a_trackpad(void) {
#if defined(FRAMES_X11)
    if (windowed) {
        x_move(px(SCROLLER), py(SCROLLER));
        for (int i = 0; i < 4; ++i)
            x_press(5, 1), x_press(5, 0);
        return;
    }
#endif
    for (int i = 0; i < 4; ++i)
        zebkit_view_send_wheel(view, px(SCROLLER), py(SCROLLER), 0, 40, true,
                               i ? ZEBKIT_WHEEL_PHASE_CHANGED : ZEBKIT_WHEEL_PHASE_BEGAN, 0);
    zebkit_view_send_wheel(view, px(SCROLLER), py(SCROLLER), 0, 0, true, ZEBKIT_WHEEL_PHASE_ENDED, 0);
}
static void a_open_select(void) { click(px(PICK), py(PICK)); }
static void a_choose(void) {
    key(0x28, "ArrowDown", "ArrowDown", NULL);
    key(0x0D, "Enter", "Enter", NULL);
}
static void a_open_select_again(void) { a_open_select(); }
static void a_choose_with_mouse(void) {
    /* The third item: the list's last third. */
    click(popup_rect.x + popup_rect.width / 2.0, popup_rect.y + popup_rect.height * 5.0 / 6.0);
}
static void dropped(zebkit_view *v, unsigned operation, void *data) { (void)v, (void)operation, (void)data; }
static void a_drop(void) {
    const double x = px(DROP), y = py(DROP);
    zebkit_view_drag_enter(view, "{\"text\": \"hello\"}", x - 30, y, ZEBKIT_DRAG_ALL, 0);
    zebkit_view_drag_over(view, x - 10, y, ZEBKIT_DRAG_ALL, 0, NULL, NULL);
    zebkit_view_drag_over(view, x, y, ZEBKIT_DRAG_ALL, 0, NULL, NULL);
    zebkit_view_drop(view, x, y, 0, dropped, NULL);
}
/* Out of the frame: a click on the page between the boxes, and a key. */
static void a_leave(void) { click(392, 440); }
static void a_type_in_page(void) { key(0x5A, "KeyZ", "z", "z"); }

/* ---- What each frame should have seen ----------------------------------------- */

static int cursor_is_pointer(void) { return strcmp(cursor, "pointer") == 0; }
static int popup_is_shown(void) { return popup_shown && popup_rect.height > 20; }
static int popup_is_gone(void) { return !popup_shown; }

/* A step: an action, then a wait for a script (in the page, '@' the frame's
 * name) to be true, or for a condition of the test's. */
struct step {
    void (*action)(void);
    const char *expect;
    int (*condition)(void);
    const char *what;
    /* Only for an off-screen view: what the system's input cannot make, or
     * only an off-screen view reports. */
    int offscreen_only;
};
static const struct step frame_steps[] = {
    {a_hover, "has('@:mouseover:button')", NULL, "hover reaches the frame's button"},
    {NULL, NULL, cursor_is_pointer, "the cursor over the frame's button is the frame's", 1},
    {a_click, "has('@:click:button')", NULL, "a click reaches the frame's button"},
    {a_right_click, "has('@:contextmenu:button')", NULL, "a right click is the frame's context menu event"},
    {a_select_text, "(last('@', 'selection') || '').startsWith('select th')", NULL,
     "a drag selects the frame's text"},
    {a_focus_field, "has('@:focus:field')", NULL, "a click focuses the frame's field"},
    {a_type, "last('@', 'value') == 'ab'", NULL, "keys type into the frame's field"},
    {a_compose, "has('@:composing:ni') && last('@', 'value') == 'abni'", NULL,
     "an input method's composition is in the frame's field", 1},
    {a_commit, "last('@', 'value') == 'ab你'", NULL, "the composition committed in the frame's field", 1},
    {a_tab, "has('@:blur:field') && has('@:focus:pick')", NULL, "Tab moves focus to the frame's next field"},
    {a_wheel, "Number(last('@', 'scroll')) > 0", NULL, "the wheel scrolls the frame's box"},
    {a_trackpad, "Number(last('@', 'scroll')) > 100", NULL, "a trackpad's scroll scrolls the frame's box"},
    {a_open_select, NULL, popup_is_shown, "the frame's <select> opens its list", 1},
    {a_choose, "has('@:change:b')", popup_is_gone, "chosen from the frame's list with the keyboard", 1},
    {a_open_select_again, NULL, popup_is_shown, "the frame's list again", 1},
    {a_choose_with_mouse, "has('@:change:c')", popup_is_gone, "chosen from the frame's list with the mouse", 1},
    {a_drop, "has('@:drop:hello')", NULL, "text dropped from outside onto the frame", 1},
    {a_focus_field, "log.filter((e) => e == '@:focus:field').length == 2", NULL, "the frame's field focused again"},
    {a_leave, "has('@:blur:field')", NULL, "a click on the page takes focus out of the frame"},
    {a_type_in_page, "has('top:keydown') && !has('@:keydown:z')", NULL,
     "keys go to the page again, not to the frame"},
};

/* ---- Running them ---------------------------------------------------------- */

static size_t frame_index, step_index;
static const struct step *step;
static int64_t deadline;
static char script[1024];

static void next_step(void *data);
static void poll(void *data);

static void finish(void) {
    printf("%d of %d checks passed\n", checks - failures, checks);
    zebkit_quit();
}

static void report_seen(zebkit_view *v, const char *json, const char *error, void *data) {
    (void)v, (void)data;
    printf("     the frame's last events: %s\n", json ? json : error ? error : "");
    ++step_index;
    zebkit_post_delayed_task(next_step, NULL, 50);
}

static void evaluated(zebkit_view *v, const char *json, const char *error, void *data) {
    (void)v, (void)data;
    const int done = !error && json && strcmp(json, "true") == 0;
    const int condition = !step->condition || step->condition();
    if (done && condition) {
        check(1, step->what, NULL);
    } else if (now_us() < deadline) {
        zebkit_post_delayed_task(poll, NULL, 30);
        return;
    } else {
        check(0, step->what, "%s%s in %s", error ? error : json ? json : "",
              condition ? "" : " (the test's condition false)", frame->name);
        /* What the frame saw last, to tell why. */
        char seen[256];
        snprintf(seen, sizeof(seen), "JSON.stringify(log.filter((e) => e.startsWith('%s:')).slice(-8))",
                 frame->name);
        zebkit_view_evaluate(view, seen, report_seen, NULL);
        return;
    }
    ++step_index;
    /* A moment between steps, as between a person's actions. */
    zebkit_post_delayed_task(next_step, NULL, 50);
}

static void poll(void *data) {
    (void)data;
    zebkit_view_evaluate(view, step->expect ? script : "true", evaluated, NULL);
}

static void expand(const char *expect) {
    size_t n = 0;
    for (const char *c = expect; *c && n + 32 < sizeof(script); ++c) {
        if (*c == '@')
            n += (size_t)snprintf(script + n, sizeof(script) - n, "%s", frame->name);
        else
            script[n++] = *c;
    }
    script[n] = 0;
}

static void next_step(void *data) {
    (void)data;
    if (step_index == sizeof(frame_steps) / sizeof(frame_steps[0])) {
        step_index = 0;
        if (++frame_index == frame_count) {
            finish();
            return;
        }
    }
    frame = &all_frames[frame_index];
    step = &frame_steps[step_index];
    if (windowed && step->offscreen_only) {
        ++step_index;
        zebkit_post_delayed_task(next_step, NULL, 0);
        return;
    }
    if (step->expect)
        expand(step->expect);
    if (step->action)
        step->action();
    deadline = now_us() + 4000 * 1000;
    zebkit_post_delayed_task(poll, NULL, 30);
}

/* ---- Start: the page and its frames ready ------------------------------------ */

static int64_t ready_deadline;
static void wait_ready(void *data);
/* "\"name x y scale;...\"" */
static void placed(zebkit_view *v, const char *json, const char *error, void *data) {
    (void)v, (void)data;
    const char *at = json && !error && *json == '"' ? json + 1 : "";
    while (*at && *at != '"' && frame_count < sizeof(all_frames) / sizeof(all_frames[0])) {
        struct frame *f = &all_frames[frame_count];
        int used = 0;
        if (sscanf(at, "%31s %lf %lf %lf%n", f->name, &f->x, &f->y, &f->scale, &used) != 4)
            break;
        ++frame_count;
        at += used;
        if (*at == ';')
            ++at;
    }
    check(frame_count == 7, "the page says where its seven frames are", "%s", json ? json : error ? error : "");
    if (frame_count == 0) {
        finish();
        return;
    }
    zebkit_post_delayed_task(next_step, NULL, 300);
}
static void ready_answer(zebkit_view *v, const char *json, const char *error, void *data) {
    (void)v, (void)data;
#if defined(FRAMES_X11)
    if (windowed && !x_window) {
        x_window = find_sized(DefaultRootWindow(x_display), 1200, 900);
        if (!x_window && now_us() < ready_deadline) {
            zebkit_post_delayed_task(wait_ready, NULL, 50);
            return;
        }
        check(x_window != 0, "the view's window on the display", NULL);
        if (!x_window) {
            finish();
            return;
        }
    }
#endif
    if (!error && json && strcmp(json, "true") == 0 && (frames > 0 || windowed)) {
        check(1, "the page and its frames drawn", NULL);
        zebkit_view_evaluate(view, "frames_()", placed, NULL);
    } else if (now_us() < ready_deadline) {
        zebkit_post_delayed_task(wait_ready, NULL, 50);
    } else {
        check(0, "the page and its frames drawn", "%s, %d frames", error ? error : json ? json : "", frames);
        finish();
    }
}
static void wait_ready(void *data) {
    (void)data;
    zebkit_view_evaluate(view,
                         "allReady() || JSON.stringify([[...ready], window.toggledOn,"
                         " [...document.querySelectorAll('.box')].map((box) => box.scrollTop)])",
                         ready_answer, NULL);
}

static void timeout(void *data) {
    (void)data;
    check(0, "the test finished in time", NULL);
    finish();
}

static void start(void *data) {
    (void)data;
    const char *base = getenv("ZEBKIT_TEST_BASE");
    const char *other = getenv("ZEBKIT_TEST_OTHER");
    char url[512];
    snprintf(url, sizeof(url), "%s/frames-input.html%s%s", base,
             other ? "?other=" : "", other ? other : "");
    if (windowed) {
        view = zebkit_view_new(url, 1200, 900);
        check(view != NULL, "zebkit_view_new", NULL);
    } else {
        /* ZEBKIT_TEST_SCALE: the view's scale (device pixels per CSS pixel). */
        const char *scale = getenv("ZEBKIT_TEST_SCALE");
        view = zebkit_view_new_offscreen(url, 1200, 900, scale ? (float)atof(scale) : 1);
        check(view != NULL, "zebkit_view_new_offscreen", NULL);
    }
    if (!view) {
        zebkit_quit();
        return;
    }
    if (!windowed) {
        zebkit_view_on_frame(view, on_frame, NULL);
        zebkit_view_on_cursor(view, on_cursor, NULL);
        zebkit_view_on_popup(view, on_popup, NULL);
        zebkit_view_set_focus(view, true);
    }
    ready_deadline = now_us() + 20 * 1000 * 1000;
    zebkit_post_delayed_task(wait_ready, NULL, 200);
    zebkit_post_delayed_task(timeout, NULL, 600 * 1000);
}

int main(int argc, char **argv) {
    if (!getenv("ZEBKIT_TEST_BASE")) {
        fprintf(stderr, "set ZEBKIT_TEST_BASE: http://127.0.0.1:<port>, serving tests/engine/pages\n");
        return 2;
    }
    windowed = getenv("ZEBKIT_TEST_WINDOW") != NULL;
#if defined(FRAMES_X11)
    if (windowed) {
        x_display = XOpenDisplay(NULL);
        void *test = dlopen("libXtst.so.6", RTLD_NOW);
        x_button = test ? (fake_button_func)dlsym(test, "XTestFakeButtonEvent") : NULL;
        x_motion = test ? (fake_motion_func)dlsym(test, "XTestFakeMotionEvent") : NULL;
        x_key = test ? (fake_key_func)dlsym(test, "XTestFakeKeyEvent") : NULL;
        if (!x_display || !x_button || !x_motion || !x_key) {
            printf("FAIL an X server and XTEST\n");
            return 1;
        }
    }
#else
    if (windowed) {
        printf("FAIL ZEBKIT_TEST_WINDOW: built without FRAMES_X11\n");
        return 1;
    }
#endif
    int code = zebkit_main(argc, argv, start, NULL);
    if (code)
        return code;
    return failures || checks < 20 ? 1 : 0;
}
