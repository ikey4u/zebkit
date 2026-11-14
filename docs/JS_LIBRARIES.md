# JavaScript libraries as normal libraries

Status: stage 1 is built (2026-11-13): the `isolate` backend, the C API, the
Rust, Python, Go and host JavaScript bindings, `tools/js-bundle.mjs`, the
benchmark and `examples/js-libraries`; and of stage 3, the code cache, the
idle shrink, `instances`, the application's functions (`host.call()`) and the
bundler's CommonJS packages, `--list` and `--wrapper` (2026-11-13). Stage 2,
`page`: "Backend \"page\"". It replaces the earlier proposal. "Built: what
changed" below lists where building changed the design.

An application loads a bundled JavaScript library and calls its functions
from C, Rust, Python, Go or host JavaScript, as it calls any library:

```rust
let charts = zebkit::js::Library::load("resources/libs/echarts", &Value::Null)?;
let svg = charts.call("renderSvg", Value::Array(vec![spec])).await?.value; // a JSON string
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
`src/engine/script.cc:4284-4324`), besides `zebkit:host`, the application's
functions ("The application's functions"). Data files and WebAssembly are put in the
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

/* host.call(@name, ...args) in the library, @name one of the load's "host": answer
 * @request once, from any thread. On the engine's thread. */
typedef void (*zebkit_library_host_func)(zebkit_library *library, uint64_t request, const char *name,
                                         const char *args_json, const zebkit_part *parts,
                                         size_t part_count, void *data);
ZEBKIT_API void zebkit_library_set_host(zebkit_library *library, zebkit_library_host_func func,
                                        void *data);
/* OK resolves host.call() with @json and @parts, another code rejects it ({"message",
 * "name"}). 1 when taken; 0 when answered already, past its deadline, or its instance ended. */
ZEBKIT_API int zebkit_library_answer(zebkit_library *library, uint64_t request, zebkit_library_code code,
                                     const char *json, const zebkit_part *parts, size_t part_count);

/* Untold calls are told GONE after this returns; the handle is freed after the last. */
ZEBKIT_API void zebkit_library_close(zebkit_library *library);
```

Four functions for the calls, not three: a call waiting on the network must
end without closing the library; two more for the application's functions.
Load, close and set_host are for the engine's thread; call, cancel and answer
for any thread, as `zebkit_call_resolve()` is
(`src/engine/include/zebkit/zebkit.h:726-741`), so async runtimes on other
threads can call and answer. Callbacks run on the engine's thread. A refused
call never calls back, the rule of `zebkit.h:30-35`.

Load options (JSON): `backend` (`"isolate"`, or `"page"`), `timeoutMs`
(30000; also the deadline of the application's answer to `host.call()`),
`queue` (64 waiting calls, and as many requests to the application),
`heapLimitMB` (128, each instance's), `maxBytes` (64 MiB each way), `network`
(origins `fetch()` may reach; default none), `instances` (1 to 64, default 1),
`codeCache` (true), `host` (the names of the application's functions), and
for pages `width`, `height`.

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
takes a closure); `Call` is the first. The application's functions, in each
binding's idiom:

| | Given at load | A function |
|---|---|---|
| Rust | `Library::load_with_host(path, &options, Host::new().function(..).function_async(..))` | a closure `Fn(Reply) -> Result<Reply, String>`, or one returning a `Future` of that (polled on the engine's thread, woken from any) |
| Python | `load(path, options, host={name: f})` | `f(*args)`: its value, a coroutine (run with `zebkit.automation.run()`) or a Future set from any thread; what it raises rejects |
| Go | `LoadLibraryWithHost(path, options, map[string]LibraryHostFunc{..}, ready)` | `func(args json.RawMessage, parts [][]byte) (json.RawMessage, [][]byte, error)`, run on a goroutine of its own |
| Host JS | `Library.load(path, {host: {name: f}})` | `f(...args)`: a value or a promise; what it throws rejects |

## Calls

One at a time per instance, in order, each on the first instance free;
libraries run in parallel. Every accepted call is told once, with a value or
a code. At its deadline a call whose script is **running** is stopped by
`TerminateExecution()`, which any thread may call
(`v8/include/v8-isolate.h:1293-1297`): it is TIMEOUT and its instance ends. A
call that is **waiting** (a timer, `fetch()`, the application's answer) is
TIMEOUT and the instance takes the next; its late result is dropped. Cancel
removes a queued call; a running one is told CANCELLED at once and the next
starts after it settles. An instance that ends (heap, termination) is not
restarted; the other instances take the calls, and with the last one the
library ends (pending calls GONE; its renderer gone ends a page). Nothing is
replayed: calls with side effects cannot safely be.

**Instances.** `"instances": N` loads the library N times, each in an isolate
on a thread of its own, and the calls go to whichever is free, so that N
calls run at once. Each instance has its own module state (a counter, a
cache the library keeps), its own heap of `heapLimitMB`, and loads the
library once; `ready` is told once every instance has loaded (LOAD_FAILED if
one did not). A call that changes state changes one instance's: libraries
whose functions are pure (render this, parse that) suit a pool; one that
keeps state between calls wants one instance. An instance stopped (its
deadline, its heap) ends alone, as above: restarting it would hide the
failure and cost a load, while the others go on.

## The application's functions

A library calls functions the application gives at load (`"host"` names them,
`zebkit_library_set_host()` answers them), as an ES module import:

```js
import host from 'zebkit:host';          // or: import {call, functions} from 'zebkit:host'

export async function report(rows) {
  for (let i = 0; i < rows.length; ++i) await host.call('progress', i, rows.length);
  const logo = await host.call('asset', 'logo.png');   // {"$part": 0}: a Uint8Array here
  return build(rows, logo);
}
```

An import, not a global: it reads as where the function comes from, bundles
(`js-bundle.mjs` keeps `zebkit:host` an import), and a library that uses none
sees nothing new. Every library may import it, a one-file one too. Values are
JSON and parts both ways, as a call's. `host.call()` of a name not given is a
TypeError at once; the application's error (what its function throws) is an
Error of its name and message; an answer not given within `timeoutMs` is a
TimeoutError, and the answer after it is refused (`zebkit_library_answer()`
returns 0), as a second answer is: each request is answered exactly once.
The application answers from any thread; requests are given to it on the
engine's thread, those made before `zebkit_library_set_host()` once it is
called (a library may ask at load, in its top level). At most `queue`
requests wait; close drops them, and the call waiting on one is GONE.

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
  the script and ends the instance with OUT_OF_MEMORY rather than aborting the
  process; `ArrayBuffer`s counted by the library's own allocator. A V8 crash
  of another kind still ends the application. An instance idle for 2 seconds
  (no call started since its last ended) calls `LowMemoryNotification()`
  (`v8-isolate.h:1521`): full collections that compact the heap and give its
  free pages back. "Measuring" has what it gains and costs.
- **Load: the code cache.** A module is compiled with the code cache V8 made
  of the same text before (`ScriptCompiler::kConsumeCodeCache`,
  `v8-script.h:810`), when there is one: by the text's SHA-256 and
  `CachedDataVersionTag()` (V8's version and flags), in memory for the
  process (64 MiB at most) and in the profile's folder `Library Code Cache`,
  a file per module path holding its key, so a changed module replaces its
  file and the folder never grows past the modules loaded; never in the
  library's folder. The cache is made (`CreateCodeCache()`) when an instance
  is first idle, or at close, so it has the functions the calls compiled, not
  only the top level; V8 checks it and a cache it refuses is deleted.
  `"codeCache": false` turns both off.
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

Built (2026-11-13; `src/engine/library_page.cc`, `library_page.h`,
`library_page_runner.h`, `library_page_renderer.cc`). A library in a page of
its own, for what needs a DOM, a canvas, fonts or layout: `{"backend":
"page"}`, with `width` and `height` (CSS pixels, 800 by 600 by default), the
same four functions, values, codes and exactly-once completion; every binding
passes the options through and needs nothing new. `instances` and `host` are
the isolate's (refused with a page); `codeCache` is not used (Blink's own,
in the page's memory).

```python
diagram = zebkit.library.load("libs/diagram", {"backend": "page"})
svg = diagram.call("renderSvg", "graph TD\n  A --> B\n")   # Mermaid lays it out in the DOM
```

- **Origin.** One scheme of the engine's, `zebkit-library` (standard,
  secure, CORS-enabled and fetch-capable; reserved, so an application cannot
  register it), added to every process's schemes before start
  (`AddLibraryPageScheme()` from `scheme.cc`'s `AddSchemes()`). Each instance
  is `zebkit-library://<32 random hex digits>/`. `/` is the engine's own
  document (the runner, the entry's name in a `<meta>`, nothing else); every
  other path is a file of the library's folder, each segment unescaped and
  refused if it is `.`, `..` or holds a separator, then resolved with
  `LibraryRealPath()` (the isolate's `RealPath()`, links and on Windows
  junctions resolved) and served only inside the folder; a one-file library
  serves that file alone. Files go as their type by name
  (`net::GetMimeTypeFromFile`), `nosniff`, `no-store`. The loaders exist only
  for a library's own profile (navigation: `CreateNonNetworkNavigationURLLoaderFactory`;
  subresources: `RegisterNonNetworkSubresourceURLLoaderFactories`; workers:
  none), so another page asking for such a URL gets nothing.
- **CSP**, on the document: `default-src 'none'`; scripts from itself, the
  runner by its hash and `'wasm-unsafe-eval'`; styles from itself and inline;
  images, fonts, media and `connect-src` from itself, `data:`, `blob:` and the
  granted origins; frames from itself, `data:` and `blob:`; `worker-src`,
  `object-src`, `base-uri`, `form-action`, `manifest-src` `'none'`; and
  `sandbox allow-scripts allow-same-origin`: no popups, downloads, modal
  dialogs (`alert()` returns at once rather than blocking a hidden page),
  forms, pointer lock or top-level navigation. No `'unsafe-eval'`: a library
  that needs `eval` or `new Function` works in the isolate, not here.
- **Profile.** Its own, in memory (`new Profile(off_the_record)` and
  `ConfigureContext()`, as `zebkit_context_new()` makes one): none of the
  application's cookies, storage, permissions or cache. The profile carries
  the page's description as user data, so the fence, the loaders and the
  renderer's switches know a library's profile whatever frame, worker or
  process asks; it goes with the page (`DestroyProfileWhenUnused()`).
- **Window.** A `zebkit::Window` made with `{"show": false}` (as
  `zebkit_view_new_with_options()` makes one, but no `zebkit_view`: no
  DevTools session, no bridge, no view the application sees), so it works on
  Windows, Wayland and under `--headless` where off-screen views do not.
  `IncrementCapturerCount(gfx::Size(), stay_hidden=false, stay_awake=false,
  is_activity=false)` (`content/public/browser/web_contents.h:933`) makes its
  renderer treat the page as visible: `document.visibilityState` is
  `"visible"`, 100 nested `setTimeout(0)` take a few milliseconds rather than
  about a second each, and `requestAnimationFrame` runs (tested headless and
  in an X11 window under Xvfb). WebRTC's own sockets, which no URL loader
  sees, are limited by `webrtc_ip_handling_policy =
  disable_non_proxied_udp` (none without a proxy).
- **No user gesture.** Nothing is evaluated in the page. The runner
  (`library_page_runner.h`), the document's only script, takes the frame's
  `__zebkit_call` (which `page_call_renderer.cc` leaves in a
  `zebkit-library` main frame instead of deleting it) and deletes it before
  importing the library, then keeps one request `library.next` out over
  `PageCallHost` whenever the page is free; the browser answers it with a
  call (`<id>\n<part sizes>\n<name as JSON>\n<arguments>`, the parts in the
  answer's bytes, shared memory past 64 KiB), and the runner sends
  `library.done` with the result (`<id>\n<code>\n<sizes>\n<JSON>`, the
  parts as bytes). `call.cc` hands `library.*` calls of a library's page to
  `TakeLibraryPageCall()` before anything else; the browser knows the
  instance from the frame (its page's main frame, of its origin), checks the
  JSON the renderer wrote (`IsPlainJson()`) and `maxBytes`, since the
  renderer is not trusted to write what the application parses. The runner's
  value model is the isolate's (`revive`, `check`, `replaceParts` of
  `library_bootstrap.h`): the same codes for the same values.
  `navigator.userActivation.isActive` and `hasBeenActive` are false and
  `window.open()` returns null (tested).
- **Network.** `FenceLibraryPageNetwork()` is the first interceptor
  `WillCreateURLLoaderFactory()` adds for a library's profile, whatever the
  factory's type (navigation, document subresources, workers, service
  workers, downloads, prefetch): a request passes only to the page's own
  origin, `data:`, `blob:`, `about:`, or an `http(s)` origin of `network`;
  others fail with `ERR_BLOCKED_BY_CLIENT` before any connection, and a
  redirect out of the granted origins ends the request. WebSockets are
  refused (`WillInterceptWebSocket()`/`CreateWebSocket()`), network hints
  (preconnect, DNS prefetch) not bound, `CanCreateWindow()` refuses popups,
  and a navigation throttle lets the main frame load only its own document
  (the engine's navigation), so `location.href = ...` does nothing. Granted
  origins on this machine work too: a library's document is of the loopback
  address space (`DetermineAddressSpaceFromURL()`), so Local Network Access
  asks no permission; the fence decided already. Requests carry the page's
  profile's cookies only, which start empty. Tested with a local server that
  counts every connection: images, CSS (`<link>`, `@import`, `url()`),
  fonts, workers (by URL, `blob:`, shared, the folder's own), `fetch()`
  (`cors` and `no-cors`), WebSocket, an iframe, navigation,
  `sendBeacon()`, `<link rel=preconnect|dns-prefetch|prefetch|preload>`, a
  download and a form: no connection arrived.
- **Deadlines.** A call's deadline counts from when the runner was given it.
  If the runner's next request is out by then, the page was waiting (a timer,
  `fetch()`): TIMEOUT, and the next call goes. If not, it is running script:
  after 500 ms more for a request in flight, its renderer is stopped
  (`RenderProcessHost::Shutdown()`), the call is TIMEOUT and the library ends
  (the others GONE), as an isolate's `TerminateExecution()` ends it. A load
  that has not ended by `timeoutMs` (a top level that spins or waits) is
  LOAD_FAILED and its page goes.
- **Memory.** `heapLimitMB` is V8's old space in the page's renderer
  (`--max-old-space-size` added to that renderer's `--js-flags` alone,
  `AppendLibraryPageSwitches()`). V8 crashes a renderer whose heap is full,
  which the browser cannot tell from another crash, so the renderer's own
  side (`library_page_renderer.cc`) adds a near-heap-limit callback that tells
  the browser (`library.oom`) and gives room until the browser stops the
  renderer: OUT_OF_MEMORY for the call, the library ended. What is not
  enforced: `ArrayBuffer`s, canvases and images outside V8's heap are bounded
  only by the renderer's own limits (PartitionAlloc's, the system's), and
  Blink's own memory (DOM, layout) is not counted.
- **A renderer gone** (crashed, killed by the system): the call running and
  those queued are GONE, later calls GONE, the page and its profile torn
  down; nothing is restarted. Close does the same with GONE.

**Built: what changed** from the design above it replaces:

- **A window, not a view**: `zebkit::Window::Create()` with `{"show": false}`
  and no `zebkit_view`, so there is no DevTools session, bridge script or
  view in `zebkit_view`'s lists; the window is what makes it work everywhere.
  `IncrementCapturerCount(stay_hidden=false)` is confirmed: timers and
  `requestAnimationFrame` run headless and in a hidden X11 window.
- **Running or waiting is told by the runner's request**: the browser cannot
  ask a renderer whether script runs, so the runner keeps its request for the
  next call out whenever it is free, and a call past its deadline without it
  (500 ms more) is running: its renderer is stopped.
- **The heap is told by the renderer**: `--max-old-space-size` per library
  renderer, and a near-heap-limit callback there that tells the browser,
  since V8's own end of a full heap is a crash like any other.
- **The fence lets the page's own origin, `data:`, `blob:` and `about:`
  through**: content wraps the non-network loaders (the page's own files, a
  `data:` frame) with the same `WillCreateURLLoaderFactory()` interceptors.
- **The page is of the loopback address space**, so a granted origin on this
  machine is reached without a Local Network Access prompt no one would see.
- **Messages are text frames, not JSON**: the browser hands the arguments and
  the result on without parsing them, and only checks the result
  (`IsPlainJson()`), as `zebkit.app`'s calls are checked.
- **`instances` and `host` are refused** with a page, `codeCache` unused.

Not done: the device scale (the display's), events from the page to the
application, a library's page shown in a view, `instances` of pages and
`host.call()` from a page. Library pages are DevTools targets, as every
`WebContents` is: a CDP client of the application (`--remote-debugging-port`)
sees and can debug them.

**Tests** (`tests/engine/library_page.py`, 40 checks, run headless and in an
X11 window: `library-page`, `library-page-x11`; with the bindings' checks
`rust-engine`, `go-check`, `library-page-javascript`): the fixture
`tests/engine/library_page_fixture` measures text in a canvas (wider for more
text, twice as wide at twice the size), the system's fonts, layout and
`getComputedStyle()`, SVG through the DOM with `getBBox()`, a PNG from an
`OffscreenCanvas` as a part; JSON, non-ASCII text, parts (an empty one, 1 MiB);
THROWN, NO_FUNCTION, BAD_RESULT (a BigInt, a DOM node), BAD_ARGUMENTS through
the C function; its own files, a link out of the folder (404), `%2e%2e`; the
network as above and a granted origin (no cookies of the application's; a
redirect out fails); two instances' `localStorage`, `sessionStorage` and
IndexedDB apart; a waiting call TIMEOUT and the next runs; a spinning page
stopped (TIMEOUT, then GONE) within seconds; a spinning load; the heap
(OUT_OF_MEMORY); a renderer killed (GONE);
cancel, a full queue, close with calls pending and while loading, four at
once, every closed instance's renderer gone; Mermaid's bundle.

**Measured**, `page`, 2026-11-13: zebkit 0.5.0 (`slim`, Chromium
157.0.8103.38) on Linux x86-64, 24 threads, headless;
`scripts/library-bench.sh slim --backends page`, medians of 20 fresh
processes (`tests/bench/library/bench.c` with `{"backend": "page"}`); memory
"with its processes" adds the proportional set size of the process's
children (the page's renderer, the network service) to the process's own
resident growth (`emptyInstanceAllMiB`, `chartsAfter100AllMiB`).

| | target | measured |
|---|---|---|
| Load to ready: markdown-it / ECharts | 150 / 250 ms | 41.9 / 78.1 ms (an empty library: 32.2 ms) |
| Call overhead, 100 B JSON each way: median / p99 | 500 µs | 117 / 283 µs |
| 1 MiB part there and back | 3 ms | 3.43 ms |
| Memory: empty instance / ECharts after 100 renders, with its processes | 35 / 60 MiB | 10.3 / 59.3 MiB |

The application's own process grows 1.2 MiB per empty page (the window, the
profile); the rest is the renderer. ECharts' first render takes 58.7 ms, the
next 2.1 ms each; 40 renders at once on one page 66 ms. Mermaid
(`examples/js-libraries/diagram`, 5.2 MiB; median of 5 processes, through
the Python binding): load to ready 240 ms, the first flowchart 303 ms, the
next 33 ms each, 179 MiB with its processes after 20. A hidden view's
`zebkit_view_evaluate()` of the same echo takes 164 µs and the page backend's
call 154 µs, both from Python: the page backend is not slower. The 1 MiB part
is copied four times on its way (the application's buffer into the call, into
the channel's shared memory, into the page's `ArrayBuffer` and back, once
fewer for a call with one part); it is over the target by 0.4 ms.

**Example**: `examples/js-libraries/diagram` (Mermaid 12.2.0, 5.2 MiB
minified, 66 packages; `build.sh` bundles it beside the others, and the run
is offline after `engine-test.sh --fetch`): `renderSvg(text)`,
`renderSvgBytes(text)`, `diagramType(text)`. Mermaid sizes its boxes by
measuring labels in the DOM, which the isolate has not. Its bundle has one
`new Function` (d3-dsv's CSV parser), which the CSP refuses; the diagrams
that parse CSV (Sankey) fail with it.

## Bundling npm packages

`node js-bundle.mjs <package, package folder or entry> <folder>`, in the release archive. It
installs the package with npm `--ignore-scripts` (as `scripts/lib.sh:244-253`
installs tools) and runs esbuild, already used by
`scripts/playwright-sources.mjs` at `upstream.lock`'s 0.28.1, with Node.js
at mise.toml's 24.22.0: one ES module, neutral platform, browser fields
first, `NODE_ENV=production`, JSON and wasm inlined, licences to a file.
Build time only; zebkit never downloads code. It cannot bundle native
addons, Node.js built-ins (it names each such import; an optional one can be
replaced by an empty module) or computed imports. Code that needs `eval`
works in the isolate, not under the page's CSP.

**CommonJS packages** need no entry: the package is resolved as esbuild
resolves it, and when that is CommonJS the names it exports are read with
cjs-module-lexer (2.2.1, the lexer Node.js uses for the same, installed
beside esbuild), which reads `exports.x =`, `module.exports = {...}` and
re-exports of its own files without running the code; each becomes a
function of the library (bound to the package), and `module.exports` itself
is `default`. Names a package makes at run time are not found; an entry
names them.

**What the bundle exports** goes to `<folder>/exports.json`: each function
(listed by importing the bundle in a Node.js process of its own, as
`ready` lists them), its doc comment, and its parameters' and return types,
from the entry's JSDoc (`@param {string} text`, `@returns {string}`) or the
package's TypeScript declarations (its `types`, or `@types/<name>`).
`--list <folder>` prints them; `--wrapper <rust|python|go|ts> <folder>
[--name Name]` prints a typed facade: a type with a method per function and
its doc comment, over `zebkit::js::Library` (Rust: an `async fn` per
function), `zebkit.library` (Python: a Future per call), `zebkit.LoadLibrary`
(Go: a `done` callback per call), or TypeScript declarations of
`zebkit.Library`'s calls (host JavaScript). string, number, boolean,
`Uint8Array` (a part), void and arrays of the first three are typed; any other
type is a JSON value. `examples/js-libraries/markdown` has markdown-it's
Rust and Python facades, which the tests use and check are what the
generator makes.

## Measuring

Targets:

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
`zebkit_view_evaluate()`: the page backend must not be slower. Each fresh
process is followed by another on the same profile, which loads with the code
cache the first left on disk. It writes `docs/reports/library-bench.md`, a
column per backend.

Measured, `isolate`, 2026-11-13: zebkit 0.5.0 (`slim`, Chromium
157.0.8103.38) on Linux x86-64, 24 threads; `scripts/library-bench.sh slim`,
medians of 20 fresh processes; the bundles of `examples/js-libraries`
(markdown-it 14.1.0, 144 KiB; ECharts 5.6.0, 1016 KiB, minified). The
100-byte call is measured over 2,000 calls (after 200 to warm up) rather than
10,000. "Stage 1" is the engine before stage 3's changes, measured with the
same benchmark the same day; "now" is `docs/reports/library-bench.md`.

| | target | stage 1 | now |
|---|---|---|---|
| Load to ready: markdown-it, first in the process | 15 ms | 12.2 ms | 11.1 ms |
| ... again in the process / a new process with the disk cache | | 9.8 / 12.4 ms | 10.5 / 7.5 ms |
| Load to ready: ECharts, first in the process | 60 ms | 43.9 ms | 46.8 ms |
| ... again in the process / a new process with the disk cache | | 49.4 / 43.5 ms | 18.8 / 17.3 ms |
| ECharts' first render: first load / again / disk cache | | 25.5 / 29.6 / 25.2 ms | 26.4 / 11.4 / 10.0 ms |
| ECharts' next renders, each | | 1.87 ms | 1.89 ms |
| Call overhead, 100 B JSON each way: median / p99 | 50 µs | 29.0 / 59.8 µs | 29.8 / 76.3 µs |
| 1 MiB part there and back | 1 ms | 1.05 ms | 1.06 ms |
| `host.call()` answered at once (a call that makes one) | | | 45.8 µs |
| Memory: an empty instance | 3 MiB | 1.8 MiB | 1.8 MiB |
| Memory: ECharts after 100 renders / then 3 s idle | 30 MiB | 28.4 / 28.4 MiB | 28.8 / 11.9 MiB |
| ECharts' first render after 3 s idle / the next | | 1.9 / 2.0 ms | 8.2 / 5.2 ms |
| 40 ECharts renders made at once: 1 instance / 4 | | 59.7 ms / - | 69.8 / 30.8 ms |

(Stage 1's own report of 2026-11-13 had ECharts at 34.0 MiB after 100
renders: measured from before markdown-it was closed, not after it.)

**The code cache** makes ECharts' load 2.7 times as fast in a process that
has loaded it before or that finds the disk cache (46.8 to 17.3 ms), and its
first render too (26.4 to 10.0 ms): together 73 ms to 27 ms before the first
chart. markdown-it gains less (11.1 to 7.5 ms): its load is mostly the
isolate's making. A load with no cache to find costs what hashing the text
costs (SHA-256: about 1 ms for ECharts' 1 MiB, inside the noise of 20 runs).
The disk cache is kept: it is the gain for an application's every start, its
place is the profile (never the library's folder), its size about the
modules' own (ECharts' file is 1.0 MiB, markdown-it's 178 KiB; a file per
module path, replaced when the module changes), and `codeCache: false` turns
it off.

**The idle shrink**, measured as the policies it was chosen from (8 fresh
processes each, cold profiles, an idle of 1 s then; the same benchmark):

| After a call | ECharts render, each | after 100 renders | after 3 s idle | first render after idle / the next | call, 100 B |
|---|---|---|---|---|---|
| nothing (stage 1) | 1.75 ms | 27.8 MiB | 27.8 MiB | 1.9 / 2.0 ms | 31.5 µs |
| idle: `LowMemoryNotification()` (kept) | 1.79 ms | 28.4 MiB | 11.5 MiB | 8.1 / 4.5 ms | 31.4 µs |
| idle: `MemoryPressureNotification(kModerate)` | 1.89 ms | 35.2 MiB | 23.7 MiB | 7.1 / 3.6 ms | 30.5 µs |
| idle: `MemoryPressureNotification(kCritical)` | 1.87 ms | 35.4 MiB | 24.1 MiB | 6.9 / 3.3 ms | 29.8 µs |
| each call: `LowMemoryNotification()` | 8.21 ms | 17.4 MiB | 20.6 MiB | | 907 µs |
| each call: `MemoryPressureNotification(kModerate)` | 4.01 ms | 20.0 MiB | 18.7 MiB | | 78 µs |

After each call is not cheap (the call costs 2.5 to 30 times as much), so it
is not done. Idle, `LowMemoryNotification()` gives back 59% of ECharts'
memory (28 to 12 MiB, under the 30 MiB target) where the pressure
notifications give back 15%; what it costs is the first two calls after the
idle (about 9 ms more, once: V8 grows its heap again and compiles again what
it dropped), not the calls in between. An idle of 2 s rather than 1 s makes
that cost rarer for an application that calls every second or so.

**Instances**: 40 ECharts renders made at once take 30.8 ms on 4 instances,
69.8 on one (2.3 times as fast; each instance's first renders compile its
code, which the code cache shares). Each instance costs its own memory:
1.8 MiB empty, and its heap as the library grows it.

## Stages## Stages

1. `isolate`, the C API, Rust, Python, Go and host JavaScript, `js-bundle.mjs`,
   the benchmark. Tests per binding: JSON and parts echoed, non-ASCII text,
   each code, both deadlines, cancel, full queue, close with calls pending,
   heap exhaustion, a path leaving the folder, `fetch()` refused and allowed
   (local server). Examples: ECharts to SVG, markdown-it to HTML.
2. `page`, tested with a library that measures text in a canvas, and with
   images, CSS and workers refused as well as `fetch()`.
3. Only when needed: V8 code cache for libraries, several isolates per
   library, events to the application, a library's page shown in a view.
   Built (2026-11-13): the code cache, `instances`, the application's
   functions (`host.call()`, which events to the application are: a call
   whose answer may be null), with the idle shrink and the bundler's
   CommonJS packages, `--list` and `--wrapper`. Left: a library's page shown
   in a view.

## Built: what changed

Stage 1, 2026-11-13. Where building showed the design needed a choice or a
change:

- **A call's deadline counts from when it starts to run**, not from the
  call: a call waiting its turn is not TIMEOUT for the calls before it. The
  load has a deadline too, `timeoutMs`: a top level that never ends is
  stopped (LOAD_FAILED) as a call is.
- **Refused at once (0, never told) only for a NULL library or function, or a
  closed library.** QUEUE_FULL, BAD_ARGUMENTS (too large) and calls of a
  library that failed or ended (LOAD_FAILED, GONE) are told like any outcome,
  so that an application has one path for failures.
- **`ready` is told GONE** when the library is closed before its load ends;
  the design had only OK and LOAD_FAILED, and every accepted callback is told.
- **A one-file library imports nothing**: its folder may hold other things
  (the application's resources), which "imports stay in the folder" would let
  it read as modules. A folder with `index.mjs` imports inside it, with links
  resolved (symbolic links everywhere; on Windows junctions and short names
  too, by the final path of the opened file); bare and absolute specifiers
  are refused.
- **`fetch()` does not go through `zebkit_fetch()`**, which sends the
  profile's cookies: `library_web.cc` makes its own request on the same
  network stack (the default profile's loader factory, its cache and proxy)
  with credentials omitted, so no cookies are sent or kept, and follows a
  redirect only to a granted origin.
- **ArrayBuffers counted against `heapLimitMB` fail their allocation** (a
  RangeError the library may catch, THROWN if it does not) rather than ending
  the library; only V8's heap reaching its limit is OUT_OF_MEMORY.
- **Results:** any typed array or ArrayBuffer is a part, not only a
  Uint8Array; an object with a `"$part"` key in a result is BAD_RESULT, since
  the receiver could not tell it from a part.
- **No `self`, `window`, `navigator` or `process`**: ECharts takes a defined
  `self` without `document` for a worker and draws differently; libraries see
  the environment Node.js gives a bundle, less Node's own modules.
  `TextDecoder` is UTF-8 only.
- **The host script had no `URL` or `structuredClone`** (`docs/API.md`);
  only libraries have them.
- **Bindings:** Rust's `Library::call(name, args)` takes the arguments as a
  JSON array (`Value::Array`), and `call_with` adds parts and a deadline;
  dropping a `Library` closes it, posted to the engine's thread when dropped
  elsewhere. Python's `call(..., function=f)` returns the call's number for
  `cancel()`; its Future's own `cancel()` cancels the call too. Host
  JavaScript has `callWith(name, args, {timeoutMs})` beside `call(name,
  ...args)`, and errors carry the code as `error.code` (`"TIMEOUT"`). Each
  binding refuses calls after close itself, so that a cancel never reaches a
  freed handle.
- **`js-bundle.mjs` is `tools/js-bundle.mjs`** and goes into the release
  archive beside zebkit-js. A package given as its argument is bundled as
  `export * from '<package>'`, which suits ES module packages; a CommonJS
  package needed a small entry (stage 3 makes one for it). `--empty <module>`
  takes an import (a Node.js built-in used only where present) as an empty
  module; without it each built-in import is named and the bundle fails.

Stage 3, 2026-11-13:

- **The code cache is made when an instance is first idle, or at close**,
  not after the load: V8 compiles functions lazily, and a cache made after
  the top level ran holds little of what the calls use (ECharts' first
  render compiles most of it). At close, too, so a program that loads, calls
  and quits leaves one. In memory as well as on disk, both behind
  `codeCache`, because a second load in the process (an instance, a library
  loaded again) gains the same.
- **The application's functions are an import, `zebkit:host`**, not a
  global; one callback type and two functions, `zebkit_library_set_host()`
  and `zebkit_library_answer()`, as `zebkit_call_resolve()` answers a page:
  set after load (the load's arguments are the released ABI), with requests
  made before it given once it is set. Their deadline is `timeoutMs`, the
  library's, rather than another option.
- **An instance that ends is not restarted; the library ends with its last.**
  Restarting would hide a library that hangs or leaks and costs a load each
  time; the application sees TIMEOUT or OUT_OF_MEMORY and loads it again if
  it wants. With one instance this is stage 1's rule.
- **Each binding's host functions are its own idiom**: Rust a closure or an
  async one (`Host`), Python a function, coroutine or Future (`host=`), Go a
  `func` run on its own goroutine (`LoadLibraryWithHost`), host JavaScript a
  function or async function (`{host: {...}}`).
- **`js-bundle.mjs` reads CommonJS exports with cjs-module-lexer** rather than
  running the package, and lists functions by importing the bundle in a
  Node.js process of its own (what `ready` will say). It writes
  `exports.json` beside the bundle, which `--list` and `--wrapper` read; a
  local package folder can be bundled as a package.

Stage 2, 2026-11-13: built; "Backend \"page\"" says how and what changed
from the design (the window without a view, the deadline told from the
runner's request, the heap told by the renderer, the address space, the
options refused). `docs/reports/library-bench.md` with both columns is
`scripts/library-bench.sh`'s.

## Security defaults

No files, no network, no `zebkit` object, no profile cookies or storage;
network only to origins given at load, refused in the browser; no user
gesture, popups, downloads or permissions in pages; limits on heap, queue,
size and time. A library is code the application ships, not a plug-in from
elsewhere.
