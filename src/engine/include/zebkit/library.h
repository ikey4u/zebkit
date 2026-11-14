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
 * stay inside its folder (a one-file library imports nothing), besides
 * 'zebkit:host'. It has
 * console, timers, queueMicrotask, TextEncoder/TextDecoder, atob/btoa, URL
 * and URLSearchParams, structuredClone, crypto.getRandomValues/randomUUID,
 * performance.now, Event, EventTarget, AbortController, DOMException,
 * WebAssembly, Intl, and fetch() (with Headers and Response) to the granted
 * origins, without cookies. It calls the application's functions named at
 * load ("host") as
 *
 *     import host from 'zebkit:host';
 *     const answer = await host.call('ask', question);   // zebkit_library_set_host()
 *
 * Values are JSON plus parts: in JSON, {"$part": n} stands for parts[n], a
 * Uint8Array in the library. An object with a "$part" key and another key is
 * BAD_ARGUMENTS; depth is limited to 200. A function's value is awaited when
 * it is a promise; undefined is null; functions, BigInt, NaN, Infinity,
 * cycles, symbols and objects other than plain ones and arrays (class
 * instances, Map, Date) are BAD_RESULT. A library's object with a "$part" key
 * is BAD_RESULT too. Any typed array or ArrayBuffer it returns is a part.
 *
 * Threads: load, close and set_host are for the engine's thread; call,
 * cancel and answer for any thread. Every callback runs on the engine's
 * thread.
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
    ZEBKIT_LIBRARY_OUT_OF_MEMORY = 9,   /* an instance's heap passed "heapLimitMB": it has ended */
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
 * @options_json (NULL is {}): "backend" ("isolate", or "page"),
 * "timeoutMs" (30000: a call's deadline unless it gives its own, the load's,
 * and that of the application's answer to host.call()), "queue" (64: calls
 * that may wait, and requests to the application), "heapLimitMB" (128, at
 * least 16; each instance's), "maxBytes" (64 MiB: JSON and parts each way),
 * "network" (the origins fetch() may reach, as "https://example.com" or
 * "http://127.0.0.1:8080"; none by default), "instances" (1 to 64, default 1:
 * isolates on threads of their own, each loading the library, the calls
 * shared among them; state is each instance's own), "codeCache" (true: V8's
 * code cache of the modules, kept in memory for the process and in the
 * profile's folder "Library Code Cache" for the next; false: neither),
 * "host" (the names of the application's functions host.call() reaches; see
 * zebkit_library_set_host()). Unknown keys are errors. An instance idle for
 * 2 s (no call started since its last ended) makes the code cache and lets V8
 * give its memory back.
 *
 * "backend": "page" runs the library in a hidden page of its own instead (a
 * renderer of its own, a profile in memory, zebkit-library://<random id>/),
 * for what needs a DOM, a canvas, fonts or layout; "width" and "height" (CSS
 * pixels, 800 by 600) are its size. The same calls, values and codes; no user
 * gesture, popups, downloads, workers or WebSockets; requests other than its
 * own files and "network"'s origins fail. A page running script past a call's
 * deadline has its renderer stopped (TIMEOUT, then GONE); its V8 heap past
 * "heapLimitMB" is OUT_OF_MEMORY; its renderer gone, GONE. "instances" and
 * "host" are the isolate's, refused with a page.
 */
ZEBKIT_API zebkit_library *zebkit_library_load(const char *path, const char *options_json,
                                               zebkit_library_func ready, void *data, char **error);

/*
 * Calls the exported function @function with the arguments @args_json, a
 * JSON array (NULL is []), and @parts. Both are copied before this returns.
 * Calls run one at a time per instance (one, unless "instances"), in the
 * order they were made, each on the first instance free; libraries run in
 * parallel. @timeout_ms: 0 the library's default ("timeoutMs"), < 0 none; it
 * counts from when the call starts to run.
 *
 * Returns the call's number (> 0), and @func is told exactly once, with
 * @data, after this returns. Returns 0 when refused at once (a NULL library
 * or @function, the library closed), and @func is never told. Any thread.
 *
 * At its deadline a call whose script is running (a loop that never ends) is
 * stopped: it is TIMEOUT and its instance ends. A call that is waiting then
 * (on a timer, on fetch(), on the application's answer) is TIMEOUT and the
 * instance takes the next call; what it does later is dropped. An instance
 * that ends (its heap exhausted, a script stopped) is not restarted: the
 * other instances take the calls; with the last one the library ends, and
 * the calls after it are GONE, as every later call is. Nothing is replayed,
 * as calls with side effects cannot safely be; load the library again.
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
 * The library's call of an application's function: host.call(@name, ...args)
 * in its script, @name one of the load's "host". @args_json is the arguments
 * (a JSON array) and @parts their bytes, as a call's are; both the engine's,
 * valid for the call. Answer @request exactly once, now or later, from any
 * thread, with zebkit_library_answer(). Runs on the engine's thread.
 */
typedef void (*zebkit_library_host_func)(zebkit_library *library, uint64_t request, const char *name,
                                         const char *args_json, const zebkit_part *parts, size_t part_count,
                                         void *data);

/*
 * Sets what answers the library's host.call() (NULL: nothing; requests wait),
 * with @data. Requests made before it is set are given to it once it is. A
 * request not answered in the library's "timeoutMs" is rejected in the
 * library (a TimeoutError), and its answer after that is refused. The
 * engine's thread; the backend "isolate".
 */
ZEBKIT_API void zebkit_library_set_host(zebkit_library *library, zebkit_library_host_func func, void *data);

/*
 * Answers @request: @code OK resolves host.call()'s promise with @json (a
 * JSON value; NULL is null) and @parts ({"$part": n}, Uint8Arrays in the
 * library); any other code rejects it with an Error of @json's "message" and
 * "name" ({"message", "name"}, as the library's errors are). Copied before
 * this returns. 1 when taken; 0 when @request was answered already, is past
 * its deadline, or its instance has ended. Any thread, until
 * zebkit_library_close().
 */
ZEBKIT_API int zebkit_library_answer(zebkit_library *library, uint64_t request, zebkit_library_code code,
                                     const char *json, const zebkit_part *parts, size_t part_count);

/*
 * Ends the library: calls not yet told are told GONE after this returns (and
 * @ready GONE, if the load had not ended), a running script is stopped,
 * requests to the application are dropped, and the handle is freed after the
 * last of them. Not to be used after this, zebkit_library_answer() included.
 * The engine's thread.
 */
ZEBKIT_API void zebkit_library_close(zebkit_library *library);

#ifdef __cplusplus
}
#endif

#endif /* ZEBKIT_LIBRARY_H */
