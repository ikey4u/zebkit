/*
 * JavaScript libraries as normal libraries (docs/JS_LIBRARIES.md): a bundled
 * ES module loaded once and called from the application, as any library is.
 *
 *     static void told(zebkit_library *library, zebkit_library_code code, const char *json,
 *                      const zebkit_part *parts, size_t part_count, void *data) {
 *         if (code == ZEBKIT_LIBRARY_OK) puts(json);  // "\"<p>hi</p>\\n\""
 *     }
 *
 *     zebkit_library *md = zebkit_library_load("libs/markdown", NULL, NULL, NULL, &error);
 *     zebkit_library_call(md, "render", "[\"hi\"]", NULL, 0, 0, told, NULL);
 *
 * A library is a folder with index.mjs, or one .mjs file. Its exported
 * functions are its interface. It runs in a V8 isolate of its own, on a
 * thread of its own (the "isolate" backend), with no zebkit object, no files
 * and no network but the origins given at load; its imports are relative and
 * stay inside its folder (a one-file library imports nothing). It has
 * console, timers, queueMicrotask, TextEncoder/TextDecoder, atob/btoa, URL
 * and URLSearchParams, structuredClone, crypto.getRandomValues/randomUUID,
 * performance.now, Event, EventTarget, AbortController, DOMException,
 * WebAssembly, Intl, and fetch() (with Headers and Response) to the granted
 * origins, without cookies.
 *
 * Values are JSON plus parts: in JSON, {"$part": n} stands for parts[n], a
 * Uint8Array in the library. An object with a "$part" key and another key is
 * BAD_ARGUMENTS; depth is limited to 200. A function's value is awaited when
 * it is a promise; undefined is null; functions, BigInt, NaN, Infinity,
 * cycles, symbols and objects other than plain ones and arrays (class
 * instances, Map, Date) are BAD_RESULT. A library's object with a "$part" key
 * is BAD_RESULT too. Any typed array or ArrayBuffer it returns is a part.
 *
 * Threads: load and close are for the engine's thread; call and cancel for
 * any thread. Every callback runs on the engine's thread.
 */

#ifndef ZEBKIT_LIBRARY_H
#define ZEBKIT_LIBRARY_H

#include "zebkit.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct zebkit_library zebkit_library;

/* Bytes beside the JSON: {"$part": n} in it is parts[n]. */
typedef struct {
    const uint8_t *data;
    size_t size;
} zebkit_part;

typedef enum {
    ZEBKIT_LIBRARY_OK = 0,
    ZEBKIT_LIBRARY_THROWN = 1,          /* the function threw, or its promise was rejected */
    ZEBKIT_LIBRARY_LOAD_FAILED = 2,     /* the library did not load: the call never ran */
    ZEBKIT_LIBRARY_NO_FUNCTION = 3,     /* the library exports no function of that name */
    ZEBKIT_LIBRARY_BAD_ARGUMENTS = 4,   /* not a JSON array, a bad {"$part": n}, too deep or too large */
    ZEBKIT_LIBRARY_BAD_RESULT = 5,      /* a value JSON and parts cannot carry, or too large */
    ZEBKIT_LIBRARY_TIMEOUT = 6,         /* past the call's deadline (see zebkit_library_call()) */
    ZEBKIT_LIBRARY_CANCELLED = 7,       /* zebkit_library_cancel() */
    ZEBKIT_LIBRARY_QUEUE_FULL = 8,      /* as many calls wait already as the load's "queue" */
    ZEBKIT_LIBRARY_OUT_OF_MEMORY = 9,   /* the library's heap passed "heapLimitMB": it has ended */
    ZEBKIT_LIBRARY_GONE = 10,           /* the library ended, or was closed, before the call was told */
} zebkit_library_code;

/*
 * A call's outcome, or the load's (@ready). @json: the value when OK (for the
 * load, {"functions": [...]}), else {"message", "name", "stack"} saying why.
 * @json and @parts are the engine's, valid for the call.
 */
typedef void (*zebkit_library_func)(zebkit_library *library, zebkit_library_code code, const char *json,
                                    const zebkit_part *parts, size_t part_count, void *data);

/*
 * Loads the library at @path (a folder with index.mjs, or a .mjs file) on a
 * thread of its own, and returns at once; calls wait for the load. @ready
 * (may be NULL) is told once, with @data: OK with {"functions": [...]}, its
 * exported functions' names; LOAD_FAILED with why (a module that does not
 * compile, an import refused, an exception, the load past "timeoutMs"); or
 * GONE when it is closed first. NULL with @error (free it with zebkit_free())
 * for bad options or a path that is no library, or off the engine's thread.
 *
 * @options_json (NULL is {}): "backend" ("isolate", the only one so far),
 * "timeoutMs" (30000: a call's deadline unless it gives its own, and the
 * load's), "queue" (64: calls that may wait), "heapLimitMB" (128, at least
 * 16), "maxBytes" (64 MiB: JSON and parts each way), "network" (the origins
 * fetch() may reach, as "https://example.com" or "http://127.0.0.1:8080";
 * none by default). Unknown keys are errors.
 */
ZEBKIT_API zebkit_library *zebkit_library_load(const char *path, const char *options_json,
                                               zebkit_library_func ready, void *data, char **error);

/*
 * Calls the exported function @function with the arguments @args_json, a
 * JSON array (NULL is []), and @parts. Both are copied before this returns.
 * Calls run one at a time per library, in the order they were made;
 * libraries run in parallel. @timeout_ms: 0 the library's default ("timeoutMs"),
 * < 0 none; it counts from when the call starts to run.
 *
 * Returns the call's number (> 0), and @func is told exactly once, with
 * @data, after this returns. Returns 0 when refused at once (a NULL library
 * or @function, the library closed), and @func is never told. Any thread.
 *
 * At its deadline a call whose script is running (a loop that never ends) is
 * stopped: it is TIMEOUT, the library ends, and the calls after it are GONE,
 * as every later call is. A call that is waiting then (on a timer, on
 * fetch()) is TIMEOUT and the next call starts; what it does later is dropped.
 * A library that ends (its heap exhausted, a script stopped) is not
 * restarted: replaying calls with side effects is unsafe; load it again.
 */
ZEBKIT_API uint64_t zebkit_library_call(zebkit_library *library, const char *function, const char *args_json,
                                        const zebkit_part *parts, size_t part_count, int64_t timeout_ms,
                                        zebkit_library_func func, void *data);

/*
 * The call @call is told CANCELLED, unless it was told already. A call still
 * waiting its turn never runs; one running is told at once, and the next one
 * starts once it settles (or at its deadline). Any thread.
 */
ZEBKIT_API void zebkit_library_cancel(zebkit_library *library, uint64_t call);

/*
 * Ends the library: calls not yet told are told GONE after this returns (and
 * @ready GONE, if the load had not ended), a running script is stopped, and
 * the handle is freed after the last of them. Not to be used after this.
 * The engine's thread.
 */
ZEBKIT_API void zebkit_library_close(zebkit_library *library);

#ifdef __cplusplus
}
#endif

#endif /* ZEBKIT_LIBRARY_H */
