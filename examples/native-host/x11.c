/* A page in a window of the application's: plain Xlib, the engine's window its child.
 * Run it in an X11 session; the second argument is the page. Close the window to quit. */
#include <zebkit/zebkit.h>
#include <X11/Xlib.h>
#include <stdio.h>

static const char *url = "https://example.com/";
static Display *display;
static Window window;
static Atom close_atom;
static zebkit_view *view;

static void poll_events(void *data) {
    (void)data;
    while (XPending(display)) {
        XEvent event;
        XNextEvent(display, &event);
        if (event.type == ConfigureNotify && view) {
            zebkit_view_set_bounds(view, 0, 0, event.xconfigure.width, event.xconfigure.height);
        } else if (event.type == FocusIn && view) {
            zebkit_view_focus(view);
        } else if (event.type == ClientMessage && (Atom)event.xclient.data.l[0] == close_atom) {
            zebkit_quit();
            return;
        }
    }
    zebkit_post_delayed_task(poll_events, NULL, 10); /* a real host joins its loop: zebkit_set_wakeup() */
}

static void ready(void *data) {
    (void)data;
    char *error = NULL;
    view = zebkit_view_new(url, 800, 600);
    if (!zebkit_view_attach_native(view, window, 0, 0, 800, 600, &error)) {
        fprintf(stderr, "%s\n", error);
        zebkit_free(error);
        zebkit_quit();
        return;
    }
    poll_events(NULL);
}

int main(int argc, char **argv) {
    display = XOpenDisplay(NULL);
    if (!display) {
        fprintf(stderr, "no X11 display\n");
        return 1;
    }
    window = XCreateSimpleWindow(display, DefaultRootWindow(display), 0, 0, 800, 600, 0, 0, 0);
    close_atom = XInternAtom(display, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(display, window, &close_atom, 1);
    XSelectInput(display, window, StructureNotifyMask | FocusChangeMask);
    XMapWindow(display, window);
    XFlush(display);
    if (argc > 1 && argv[argc - 1][0] != '-')
        url = argv[--argc];
    return zebkit_main(argc, argv, ready, NULL);
}
