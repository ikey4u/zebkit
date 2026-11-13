# JavaScript libraries as normal libraries

Status: design; nothing is built. It replaces the earlier proposal.

An application loads a bundled JavaScript library and calls its functions
from C, Rust, Python, Go or host JavaScript, as it calls any library:

```rust
let charts = zebkit::js::Library::load("resources/libs/echarts", Value::Null)?;
let svg = charts.call("renderSvg", json_value).await?.value; // a JSON string
```

Paths are the repository's; Chromium's are under `upstream/chromium/src`.

**Not for:** a library the application's page already loads (call it there:
a second copy costs its memory again); output people click (that is a page);
Node.js programs (no `fs`, `net`); code the application does not trust (the
`isolate` backend runs in the application's process).

## A library

A folder with `index.mjs`, or one `.mjs` file. Its exported functions are its
interface; there is no manifest. Imports are relative and stay inside the
folder once links are resolved (today's host script accepts any path:
`src/engine/script.cc:4284-4324`). Data files and WebAssembly are put in the
bundle: a library has no file access. An application usually writes a small
entry, for example `renderSvg({width, height, option})` calling
`echarts.init(null, null, {renderer: 'svg', ssr: true, width, height})`,
`renderToSVGString()` and `dispose()`.

## C API: `<zebkit/library.h>`

```c
typedef struct zebkit_library zebkit_library;
typedef struct { const uint8_t *data; size_t size; } zebkit_part;  /* {"$part": n} is parts[n] */

typedef enum {
    ZEBKIT_LIBRARY_OK, ZEBKIT_LIBRARY_THROWN, ZEBKIT_LIBRARY_LOAD_FAILED,
    ZEBKIT_LIBRARY_NO_FUNCTION, ZEBKIT_LIBRARY_BAD_ARGUMENTS, ZEBKIT_LIBRARY_BAD_RESULT,
    ZEBKIT_LIBRARY_TIMEOUT, ZEBKIT_LIBRARY_CANCELLED, ZEBKIT_LIBRARY_QUEUE_FULL,
    ZEBKIT_LIBRARY_OUT_OF_MEMORY, ZEBKIT_LIBRARY_GONE,
} zebkit_library_code;

/* @json: the value when OK, else {"message", "name", "stack"}. The engine's, valid for the call. */
typedef void (*zebkit_library_func)(zebkit_library *library, zebkit_library_code code, const char *json,
                                    const zebkit_part *parts, size_t part_count, void *data);

/* Returns at once; calls wait for the load. @ready (may be NULL) is told once: OK with
 * {"functions": [...]}, or LOAD_FAILED. NULL with @error for bad options or path. */
ZEBKIT_API zebkit_library *zebkit_library_load(const char *path, const char *options_json,
                                               zebkit_library_func ready, void *data, char **error);

/* @args_json: a JSON array (NULL is []). Arguments are copied before return. @timeout_ms:
 * 0 the library's default, < 0 none. Returns the call's number, and @func is told exactly
 * once, after this returns; 0 when refused at once, and @func is never told. */
ZEBKIT_API uint64_t zebkit_library_call(zebkit_library *library, const char *function,
                                        const char *args_json, const zebkit_part *parts,
                                        size_t part_count, int64_t timeout_ms,
                                        zebkit_library_func func, void *data);

/* @func is told CANCELLED, unless told already. */
ZEBKIT_API void zebkit_library_cancel(zebkit_library *library, uint64_t call);

/* Untold calls are told GONE after this returns; the handle is freed after the last. */
ZEBKIT_API void zebkit_library_close(zebkit_library *library);
```

Four functions, not three: a call waiting on the network must end without
closing the library. Load and close are for the engine's thread; call and
cancel for any thread, as `zebkit_call_resolve()` is
(`src/engine/include/zebkit/zebkit.h:726-741`), so async runtimes on other
threads can call. Callbacks run on the engine's thread. A refused call never
calls back, the rule of `zebkit.h:30-35`.

Load options (JSON): `backend` (`"isolate"`, or `"page"`), `timeoutMs`
(30000), `queue` (64 waiting calls), `heapLimitMB` (128), `maxBytes` (64 MiB
each way), `network` (origins `fetch()` may reach; default none), and for
pages `width`, `height`.

**Values.** JSON plus parts; a part is a `Uint8Array` in JavaScript. An object
with a `"$part"` key and another key is BAD_ARGUMENTS. Depth is limited to
200, as `src/engine/call.cc:69` limits it. Promises are awaited; `undefined`
is `null`; functions, `BigInt`, `NaN`, cycles and class instances are
BAD_RESULT. No base64, unlike `zebkit.app`'s `{"$bytes": ...}`
(`src/engine/bridge.cc:623`).

**Bindings**, each in its own idiom, with the same features:

| | Values | Result |
|---|---|---|
| Rust | `json::Value` + `Vec<Vec<u8>>` | `Call`, a `Future` woken from the engine's thread; `.then(f)` |
| Python | values with `bytes` in place | zebkit's `Future` (`bindings/python/zebkit/automation.py:100`), or `function=` |
| Go | `json.RawMessage` + `[][]byte` | a callback, or a channel |
| Host JS | values with `Uint8Array` in place | a promise; `library.cancel(promise)` |

The Rust crate has no futures today (`bindings/rust/zebkit/src/view.rs:734`
takes a closure); `Call` is the first.

## Calls

One at a time per library, in order; libraries run in parallel. Every
accepted call is told once, with a value or a code. At its deadline a call
whose script is **running** is stopped by `TerminateExecution()`, which any
thread may call (`v8/include/v8-isolate.h:1293-1297`): it is TIMEOUT and the
library ends (other calls GONE). A call that is **waiting** (a timer,
`fetch()`) is TIMEOUT and the queue moves on; its late result is dropped.
Cancel removes a queued call; a running one is told CANCELLED at once and
the next starts after it settles. A library that ends (heap, termination, its
renderer gone) fails pending calls with GONE and is not restarted: replaying
calls with side effects is unsafe.

## Backend "isolate"

A cut-down `zebkit_script`: its V8 set-up (`src/engine/script.cc:1131-1175`)
and module loader (`script.cc:4258-4324`), without the `zebkit` object
(`script.cc:563`), views, files or schemes.

- **Thread.** Each library has its own, with one isolate created and used
  there (`gin::IsolateHolder::kSingleThread`,
  `gin/public/isolate_holder.h:46-48`), so no `v8::Locker`. The host script
  runs on the engine's thread (`script.cc:1144-1146`); a long library call
  there would stall every page's events.
- **Memory.** A heap limit through `CreateParams` (`isolate_holder.h:94-101`)
  and a near-heap-limit callback (`v8-isolate.h:1711-1715`) that terminates
  the script and ends the library with OUT_OF_MEMORY rather than aborting the
  process; `ArrayBuffer`s counted by the library's own allocator. A V8 crash
  of another kind still ends the application.
- **Web APIs.** Today's host script has `console`, timers,
  `TextEncoder`/`TextDecoder` and `atob`/`btoa` in JavaScript
  (`script.cc:89-160`). The library isolate adds, native where speed matters:
  `queueMicrotask`, native UTF-8 `TextEncoder`/`TextDecoder`, `URL` and
  `URLSearchParams` (on `GURL`), `structuredClone` (`v8::ValueSerializer`),
  `crypto.getRandomValues`/`randomUUID`, `performance.now`,
  `AbortController`, `EventTarget`; V8's `WebAssembly` and `Intl` (ICU data:
  English and CJK, `scripts/build-icu-data.sh`); `fetch` only to the granted
  origins, through the stack `zebkit_fetch()` uses
  (`src/engine/services.cc:231-234`), without cookies. No DOM, Canvas,
  fonts, workers or storage.
- **Coverage (expected; stage 1 tests the first two).** ECharts SVG with
  `ssr: true` (text width estimated without a canvas), markdown-it, marked,
  date-fns, lodash, zod, papaparse on strings, pdf-lib (the PDF returns as a
  part), highlight.js, Prism, Shiki with its wasm bundled. Libraries that
  measure text, draw on a canvas or lay out HTML need `page`.

## Backend "page"

A page without a window, in a sandboxed renderer:

- **Origin.** One internal scheme, `zebkit-library`, standard and secure,
  registered by the engine before start, since schemes cannot be added
  later (`zebkit.h:851-857`; `src/engine/scheme.cc:368-380`, from
  `src/engine/engine.cc:688-690`). Each instance is
  `zebkit-library://<random id>/`, an origin and with site isolation a
  process of its own. Files are served from the folder with a path check
  and a CSP allowing only that origin and the granted ones.
- **Profile.** In memory and its own, as `zebkit_context_new()` makes
  (`src/engine/view.cc:1569-1577`): none of the application's cookies.
- **Window.** Off-screen views are refused on Windows outside `--headless`
  and on Wayland (`src/engine/offscreen.cc:2095-2108`); a view made with
  `{"show": false}` (`src/engine/include/zebkit/window.h:47,61`) works
  everywhere. To keep a hidden page's timers unthrottled it holds
  `IncrementCapturerCount(size, stay_hidden=false, ...)`
  (`content/public/browser/web_contents.h:923-937`); the first prototype
  confirms this.
- **No user gesture.** `zebkit_view_evaluate()` runs script with
  `userGesture: true` (`src/engine/view.cc:587`). The page backend evaluates
  nothing: a runner in the page pulls work over the frame's existing
  `PageCallHost` channel (`src/engine/page_call.mojom`), calling
  `library.next`, which the browser answers when a call is queued, and
  `library.done` with the result. The browser knows the instance from the
  channel (`src/engine/call.cc:445-482`). That channel already carries JSON,
  parts and shared memory past 64 KiB; no new Mojo interface is needed.
- **Network.** Other requests fail in the browser, in a URL loader factory
  in front of the network's, where zebkit's request hooks sit
  (`src/engine/network_hooks.cc`); CSP alone does not cover every request.

## Bundling npm packages

`node js-bundle.mjs <package or entry> <folder>`, in the release archive. It
installs the package with npm `--ignore-scripts` (as `scripts/lib.sh:244-253`
installs tools) and runs esbuild, already used by
`scripts/playwright-sources.mjs` at `upstream.lock`'s 0.28.1, with Node.js
at mise.toml's 24.22.0: one ES module, neutral platform, browser fields
first, `NODE_ENV=production`, JSON and wasm inlined, licences to a file.
Build time only; zebkit never downloads code. It cannot bundle native
addons, Node.js built-ins (it names each such import; an optional one can be
replaced by an empty module) or computed imports. Code that needs `eval`
works in the isolate, not under the page's CSP.

## Measuring

Targets, not measurements:

| | isolate | page |
|---|---|---|
| Load to ready: markdown-it / ECharts (about 1 MiB) | 15 / 60 ms | 150 / 250 ms |
| Call overhead, 100 B JSON each way, median | 50 µs | 500 µs |
| 1 MiB part there and back | 1 ms | 3 ms |
| Memory: empty instance / ECharts after 100 renders | 3 / 30 MiB | 35 / 60 MiB |

`scripts/library-bench.sh` runs `tests/bench/library/bench.c` on the release
package: a steady clock from load to `ready` and around 10,000 calls of an
`echo` function (median and 99th percentile over 20 fresh processes);
resident memory before and after 10 instances, divided by 10, with the
renderers' for `page`. Beside it, the same work through a hidden view and
`zebkit_view_evaluate()`: the page backend must not be slower. Report:
`docs/reports/library-bench.md`.

## Stages

1. `isolate`, the C API, Rust, Python, Go and host JavaScript, `js-bundle.mjs`,
   the benchmark. Tests per binding: JSON and parts echoed, non-ASCII text,
   each code, both deadlines, cancel, full queue, close with calls pending,
   heap exhaustion, a path leaving the folder, `fetch()` refused and allowed
   (local server). Examples: ECharts to SVG, markdown-it to HTML.
2. `page`, tested with a library that measures text in a canvas, and with
   images, CSS and workers refused as well as `fetch()`.
3. Only when needed: V8 code cache for libraries, several isolates per
   library, events to the application, a library's page shown in a view.

## Security defaults

No files, no network, no `zebkit` object, no profile cookies or storage;
network only to origins given at load, refused in the browser; no user
gesture, popups, downloads or permissions in pages; limits on heap, queue,
size and time. A library is code the application ships, not a plug-in from
elsewhere.
