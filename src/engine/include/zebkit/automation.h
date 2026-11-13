/*
 * zebkit: Playwright's API, natively (docs/AUTOMATION.md).
 *
 * A view's page driven as Playwright drives one: selectors and locators
 * (CSS, text=, role=, internal:role=button[name="Go"i], ...), actions that
 * wait until the element is visible, enabled, stable and the one under the
 * point, keyboard and mouse, navigation that waits for the page to load.
 * Each command is one of Playwright's protocol (the .yml files of packages/protocol/spec
 * in Playwright's source), with its name, parameters and result, and runs
 * inside the engine: no CDP, no driver process.
 *
 * Everything here is for the engine's thread, as in <zebkit/zebkit.h>.
 */

#ifndef ZEBKIT_AUTOMATION_H
#define ZEBKIT_AUTOMATION_H

#include <zebkit/zebkit.h>

#ifdef __cplusplus
extern "C" {
#endif

/* @json is the command's result as a JSON object, or NULL with @error,
 * Playwright's message ("Timeout 30000ms exceeded.\nCall log:\n  - ...",
 * "strict mode violation: ..."). Both are the engine's for the call. */
typedef void (*zebkit_automation_func)(zebkit_view *view, const char *json, const char *error, void *data);

/*
 * Runs Playwright's command @method on @view's page with @params, a JSON
 * object as Playwright's protocol has it (NULL for {}), and calls @func
 * once (@func may be NULL). Commands that wait take "timeout" in ms (30000
 * by default, 0 for none). Several may run at once.
 *
 *   zebkit_automation_call(view, "Frame.click", "{\"selector\": \"#go\", \"strict\": true}", done, NULL);
 *   zebkit_automation_call(view, "Frame.goto", "{\"url\": \"https://example.com\", \"waitUntil\": \"load\"}", ...);
 *
 * Where zebkit differs from the protocol: an evaluation's "arg" and
 * "value" are plain JSON, not Playwright's serialized values; binary
 * results ("binary" of Page.screenshot) are base64; Page.screenshot also
 * takes "path", a file to write instead.
 */
ZEBKIT_API void zebkit_automation_call(zebkit_view *view, const char *method, const char *params,
                                       zebkit_automation_func func, void *data);

/*
 * zebkit's own, beyond Playwright's API: a screenshot's pixels, unencoded.
 * @method is "Page.screenshot", or "ElementHandle.screenshot" with the
 * element's "handle" in @params; @params are those of the command
 * (clip, fullPage, scale, animations, caret, mask, maskColor, style,
 * omitBackground, timeout), but not type, quality or path. The picture is
 * the one the command's PNG has: @func gets @height rows of @stride bytes,
 * 4 bytes a pixel, R, G, B, A in that order, 8 bits each, not
 * premultiplied (RGBA8888, straight alpha; @stride is @width * 4). The
 * pixels are the engine's, for the time of the call to @func only (copy
 * them to keep them). NULL with @error on failure.
 *
 *   zebkit_automation_screenshot_pixels(view, "Page.screenshot", "{\"fullPage\": true}", got, NULL);
 */
typedef void (*zebkit_automation_pixels_func)(zebkit_view *view, const unsigned char *pixels, int width,
                                              int height, int stride, const char *error, void *data);
ZEBKIT_API void zebkit_automation_screenshot_pixels(zebkit_view *view, const char *method, const char *params,
                                                    zebkit_automation_pixels_func func, void *data);

/*
 * Events of @view's page, as Playwright's protocol has them: @event is
 * "Frame.navigated", "Frame.loadstate", "Page.frameAttached",
 * "Page.frameDetached", "BrowserContext.console", "BrowserContext.dialog",
 * "BrowserContext.dialogClosed", "Page.fileChooser", "Page.crash",
 * "Page.close"; @json its parameters. Dialogs are held for
 * Dialog.accept or Dialog.dismiss, and file choosers for
 * FileChooser.setFiles, once Page.updateSubscription has asked for them
 * ({"event": "dialog" or "fileChooser", "enabled": true}). One function
 * per view; NULL stops. The page's last events ("Page.close" among them)
 * can come as the view closes, after its zebkit_view_on_close() function has
 * run: keep @data valid until the task that closed the view has ended (free
 * it in a posted task, as the bindings do), or stop the function before
 * closing the view yourself.
 */
typedef void (*zebkit_automation_event_func)(zebkit_view *view, const char *event, const char *json, void *data);
ZEBKIT_API void zebkit_automation_on_event(zebkit_view *view, zebkit_automation_event_func func, void *data);

/*
 * Contexts and pages: zebkit_automation_call(NULL, "Browser.newContext",
 * {viewport, userAgent, ...}) answers {"context": "x1"};
 * "BrowserContext.newPage" ({"context": "x1"}) answers {"page": "p2"}, a
 * view of the context's (off-screen under --headless), and
 * zebkit_automation_page() is that view; "BrowserContext.pages",
 * "BrowserContext.close". A view's page id is zebkit_automation_page_id()
 * (free it with zebkit_free()).
 */
ZEBKIT_API zebkit_view *zebkit_automation_page(const char *page);
ZEBKIT_API char *zebkit_automation_page_id(zebkit_view *view);

/*
 * Playwright's own endpoint: a WebSocket on 127.0.0.1:@port (0 picks one)
 * speaking Playwright's protocol, what `chromium.connect(url)` of each of
 * Playwright's languages connects to, served by these commands (no CDP, no
 * Node). @func gets its url ("ws://127.0.0.1:<port>/<random token>") or an
 * error, on the engine's thread. The url is the credential: anything with it
 * drives the engine and writes files (Page.screenshot's "path"), so give it
 * only to what should. Only that path connects, and no request with an Origin
 * header (a web page's WebSocket has one). A client's contexts close when it
 * disconnects. Called off the engine's thread, @func is told the error at once.
 */
typedef void (*zebkit_automation_listen_func)(const char *url, const char *error, void *data);
ZEBKIT_API void zebkit_automation_listen(int port, zebkit_automation_listen_func func, void *data);

/* The commands this engine has, as a JSON array of their names. Free with
 * zebkit_free(). */
ZEBKIT_API char *zebkit_automation_commands(void);

#ifdef __cplusplus
}
#endif

#endif /* ZEBKIT_AUTOMATION_H */
