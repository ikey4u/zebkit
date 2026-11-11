/*
 * A view's window: made with options before the system's window exists, its
 * frame (the system's title bar, the system's buttons over the page, or none),
 * its state and what it is asked to do, and where the person drags it from.
 * docs/SYSTEM_INTEGRATION.md, section 7, is the design; docs/API.md the
 * reference.
 *
 * All of it is for the engine's thread, as <zebkit/zebkit.h> is. Coordinates
 * are DIPs: the content area's with its top left the origin, the screen's for
 * a window's position. JSON in is validated whole before anything changes;
 * JSON out is the caller's to free with zebkit_free().
 */

#ifndef ZEBKIT_WINDOW_H
#define ZEBKIT_WINDOW_H

#include "zebkit.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * zebkit_view_new_in() with options, before the system's window is made: the
 * first frame the person sees is the window as configured. NULL @options_json
 * is {}. Invalid options: NULL, with the reason in @error (free it), and no
 * view. The options, every one optional:
 *
 *   width, height          the content area (default: the engine's size)
 *   minWidth, minHeight,   as zebkit_view_set_size_limits()
 *   resizable
 *   x, y                   the window's place on the screen; Wayland's
 *                          compositor places windows itself
 *   title                  the window's title; null for the page's
 *   frame                  "system" (the title bar and buttons of the
 *                          system), "overlay" (the page from the window's top,
 *                          the window's buttons over it: Window Controls
 *                          Overlay, env(titlebar-area-*) for the page), "none"
 *                          (no title bar and no buttons; resize edges kept)
 *   controls               {height, color, symbolColor}: the title bar area's
 *                          height in an overlay (at least the system's), and
 *                          the colours of buttons the engine draws (Windows,
 *                          Linux; macOS draws its own)
 *   background             "#rrggbb": the window's colour before the page's
 *                          first frame, and where the page is transparent
 *   appearance             "system", "light", "dark": the frame's look
 *   show                   false: made hidden, until the "show" action
 *   pageFullscreen         "window": a page's requestFullscreen() makes the
 *                          window fullscreen, as Chrome does; "content"
 *                          (zebkit_view_new()'s): the content area only
 *   dragRegions            {source: "none" | "css" | "host", origins: [...]}:
 *                          where the window is dragged from: the page's
 *                          app-region (CSS), honoured while the main frame is
 *                          of one of @origins, or zebkit_view_set_drag_regions()
 *   fallback               "error" (default): a frame the system cannot give
 *                          fails; "system": the system's frame then, which
 *                          the state reports
 *
 * width, height, frame, show and fallback are fixed once the window is made.
 */
ZEBKIT_API zebkit_view *zebkit_view_new_with_options(zebkit_context *context, const char *url,
                                                     const char *options_json, char **error);

/*
 * The window as JSON: {"revision", "owner" ("engine", "application" for a view
 * in the application's window, "view" for one in another view's
 * (zebkit_view_attach()), "none" without a window), "state" ("normal",
 * "minimized", "maximized", "fullscreen"), "pageFullscreen", "visible",
 * "focused", "bounds" {x, y, width, height}, "content" {width, height},
 * "scale", "frame" {requested, effective}, "titlebarArea" {x, y, width,
 * height} (what the page has as env(titlebar-area-*)), "controls" [{x, y,
 * width, height}] (the window's buttons over the page), "capabilities",
 * "options" {explicit}}. NULL for a closed view or off the engine's thread.
 */
ZEBKIT_API char *zebkit_view_get_window(zebkit_view *view);

/* Changes options that can change: all but width, height, frame, show and
 * fallback, which fail with "requires-recreate". Merged by key, applied whole
 * or not at all. */
ZEBKIT_API bool zebkit_view_set_window(zebkit_view *view, const char *options_json, char **error);

/*
 * Asks the system: "show", "hide", "minimize", "maximize", "restore",
 * "fullscreen", "exit-fullscreen", "close" (beforeunload first, as
 * zebkit_view_request_close()). True: asked; what happened comes as a state
 * event. False with @error: unsupported here, not the engine's window, a
 * closed view.
 */
ZEBKIT_API bool zebkit_view_window_action(zebkit_view *view, const char *action, char **error);

/* Every change of zebkit_view_get_window()'s state, in order of its revision,
 * whatever caused it; resizes during a live resize coalesced to one a frame.
 * One function per view; NULL removes it. The JSON is the engine's, for the
 * call. */
typedef void (*zebkit_window_state_func)(zebkit_view *view, const char *json, void *data);
ZEBKIT_API void zebkit_view_on_window_state(zebkit_view *view, zebkit_window_state_func func, void *data);

/* With dragRegions.source "host": where the window is dragged from,
 * [{"x", "y", "width", "height", "drag": true}] in content DIPs, later over
 * earlier, at most 1024; [] for none. A drag starts only from the person's own
 * press, as the system's title bar's does. */
ZEBKIT_API bool zebkit_view_set_drag_regions(zebkit_view *view, const char *regions_json, char **error);

/* Options for the windows pages of @view open from now on (window.open,
 * target=_blank): those of zebkit_view_new_with_options() but dragRegions,
 * which a popup never takes from its opener. NULL: the system's frame. */
ZEBKIT_API bool zebkit_view_set_popup_window_options(zebkit_view *view, const char *options_json,
                                                     char **error);

/* What this build can do on this system and display, as JSON:
 * {"window": {"frames": [...], "position", "actions", "controlsDrawnByEngine",
 * "controlsHeight", "appearance" (whether the "appearance" option is
 * honoured: macOS; elsewhere the frame has the toolkit's colours)},
 * "headless", "platform"}. A window's own state has its capabilities too,
 * which win. */
ZEBKIT_API char *zebkit_get_capabilities(void);

#ifdef __cplusplus
}
#endif

#endif /* ZEBKIT_WINDOW_H */
