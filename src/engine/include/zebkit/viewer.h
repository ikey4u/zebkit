/*
 * The page's global `zebkit` in the system's web view: WKWebView on iOS,
 * Android's WebView (docs/VIEWER.md). A page there has the same object as
 * in zebkit's own views (docs/API.md, "JavaScript in the page"), so one page
 * runs on every system: zebkit's engine on Linux, macOS and Windows
 * (zebkit_view_grant(), zebkit_view_bind()), the system's web view on
 * Android and iOS, with this.
 *
 * The web view carries messages; this does the rest. The application adds
 * zebkit_viewer_script() to every document before the page's own script,
 * with a function that posts a string to it (a script message handler, a
 * JavaScript interface), and hands each string posted to
 * zebkit_viewer_handle() with the origin of the frame that posted it. What
 * that returns is JavaScript to run in the frame, which answers the page's
 * promise. zebkit.app and zebkit.fetch are the application's to answer
 * (its functions; the system's HTTP): for those zebkit_viewer_handle()
 * returns NULL and says what was asked, and zebkit_viewer_answer() makes
 * the JavaScript of the answer.
 *
 * The bindings for Swift (bindings/swift: ZebkitViewer, a WKWebView) and
 * Android (bindings/android: dev.zebkit.ZebkitViewer, a WebView) do all of
 * this; an application uses those.
 *
 * Any thread: a viewer's calls may run off the main thread, and should for
 * zebkit.pdf, which takes time. Strings are UTF-8; those returned are the
 * caller's, to free with zebkit_free().
 */

#ifndef ZEBKIT_VIEWER_H
#define ZEBKIT_VIEWER_H

#include "zebkit.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct zebkit_viewer zebkit_viewer;

/* A viewer: no grants, no documents. */
ZEBKIT_API zebkit_viewer *zebkit_viewer_new(void);

/* Frees @viewer, and closes the documents its pages opened. */
ZEBKIT_API void zebkit_viewer_free(zebkit_viewer *viewer);

/*
 * Gives pages of @origin what @allow says (zebkit_allow bits; 0 takes it
 * back), as zebkit_view_grant(): @origin as a browser serializes it
 * ("https://app.example", "app://reader"), or "*" for every origin, which is
 * for pages the application wrote itself. @files_root is zebkit.fs's
 * directory for ZEBKIT_ALLOW_FILES, or NULL.
 */
ZEBKIT_API void zebkit_viewer_grant(zebkit_viewer *viewer, const char *origin, unsigned allow,
                                    const char *files_root);

/* Closes the documents @viewer's pages opened: when the page is left (a
 * navigation commits), as zebkit's views do with a frame's document. */
ZEBKIT_API void zebkit_viewer_reset(zebkit_viewer *viewer);

/*
 * The script to add before each document's own (WKUserScript at document
 * start; Android: evaluated as a page starts, and again when the interface
 * is first called). @post is the source of a JavaScript function that posts
 * one string to the application, as
 * "(m) => window.webkit.messageHandlers.zebkit.postMessage(m)". The page then
 * has `zebkit`, as in zebkit's views, but for zebkit.pathForFile().
 */
ZEBKIT_API char *zebkit_viewer_script(const char *post);

/*
 * A string a page posted, from a frame of @origin. Returns the JavaScript to
 * run in that frame, which answers the call; or NULL when the call is the
 * application's, and then *@app_call is set (the caller frees it) to JSON
 * {"id": 7, "module": "app" | "fetch", "method": "name", "args": [...]},
 * bytes in the arguments as {"$bytes": base64}, to answer with
 * zebkit_viewer_answer(). NULL with *@app_call NULL: not a call of the
 * script's; nothing to answer.
 */
ZEBKIT_API char *zebkit_viewer_handle(zebkit_viewer *viewer, const char *origin, const char *message,
                                      char **app_call);

/*
 * The JavaScript that answers call @id: with @result_json (a JSON value,
 * bytes as {"$bytes": base64}; NULL for undefined) when @ok, else rejected
 * with @result_json as the message (a plain string, not JSON).
 */
ZEBKIT_API char *zebkit_viewer_answer(long long id, bool ok, const char *result_json);

#ifdef __cplusplus
}
#endif

#endif /* ZEBKIT_VIEWER_H */
