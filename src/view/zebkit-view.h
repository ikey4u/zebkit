/*
 * A web view with a window of its own, on macOS.
 *
 * On Linux the engine gives every WebKitWebView a window and chooses where
 * it is: on the screen when the machine has a display (Wayland, else X11),
 * and nowhere when it has none, in which case the page loads, runs and can
 * be driven over CDP all the same. An application creates a view and need
 * not know which it got.
 *
 * WebKit on macOS leaves the window to the application: a WKWebView is put
 * into an NSWindow the application makes. These functions are the Linux
 * behaviour for macOS. A view made here has a window the engine made: on
 * the screen when there is one, and off every screen otherwise or when
 * asked. An application that manages its own windows does not need them
 * and creates a WKWebView as it always did.
 *
 * All of it is for the main thread.
 */

#ifndef ZEBKIT_VIEW_H
#define ZEBKIT_VIEW_H

#ifdef __APPLE__

#include <stdbool.h>

#ifdef __OBJC__
@class WKWebView;
@class WKWebViewConfiguration;
#else
typedef struct objc_object WKWebView;
typedef struct objc_object WKWebViewConfiguration;
#endif

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Whether a window can be shown: the process is in a login session with a
 * window server, and a screen is attached. False for a process started over
 * ssh or by a daemon, and for a Mac with no display.
 */
bool zebkit_display_is_available(void);

/*
 * A view in a window of its own, 1024 by 768. The window is on the screen
 * when zebkit_display_is_available(), with the application becoming one
 * that has windows (a Dock icon, a menu bar with Quit and Edit); otherwise
 * it is as with zebkit_view_new_headless(). @configuration may be NULL.
 *
 * The engine owns the view and its window until zebkit_view_destroy(). The
 * window is the view's (view.window), to size and title. Closing it, which
 * the user can do, leaves the view alive and loaded.
 */
WKWebView *zebkit_view_new(WKWebViewConfiguration *configuration);

/*
 * The same with a window that is never on any screen, whatever the machine
 * has. The page lays out, paints, plays and takes input as in a shown one.
 */
WKWebView *zebkit_view_new_headless(WKWebViewConfiguration *configuration);

/* Whether the view's window is one that is on the screen. */
bool zebkit_view_is_shown(WKWebView *view);

/* Closes the window and lets go of the view. For views made here. */
void zebkit_view_destroy(WKWebView *view);

#ifdef __cplusplus
}
#endif

#endif /* __APPLE__ */

#endif /* ZEBKIT_VIEW_H */
