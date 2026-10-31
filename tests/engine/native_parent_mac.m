/* A view on an NSView of the application's (zebkit_view_attach_native), as an AppKit
 * host has it: the view tree, the page's pixels in the host's window, a click and a key
 * at the views AppKit would give them to (so that no real pointer or key of the person at
 * the Mac is involved), keyboard focus, bounds in a flipped and an unflipped
 * parent, visibility, detaching, the view closed first and the parent gone first. A
 * page's GPU: WebGL's renderer in an attached view.
 *
 * The pixels come from screencapture, which needs the Screen Recording permission of
 * whatever started this. Without it they are skipped, and that is said: the window is
 * not worth a prompt. A <select>'s list is the system's modal menu and is not tried. */
#define _DARWIN_C_SOURCE
#import <Cocoa/Cocoa.h>
#include <zebkit/zebkit.h>
#include <zebkit/offscreen.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char PAGE[] =
    "data:text/html,<!doctype html><title>t</title>"
    "<body style='margin:0;background:%2312a456'>"
    "<script>window.clicks=[];window.keys=[];"
    "addEventListener('click',e=>clicks.push(e.clientX,e.clientY));"
    "addEventListener('keydown',e=>keys.push(e.key));</script>";

/* Key as far as the views can tell, so that nothing takes the keyboard from the person
 * at the Mac: a background program is not activated anyway, and in a window that is not
 * key a first click only raises it. */
@interface HostWindow : NSWindow
@end
@implementation HostWindow
- (BOOL)isKeyWindow { return YES; }
- (BOOL)canBecomeKeyWindow { return YES; }
@end

@interface FlippedView : NSView
@end
@implementation FlippedView
- (BOOL)isFlipped { return YES; }
- (BOOL)acceptsFirstResponder { return YES; }
@end

static int failures, passed, skipped_pixels, can_see_pixels = -1;
static NSWindow *host, *second_host, *third_host;
static NSView *container, *second_container, *third_container, *page, *second_page;
static zebkit_view *view, *second, *third;
static int closed_first, closed_second, closed_third, loaded;
static int stage, tries;
static double start_x, start_y;

static void check(int ok, const char *what, const char *detail) {
    printf("%s %s%s%s\n", ok ? "ok  " : "FAIL", what, detail ? ": " : "", detail ? detail : "");
    fflush(stdout);
    failures += !ok;
    passed += !!ok;
}

static NSWindow *make_host(NSRect frame, NSView *content) {
    NSWindow *window = [[HostWindow alloc] initWithContentRect:frame
                                                   styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable
                                                     backing:NSBackingStoreBuffered
                                                       defer:NO];
    window.releasedWhenClosed = NO;
    window.title = @"zebkit test: closes by itself";
    content.wantsLayer = YES;
    content.layer.backgroundColor = CGColorGetConstantColor(kCGColorBlack);
    content.frame = NSMakeRect(0, 0, frame.size.width, frame.size.height);
    window.contentView = content;
    [window orderFront:nil];
    return window;
}

/* The window's picture, in the display's colours; nil when it cannot be had. */
static NSBitmapImageRep *capture(NSWindow *window) {
    char path[1024], command[1200];
    const char *directory = getenv("TMPDIR");
    snprintf(path, sizeof path, "%s/native-parent-%d.png", directory ? directory : "/tmp", getpid());
    snprintf(command, sizeof command, "/usr/sbin/screencapture -x -o -t png -l%ld '%s' 2>/dev/null", (long)window.windowNumber, path);
    if (system(command) != 0)
        return nil;
    NSData *data = [NSData dataWithContentsOfFile:[NSString stringWithUTF8String:path]];
    unlink(path);
    return data ? [NSBitmapImageRep imageRepWithData:data] : nil;
}

/* The pixel at a point of a view of @window, as 0 to 255. */
static int pixel(NSBitmapImageRep *image, NSWindow *window, NSView *in, double x, double y, double out[3]) {
    NSPoint point = [in convertPoint:NSMakePoint(x, y) toView:nil];
    double scale = (double)image.pixelsWide / window.frame.size.width;
    NSInteger px = (NSInteger)(point.x * scale), py = (NSInteger)(image.pixelsHigh - point.y * scale);
    if (px < 0 || py < 0 || px >= image.pixelsWide || py >= image.pixelsHigh)
        return 0;
    NSColor *color = [[image colorAtX:px y:py] colorUsingColorSpace:NSColorSpace.sRGBColorSpace];
    if (!color)
        return 0;
    out[0] = color.redComponent * 255, out[1] = color.greenComponent * 255, out[2] = color.blueComponent * 255;
    return 1;
}
static int is_page(const double c[3]) { return c[1] > 100 && c[1] > c[0] + 40 && c[1] > c[2] + 20; }
static int is_host(const double c[3]) { return c[0] < 50 && c[1] < 50 && c[2] < 50; }

/* Is the page at (px, py) and the host at (hx, hy)? 1 yes, 0 not yet, -1 cannot be seen. */
static int pixels_show(NSWindow *window, NSView *in, double px, double py, double hx, double hy) {
    if (can_see_pixels == 0)
        return -1;
    NSBitmapImageRep *image = capture(window);
    double a[3], b[3];
    if (!image || !pixel(image, window, in, px, py, a) || !pixel(image, window, in, hx, hy, b)) {
        can_see_pixels = 0;
        return -1;
    }
    if (can_see_pixels < 0) {
        /* Without the permission the picture holds no window's contents. */
        can_see_pixels = is_host(b);
        if (!can_see_pixels)
            return -1;
    }
    return is_page(a) && is_host(b);
}

/* Passes when shown; a pixel check that cannot run moves on, counted. */
#define PIXELS(window, in, px, py, hx, hy, what)                       \
    {                                                                  \
        int seen = pixels_show(window, in, px, py, hx, hy);            \
        if (seen == 0) break;                                          \
        if (seen < 0) ++skipped_pixels, printf("skip %s: no picture of the window\n", what), fflush(stdout); \
        else check(1, what, NULL);                                     \
        next = 1;                                                      \
    }

static char *answer;
static int waiting;
static void on_answer(zebkit_view *v, const char *json, const char *error, void *data) {
    (void)v, (void)data;
    free(answer);
    answer = strdup(json ? json : error ? error : "");
    waiting = 0;
}
/* Script's answer, asked again until it says @expected. */
static int says(zebkit_view *v, const char *script, const char *expected) {
    if (waiting)
        return 0;
    if (answer && strcmp(answer, expected) == 0) {
        free(answer);
        answer = NULL;
        return 1;
    }
    waiting = 1;
    zebkit_view_evaluate(v, script, on_answer, NULL);
    return 0;
}

static void on_close(zebkit_view *v, void *data) {
    (void)v;
    ++*(int *)data;
}
static void on_load(zebkit_view *v, zebkit_load_event event, const char *url, void *data) {
    (void)v, (void)url;
    if (event == ZEBKIT_LOAD_FINISHED)
        ++*(int *)data;
}

/* A left click at a point of the view, as NSWindow's sendEvent: hands it on: the view
 * under it becomes first responder and gets mouseDown: and mouseUp:. The window's own
 * sendEvent: keeps a click in a window of a program that is not active, and the system's
 * event source would be the person's pointer. */
static void click(NSWindow *window, NSView *in, double x, double y) {
    NSPoint point = [in convertPoint:NSMakePoint(x, y) toView:nil];
    NSView *frame = window.contentView.superview;
    NSView *under = [frame hitTest:[frame convertPoint:point fromView:nil]];
    if (under.acceptsFirstResponder)
        [window makeFirstResponder:under];
    for (int i = 0; i < 2; i++) {
        NSEvent *event = [NSEvent mouseEventWithType:i ? NSEventTypeLeftMouseUp : NSEventTypeLeftMouseDown
                                            location:point
                                       modifierFlags:0
                                           timestamp:NSProcessInfo.processInfo.systemUptime
                                        windowNumber:window.windowNumber
                                             context:nil
                                         eventNumber:i
                                          clickCount:1
                                            pressure:i ? 0 : 1];
        if (i)
            [under mouseUp:event];
        else
            [under mouseDown:event];
    }
}

static void press_a(NSWindow *window) {
    for (int i = 0; i < 2; i++) {
        NSEvent *event = [NSEvent keyEventWithType:i ? NSEventTypeKeyUp : NSEventTypeKeyDown
                                          location:NSZeroPoint
                                     modifierFlags:0
                                         timestamp:NSProcessInfo.processInfo.systemUptime
                                      windowNumber:window.windowNumber
                                           context:nil
                                        characters:@"a"
                       charactersIgnoringModifiers:@"a"
                                         isARepeat:NO
                                           keyCode:0];
        if (i)
            [window.firstResponder keyUp:event];
        else
            [window.firstResponder keyDown:event];
    }
}

static int frame_is(NSView *v, double x, double y, double w, double h) {
    NSRect f = v.frame;
    return f.origin.x == x && f.origin.y == y && f.size.width == w && f.size.height == h;
}

/* What is on the screen of this process. */
static int visible_windows(void) {
    int shown = 0;
    for (NSWindow *window in NSApp.windows)
        shown += window.visible;
    return shown;
}

#define CHECK_THEN(condition, what) \
    if (!(condition)) break;        \
    check(1, what, NULL);           \
    next = 1

static void step(void *data);
static void later(int ms) { zebkit_post_delayed_task(step, NULL, (uint32_t)ms); }

static void step(void *data) {
    (void)data;
    char *error = NULL;
    int next = 0;
    switch (stage) {
    case 0:
        if (loaded < 3) break;
        check(!zebkit_view_attach_native(view, 0, 0, 0, 100, 100, &error) && error, "no parent: refused", error);
        zebkit_free(error), error = NULL;
        check(!zebkit_view_attach_native(view, (uintptr_t)(__bridge void *)container, 0, 0, 0, 100, &error) && error, "empty bounds: refused", error);
        zebkit_free(error), error = NULL;
        check(!zebkit_view_detach_native(view), "nothing to detach yet", NULL);
        check(zebkit_view_attach_native(view, (uintptr_t)(__bridge void *)container, 40, 30, 400, 300, &error), "attached on the host's NSView", error);
        zebkit_free(error), error = NULL;
        next = 1;
        break;
    case 1:
        page = container.subviews.lastObject;
        CHECK_THEN(page && container.subviews.count == 1 && frame_is(page, 40, 30, 400, 300),
                   "the page's NSView is the host's subview, where it was put (flipped parent)");
        break;
    case 2:
        /* The three hosts, and the windows of the two views not attached yet. */
        CHECK_THEN(visible_windows() == 5, "the view's own window is not on the screen");
        break;
    case 3:
        CHECK_THEN(says(view, "JSON.stringify([innerWidth,innerHeight])", "\"[400,300]\""), "the page is 400 by 300");
        break;
    case 4:
        PIXELS(host, container, 240, 180, 20, 20, "the page's pixels and the host's around them");
        break;
    case 5:
        if (can_see_pixels == 1) {
            NSBitmapImageRep *image = capture(host);
            double c[3];
            int corners = image != nil;
            const double points[][2] = {{41, 31}, {438, 31}, {41, 328}, {438, 328}};
            for (int i = 0; corners && i < 4; i++)
                corners = pixel(image, host, container, points[i][0], points[i][1], c) && is_page(c);
            const double outside[][2] = {{38, 180}, {442, 180}, {240, 28}, {240, 332}};
            for (int i = 0; corners && i < 4; i++)
                corners = pixel(image, host, container, outside[i][0], outside[i][1], c) && is_host(c);
            if (!corners) break;
            check(1, "drawn to its corners, no further", NULL);
        }
        next = 1;
        break;
    case 6:
        [host makeFirstResponder:container];
        next = 1;
        break;
    case 7:
        click(host, container, 40 + 50, 30 + 60);
        next = 1;
        break;
    case 8:
        CHECK_THEN(says(view, "JSON.stringify(clicks)", "\"[50,60]\""), "a click: the page saw it where it was");
        break;
    case 9:
        CHECK_THEN(host.firstResponder == page || [host.firstResponder isKindOfClass:NSView.class] && [(NSView *)host.firstResponder isDescendantOf:page],
                   "the click took the keyboard from the host");
        break;
    case 10:
        press_a(host);
        next = 1;
        break;
    case 11:
        CHECK_THEN(says(view, "JSON.stringify([keys,document.hasFocus()])", "\"[[\\\"a\\\"],true]\""), "a key reaches the page");
        break;
    case 12:
        [host makeFirstResponder:container];
        next = 1;
        break;
    case 13:
        CHECK_THEN(says(view, "String(document.hasFocus())", "\"false\""), "the host took the keyboard back: the page knows");
        break;
    case 14:
        zebkit_view_focus(view);
        next = 1;
        break;
    case 15:
        CHECK_THEN(says(view, "String(document.hasFocus())", "\"true\"") && [(NSView *)host.firstResponder isDescendantOf:page],
                   "zebkit_view_focus: the page has the keyboard");
        break;
    case 16:
        zebkit_view_set_bounds(view, 10, 20, 300, 200);
        next = 1;
        break;
    case 17:
        CHECK_THEN(frame_is(page, 10, 20, 300, 200), "zebkit_view_set_bounds: moved and resized");
        break;
    case 18:
        CHECK_THEN(says(view, "JSON.stringify([innerWidth,innerHeight])", "\"[300,200]\""), "the page's new size");
        break;
    case 19:
        PIXELS(host, container, 150, 100, 400, 300, "pixels at the new bounds");
        break;
    case 20:
        zebkit_view_resize(view, 320, 240);
        next = 1;
        break;
    case 21:
        CHECK_THEN(frame_is(page, 10, 20, 320, 240), "zebkit_view_resize keeps it in place");
        break;
    case 22:
        CHECK_THEN(says(view, "JSON.stringify([innerWidth,innerHeight])", "\"[320,240]\""), "the page's size after zebkit_view_resize");
        break;
    case 23:
        start_x = host.frame.origin.x, start_y = host.frame.origin.y;
        click(host, container, 10 + 200, 20 + 100);
        next = 1;
        break;
    case 24:
        CHECK_THEN(says(view, "JSON.stringify(clicks)", "\"[50,60,200,100]\""), "a click after the resize: where it was");
        break;
    case 25:
        [host setFrameOrigin:NSMakePoint(start_x + 60, start_y - 50)];
        next = 1;
        break;
    case 26:
        click(host, container, 10 + 100, 20 + 50);
        next = 1;
        break;
    case 27:
        CHECK_THEN(says(view, "JSON.stringify(clicks.slice(-2))", "\"[100,50]\""), "the host moved: a click is still where it was");
        break;
    case 28:
        zebkit_view_set_visible(view, false);
        next = 1;
        break;
    case 29:
        CHECK_THEN(page.hidden, "zebkit_view_set_visible(false)");
        break;
    case 30:
        if (can_see_pixels == 1) {
            NSBitmapImageRep *image = capture(host);
            double c[3];
            if (!image || !pixel(image, host, container, 150, 100, c) || !is_host(c)) break;
            check(1, "hidden: the host's pixels only", NULL);
        }
        next = 1;
        break;
    case 31:
        zebkit_view_set_visible(view, true);
        next = 1;
        break;
    case 32:
        PIXELS(host, container, 150, 100, 400, 300, "shown again: the page's pixels");
        break;
    case 33: /* Another view and another host, an unflipped parent: (x, y) from its top left all the same. */
        check(zebkit_view_attach_native(second, (uintptr_t)(__bridge void *)second_container, 10, 20, 200, 150, &error),
              "a second view in a second host, its parent not flipped", error);
        zebkit_free(error), error = NULL;
        next = 1;
        break;
    case 34:
        second_page = second_container.subviews.lastObject;
        CHECK_THEN(second_page && frame_is(second_page, 10, second_container.bounds.size.height - 20 - 150, 200, 150), "at that place from its top left");
        break;
    case 35:
        PIXELS(second_host, second_container, 100, second_container.bounds.size.height - 90, 300, 20, "its pixels are there");
        break;
    case 36:
        click(second_host, second_container, 10 + 30, second_container.bounds.size.height - 20 - 40);
        next = 1;
        break;
    case 37:
        CHECK_THEN(says(second, "JSON.stringify(clicks)", "\"[30,40]\""), "a click in the unflipped parent: where it was");
        break;
    case 38:
        CHECK_THEN(zebkit_view_detach_native(second), "zebkit_view_detach_native");
        break;
    case 39:
        CHECK_THEN(second_container.subviews.count == 0 && second_page.window && second_page.window != second_host && second_page.window.visible,
                   "detached: in a window of its own again, shown");
        break;
    case 40:
        check(zebkit_view_attach_native(second, (uintptr_t)(__bridge void *)second_container, 5, 5, 200, 150, &error), "attached again", error);
        zebkit_free(error), error = NULL;
        next = 1;
        break;
    case 41:
        CHECK_THEN(second_container.subviews.count == 1 && visible_windows() == 4, "back in the parent, its own window gone");
        break;
    case 42:
        PIXELS(second_host, second_container, 100, second_container.bounds.size.height - 80, 300, 20, "and draws there");
        break;
    case 43: /* The view closed first. */
        zebkit_view_close(view);
        next = 1;
        break;
    case 44:
        CHECK_THEN(closed_first == 1 && container.subviews.count == 0, "the view closed first: gone from a host that goes on");
        break;
    case 45:
        if (can_see_pixels == 1) {
            NSBitmapImageRep *image = capture(host);
            double c[3];
            if (!image || !pixel(image, host, container, 150, 100, c) || !is_host(c)) break;
            check(1, "the host's pixels where it was", NULL);
        }
        next = 1;
        break;
    case 46: /* The parent gone first: a view outlives its superview (<zebkit/zebkit.h>). */
        [second_host close];
        second_host = nil;
        second_container = nil;
        next = 1;
        break;
    case 47:
        CHECK_THEN(closed_second == 0, "the parent's window closed first: the view stays");
        break;
    case 48:
        CHECK_THEN(says(second, "String(1+1)", "\"2\""), "and answers");
        break;
    case 49:
        zebkit_view_close(second);
        next = 1;
        break;
    case 50:
        CHECK_THEN(closed_second == 1, "and closes when the application says");
        break;
    case 51: /* The engine and its GPU process go on; WebGL's renderer in an attached view. */
        check(zebkit_view_attach_native(third, (uintptr_t)(__bridge void *)third_container, 0, 0, 300, 200, &error), "a view attached after both", error);
        zebkit_free(error), error = NULL;
        next = 1;
        break;
    case 52:
        PIXELS(third_host, third_container, 150, 100, 350, 100, "it draws");
        break;
    case 53:
        click(third_host, third_container, 150, 100);
        next = 1;
        break;
    case 54:
        CHECK_THEN(says(third, "JSON.stringify(clicks)", "\"[150,100]\""), "and takes a click");
        break;
    case 55:
        if (waiting) break;
        if (answer) {
            int hardware = strstr(answer, "Metal") && !strstr(answer, "SwiftShader") && !strstr(answer, "Software");
            check(hardware, "an attached view's WebGL runs on the GPU through Metal", answer);
            free(answer), answer = NULL;
            next = 1;
            break;
        }
        waiting = 1;
        zebkit_view_evaluate(third,
                             "(()=>{const g=document.createElement('canvas').getContext('webgl');"
                             "if(!g)return 'no WebGL';const d=g.getExtension('WEBGL_debug_renderer_info');"
                             "return d?g.getParameter(d.UNMASKED_RENDERER_WEBGL):'no renderer info'})()",
                             on_answer, NULL);
        break;
    case 56:
        zebkit_view_close(third);
        next = 1;
        break;
    case 57:
        CHECK_THEN(closed_third == 1, "closed");
        break;
    default:
        printf("%d pixel checks skipped\n", skipped_pixels);
        printf("%d of %d checks passed\n", passed, passed + failures);
        fflush(stdout);
        zebkit_quit();
        return;
    }
    if (next) {
        stage++;
        tries = 0;
        later(stage == 7 || stage == 24 || stage == 26 || stage == 53 ? 300 : 30);
        return;
    }
    if (++tries > 100) {
        char what[64];
        snprintf(what, sizeof what, "step %d timed out", stage);
        check(0, what, answer);
        printf("%d of %d checks passed\n", passed, passed + failures);
        fflush(stdout);
        zebkit_quit();
        return;
    }
    later(100);
}

/* Under --headless: the refusal, naming off-screen views. */
static void refused_ready(void) {
    char *error = NULL;
    zebkit_view *plain = zebkit_view_new("about:blank", 200, 100);
    NSView *anything = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 10, 10)];
    int refused = !zebkit_view_attach_native(plain, (uintptr_t)(__bridge void *)anything, 0, 0, 100, 100, &error);
    check(refused && error && strstr(error, "off-screen"), "the view has no window to parent: refused, naming off-screen views", error);
    zebkit_free(error), error = NULL;
    check(!zebkit_view_detach_native(plain), "nothing to detach", NULL);
    zebkit_view_close(plain);
    zebkit_view *offscreen = zebkit_view_new_offscreen("about:blank", 200, 100, 1.0f);
    refused = offscreen && !zebkit_view_attach_native(offscreen, (uintptr_t)(__bridge void *)anything, 0, 0, 100, 100, &error);
    check(refused && error && strstr(error, "off-screen"), "an off-screen view: refused", error);
    zebkit_free(error);
    if (offscreen)
        zebkit_view_close(offscreen);
    printf("%d of %d checks passed\n", passed, passed + failures);
    fflush(stdout);
    zebkit_quit();
}

static void ready(void *data) {
    (void)data;
    if (zebkit_is_headless()) {
        refused_ready();
        return;
    }
    container = [[FlippedView alloc] init];
    host = make_host(NSMakeRect(100, 200, 640, 480), container);
    second_container = [[NSView alloc] init];
    second_host = make_host(NSMakeRect(780, 200, 400, 300), second_container);
    third_container = [[NSView alloc] init];
    third_host = make_host(NSMakeRect(100, 720, 400, 200), third_container);
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
    return code || failures ? 1 : 0;
}
