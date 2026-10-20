/*
 * zebkit: the web engine, for applications that embed it.
 *
 * The engine is Chromium's content layer, in a library (libzebkit.so.1 on
 * Linux) with a helper program beside it (zebkit-helper) that runs the
 * renderer, GPU and utility processes. An application links the library,
 * calls zebkit_main() from its main thread, and creates views when the
 * engine is ready:
 *
 *     static void ready(void *data) {
 *         zebkit_view_new("https://example.com/", 0, 0);
 *     }
 *
 *     int main(int argc, char **argv) {
 *         return zebkit_main(argc, argv, ready, NULL);
 *     }
 *
 * Threads: everything here is for the thread that called zebkit_main(),
 * and every callback is called on it, except zebkit_post_task(), which any
 * thread may call to run a function there.
 *
 * Strings are UTF-8. A string the engine returns is the caller's, to free
 * with zebkit_free(); one it passes to a callback is the engine's, valid
 * for the call.
 */

#ifndef ZEBKIT_ZEBKIT_H
#define ZEBKIT_ZEBKIT_H

#include <stdbool.h>
#include <stdint.h>

#if defined(ZEBKIT_IMPLEMENTATION)
#define ZEBKIT_API __attribute__((visibility("default")))
#else
#define ZEBKIT_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* zebkit's version, as in "0.1.0". */
ZEBKIT_API const char *zebkit_version(void);

/* The engine's, as in "Chromium 155.0.8059.84". */
ZEBKIT_API const char *zebkit_engine_version(void);

/* Frees a string or buffer the engine returned. NULL is ignored. */
ZEBKIT_API void zebkit_free(void *pointer);

/* ---- The engine ------------------------------------------------------ */

typedef void (*zebkit_task_func)(void *data);

/*
 * Starts the engine, calls @ready on this thread once views can be created,
 * runs until zebkit_quit(), and returns the process's exit code. Call it
 * once, from the main thread, with main()'s arguments: Chromium's switches
 * among them are honoured, such as --remote-debugging-port=9222 (CDP, as
 * Chrome serves it), --user-data-dir, --no-sandbox, --headless and
 * --enable-logging=stderr.
 *
 * Views have windows when the machine has a display (Wayland or X11) and
 * none otherwise; a page loads, runs and can be driven over CDP either way.
 * Profile data is kept in --user-data-dir, by default
 * $XDG_CONFIG_HOME/zebkit/<program name>.
 *
 * The loop runs until zebkit_quit(), also with no view open: closing the
 * last window is the application's to act on (zebkit_view_on_close()).
 */
ZEBKIT_API int zebkit_main(int argc, char **argv, zebkit_task_func ready, void *data);

/* Closes every view and ends zebkit_main()'s loop. */
ZEBKIT_API void zebkit_quit(void);

/* Runs @func(@data) on the engine's thread. Any thread may call it. */
ZEBKIT_API void zebkit_post_task(zebkit_task_func func, void *data);

/* The same, @delay_ms milliseconds from now. */
ZEBKIT_API void zebkit_post_delayed_task(zebkit_task_func func, void *data, uint32_t delay_ms);

/* Whether views are without windows: no display, or --headless. */
ZEBKIT_API bool zebkit_is_headless(void);

/* ---- Views ----------------------------------------------------------- */

typedef struct zebkit_view zebkit_view;

/*
 * A view loading @url (NULL for about:blank), @width by @height pixels, or
 * a default size when either is 0. It lives until it is closed, by
 * zebkit_view_close(), by the user closing its window, by the page's
 * window.close(), or by zebkit_quit(); the on_close callback is the last
 * use of it. zebkit_view_close() and zebkit_quit() take effect once the
 * current callback has returned, so either may be called from any of them.
 */
ZEBKIT_API zebkit_view *zebkit_view_new(const char *url, int width, int height);

ZEBKIT_API void zebkit_view_load_url(zebkit_view *view, const char *url);
ZEBKIT_API void zebkit_view_reload(zebkit_view *view);
ZEBKIT_API void zebkit_view_go_back(zebkit_view *view);
ZEBKIT_API void zebkit_view_go_forward(zebkit_view *view);
ZEBKIT_API void zebkit_view_resize(zebkit_view *view, int width, int height);
ZEBKIT_API void zebkit_view_close(zebkit_view *view);

/* The page's URL and title. Free with zebkit_free(). */
ZEBKIT_API char *zebkit_view_get_url(zebkit_view *view);
ZEBKIT_API char *zebkit_view_get_title(zebkit_view *view);

typedef enum {
    ZEBKIT_LOAD_STARTED = 0,   /* a load of the page was asked for */
    ZEBKIT_LOAD_COMMITTED = 1, /* the new document replaced the old */
    ZEBKIT_LOAD_FINISHED = 2,  /* the page and its resources have loaded */
    ZEBKIT_LOAD_FAILED = 3,    /* the load failed or was stopped */
} zebkit_load_event;

typedef void (*zebkit_load_func)(zebkit_view *view, zebkit_load_event event, const char *url, void *data);
typedef void (*zebkit_view_func)(zebkit_view *view, void *data);
typedef void (*zebkit_title_func)(zebkit_view *view, const char *title, void *data);

/* One callback of each kind per view; a new one replaces the old, NULL removes it. */
ZEBKIT_API void zebkit_view_on_load(zebkit_view *view, zebkit_load_func func, void *data);
ZEBKIT_API void zebkit_view_on_title(zebkit_view *view, zebkit_title_func func, void *data);
ZEBKIT_API void zebkit_view_on_close(zebkit_view *view, zebkit_view_func func, void *data);

/*
 * The result of a script, or of a call from the page. @json is the value as
 * JSON, or NULL when @error is set; @error is the message of what was
 * thrown, or NULL.
 */
typedef void (*zebkit_result_func)(zebkit_view *view, const char *json, const char *error, void *data);

/*
 * Runs @script in the page's main frame, as the page's own script would
 * run, and calls @func with its value. A promise is waited for. @func may
 * be NULL.
 */
ZEBKIT_API void zebkit_view_evaluate(zebkit_view *view, const char *script, zebkit_result_func func, void *data);

/* ---- The page's script and the application -------------------------- */

/*
 * What a page may use of the global object `zebkit`. A page has none of it
 * unless zebkit_view_grant() gives it to the page's origin.
 *
 *   ZEBKIT_ALLOW_APP       zebkit.app.<name>(...args): the functions the
 *                          application binds with zebkit_view_bind()
 *   ZEBKIT_ALLOW_FETCH     zebkit.fetch(url, {method, headers, body}): the
 *                          engine's network stack, without CORS, with the
 *                          browser's cookies (zebkit_fetch())
 *   ZEBKIT_ALLOW_FILES     zebkit.fs: readFile, writeFile, readDir, stat,
 *                          mkdir, remove, under the directory granted
 *   ZEBKIT_ALLOW_COMPRESS  zebkit.compress(format, bytes, level) and
 *                          zebkit.decompress(format, bytes)
 *   ZEBKIT_ALLOW_CRYPTO    zebkit.crypto: digest, hmac, randomBytes
 *   ZEBKIT_ALLOW_IMAGE     zebkit.image: decode(bytes), encode(format, image, quality)
 *   ZEBKIT_ALLOW_URL       zebkit.url: parse(url, base), registrableDomain(host)
 *
 * Every function returns a promise. Bytes are Uint8Arrays.
 */
typedef enum {
    ZEBKIT_ALLOW_APP = 1 << 0,
    ZEBKIT_ALLOW_FETCH = 1 << 1,
    ZEBKIT_ALLOW_FILES = 1 << 2,
    ZEBKIT_ALLOW_COMPRESS = 1 << 3,
    ZEBKIT_ALLOW_CRYPTO = 1 << 4,
    ZEBKIT_ALLOW_IMAGE = 1 << 5,
    ZEBKIT_ALLOW_URL = 1 << 6,
    ZEBKIT_ALLOW_ALL = 0x7f,
} zebkit_allow;

/*
 * Gives pages of @origin in @view what @allow says (zebkit_allow bits; 0
 * takes it back). @origin is as a browser serializes it, such as
 * "https://app.example" or "http://127.0.0.1:8080", or "file://" for local
 * files, or "*" for every origin, which is for pages the application wrote
 * itself and nothing else. @files_root is the directory zebkit.fs works in
 * for ZEBKIT_ALLOW_FILES, or NULL. Frames of another origin than a grant's
 * get nothing, and frames of another site are never given the object.
 */
ZEBKIT_API void zebkit_view_grant(zebkit_view *view, const char *origin, unsigned allow, const char *files_root);

typedef struct zebkit_call zebkit_call;

/*
 * A call from the page: zebkit.app.<name>(...args). @args_json is the
 * arguments as a JSON array, @origin the calling page's. Answer it, now or
 * later, with exactly one of zebkit_call_resolve() and zebkit_call_reject().
 */
typedef void (*zebkit_bind_func)(zebkit_view *view, const char *origin, const char *args_json, zebkit_call *call,
                                 void *data);

/* Makes zebkit.app.<@name> call @func. NULL @func removes it. */
ZEBKIT_API void zebkit_view_bind(zebkit_view *view, const char *name, zebkit_bind_func func, void *data);

/* Resolves the page's promise with @json (NULL for undefined). */
ZEBKIT_API void zebkit_call_resolve(zebkit_call *call, const char *json);

/* Rejects the page's promise with an Error of @message. */
ZEBKIT_API void zebkit_call_reject(zebkit_call *call, const char *message);

/* ---- Script outside any page ---------------------------------------- */

/*
 * A JavaScript context of the application's own, in this process, on the
 * engine's thread: V8 with the whole of zebkit.* (no grants: it is the
 * application's code, as an Electron application's main process is), and
 * zebkit.View to make views, load pages in them and answer their calls.
 * <zebkit/zebkit.h>'s script reference is docs/blink/API.md.
 */
typedef struct zebkit_script zebkit_script;

/* @args: what the script sees as zebkit.args; may be NULL. */
ZEBKIT_API zebkit_script *zebkit_script_new(int argc, const char *const *argv);

/*
 * Runs the ES module in the file @path, with the modules it imports
 * relative to it, and calls @func when its evaluation ends (for a
 * top-level await, when the promise settles): @json NULL, or @error the
 * message and stack of what was thrown.
 */
ZEBKIT_API void zebkit_script_run_module(zebkit_script *script, const char *path, zebkit_result_func func,
                                         void *data);

/* Evaluates @source as a classic script; @func gets the value as JSON. */
ZEBKIT_API void zebkit_script_evaluate(zebkit_script *script, const char *source, zebkit_result_func func,
                                       void *data);

/*
 * Called once nothing of the script is pending: no timer, no call in
 * progress, no view open, and its module evaluated. zebkit-js quits then,
 * as Node exits.
 */
ZEBKIT_API void zebkit_script_on_idle(zebkit_script *script, zebkit_task_func func, void *data);

/* Ends the script: its timers stop, its views stay. */
ZEBKIT_API void zebkit_script_destroy(zebkit_script *script);

/* ---- The Chrome DevTools Protocol ------------------------------------ */

/*
 * Serves CDP on @address (a numeric IP address, NULL for 127.0.0.1) and
 * @port (0 for one the system chooses), as Chrome does with
 * --remote-debugging-port. Returns the port, or 0 with @error set (free it
 * with zebkit_free(); @error may be NULL). If the endpoint already runs,
 * returns its port.
 */
ZEBKIT_API unsigned zebkit_cdp_listen(const char *address, unsigned port, char **error);

/* The port CDP is served on, or 0. */
ZEBKIT_API unsigned zebkit_cdp_get_port(void);

/*
 * The browser target's WebSocket address, what Puppeteer calls
 * browserWSEndpoint, or NULL when CDP is not served. Free with zebkit_free().
 */
ZEBKIT_API char *zebkit_cdp_get_url(void);

/* Disconnects every client and stops serving CDP. */
ZEBKIT_API void zebkit_cdp_stop(void);

/* ---- For zebkit-helper ----------------------------------------------- */

/* The renderer, GPU and utility processes. Only zebkit-helper calls it. */
ZEBKIT_API int zebkit_helper_main(int argc, char **argv);

#ifdef __cplusplus
}
#endif

#endif /* ZEBKIT_ZEBKIT_H */
