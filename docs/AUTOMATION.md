# Automation: Playwright's API without CDP

zebkit gives the application, and agents through it, the whole of
Playwright's API (pages, frames, locators, actions that wait until they can
be done, keyboard and mouse, network routing, dialogs, downloads, files,
tracing) as native functions of the engine, in the application's process.
There is no CDP, no WebSocket, no JSON-RPC between processes and no Node.js
driver in the path (owner, 2026-10-27). CDP stays as the endpoint for
clients that speak it (`docs/ARCHITECTURE.md`, "CDP"), and Playwright's own
clients, in any of its languages, connect to the engine's Playwright
endpoint (4.8) with `chromium.connect()`. The MCP server
(`docs/ROADMAP.md`, "Agents") is built on this layer, not on CDP.

This document says how Playwright works over CDP, what that costs (as
measured), how zebkit does the same natively, which Playwright command maps
to which part of the engine, and in what order it is built.

## 1. How Playwright drives Chromium

Measured on Playwright `1.65.0-next` (commit `a169686c`, 2026-10-27), the
source studied, with `playwright-core` 1.56 from npm for the timings.

### Three layers

```
your test / agent (TypeScript, Python, Java, .NET)
  client: Browser, Page, Frame, Locator, ElementHandle, Route, ...
        |  Playwright protocol: JSON messages, schema in packages/protocol/spec/*.yml
        v
Playwright server (Node.js; in-process for TypeScript, a driver process for the others)
  browser-neutral: frames.ts, dom.ts, page.ts, input.ts, network.ts, progress.ts
  auto-waiting, retries, strict mode, timeouts, lifecycle events
        |  the delegate interface: PageDelegate, ...
        v
  chromium/: crPage.ts, crInput.ts, crNetworkManager.ts, crExecutionContext.ts
        |  CDP over a pipe or a WebSocket, one JSON message per call
        v
browser process: content's DevTools handlers
        |  Mojo, to the renderer's DevTools agent
        v
renderer: V8 (Runtime.*), Blink (DOM.*, Input.*), and Playwright's
  "injected script" in an isolated world named __playwright_utility_world__
```

- **The client is thin.** A `Locator` is a selector string and nothing
  more: `page.getByRole('button', { name: 'Go' })` is the selector
  `internal:role=button[name="Go"i]` (`packages/isomorphic/locatorUtils.ts`),
  and `locator.click()` is the protocol command `Frame.click` with
  `{ selector, strict: true, timeout }`. Everything a command does happens
  in the server.
- **The protocol is the API.** `packages/protocol/spec/*.yml` describes
  every interface (`Frame`, `Page`, `ElementHandle`, `BrowserContext`,
  `Route`, ...), each command's parameters and results, and each event.
  It is the specification this layer implements.
- **The injected script does the DOM's part.** `packages/injected/src/injectedScript.ts`
  and what it imports are about 330 KB of JavaScript (bundled with esbuild).
  They hold every selector engine (CSS with Playwright's extensions, text,
  role with the accessible-name computation, test id, `nth`, `has`,
  frames), the checks of an element's state (visible, enabled, editable,
  stable, attached, inert), the hit-target check (is the point we will
  click on this element, or on what covers it), `fill`, `selectOptions`,
  `ariaSnapshot`, `expect` matchers, the highlight. The server instantiates
  it once per execution context with
  `new (module.exports.InjectedScript())(globalThis, options)`, and every
  DOM step is a `Runtime.callFunctionOn` on that object.

### One action, step by step

`locator.click()` (`dom.ts`, `_retryPointerAction` and
`_performPointerAction`), with the CDP messages it sent to Chromium 156,
counted with `DEBUG=pw:protocol`:

| Step | CDP |
|---|---|
| Resolve the selector, strictly (one element or an error) | `Runtime.callFunctionOn` x4, `Runtime.releaseObject` |
| Wait until visible, enabled, stable (same box over `stableRafCount` animation frames; 1 on Chromium) and not inert | `Runtime.callFunctionOn` |
| Scroll it into view if needed | `DOM.scrollIntoViewIfNeeded` |
| Where to click: the middle of its first content quad inside the viewport | `DOM.getContentQuads`, `Runtime.callFunctionOn` (viewport size) |
| Is that point on the element (not on an overlay); set up the interceptor that checks the events really reach it | `Runtime.callFunctionOn` x2 |
| Move, press, release | `Input.dispatchMouseEvent` x3, each answered once the renderer has handled it |
| Stop the interceptor, read what it saw | `Runtime.callFunctionOn`, `Runtime.releaseObject` x2 |
| Wait for a navigation the click started, if any | `Page.enable` (events) |

17 messages, each a round trip that waits for the one before. `fill` is 8
(`Input.insertText` is the text itself), `textContent` and `evaluate` 1,
`goto` 1 and then events.

### What it costs

Medians over 50 runs (20 for `goto` and `screenshot`), Playwright 1.56 in
Node 24, a local page, headless, the same machine:

| Operation | zebkit 155 over CDP | Chromium 156 (Playwright's) |
|---|---|---|
| `page.evaluate(() => 1 + 1)` | 0.97 ms | 0.94 ms |
| `locator.textContent()` | 2.28 ms | 1.85 ms |
| `locator.isVisible()` | 0.70 ms | 2.04 ms |
| `locator.count()` (200 elements) | 0.75 ms | 4.36 ms |
| `locator.allTextContents()` (200) | 2.28 ms | 9.45 ms |
| `locator.fill()` | 3.57 ms | 17.13 ms |
| `locator.press('Enter')` | 3.48 ms | 15.41 ms |
| `locator.click()` | 33.6 ms | 36.4 ms |
| `getByRole('button').click()` | 33.3 ms | 53.6 ms |
| `page.goto()` (local) | 92.2 ms | 43.0 ms |
| `page.screenshot()` | 50.2 ms | 68.1 ms |

And taking `click` apart (one run of each, same page):

| | zebkit | Chromium |
|---|---|---|
| `locator.click()` | 53.7 ms | 51.3 ms |
| `locator.click({ force: true })` (no stability wait, no hit-target check) | 32.6 ms | 31.1 ms |
| `page.mouse.click(x, y)` (three `Input` messages) | 4.0 ms | 3.9 ms |
| two `requestAnimationFrame`s, evaluated | 33.4 ms | 33.3 ms |
| a round trip (`evaluate(() => 1)`) | 0.96 ms | 0.81 ms |

What this says:

1. **A round trip is about 1 ms**: JSON in Node, the pipe or WebSocket, the
   DevTools handler, Mojo to the renderer, V8, and back. A click's 17 cost
   12-17 ms of waiting by themselves; a `fill`'s 8 about 5 ms.
2. **Frames cost more than the protocol.** A pointer action waits for
   animation frames: one to call the element stable, one more to compare,
   and the hit-target interceptor's events go through the next frame's
   input handling. At 60 Hz each frame is 16.7 ms, so a click cannot take
   less than about 33 ms however it is sent. This is Playwright's
   semantics (an element that moves is not clicked) and is kept; what is
   not kept is waiting for the display's clock where there is no display
   (section 4.6).
3. **The Node process is a second runtime** next to the engine's own V8,
   with its own heap, for every application that automates a page.
4. **`goto` is twice as slow on zebkit as on Chromium 156.** That is the
   engine's (navigation, not CDP) and is tracked as its own item.

## 2. What zebkit does instead

The same three parts, without the two wires:

```
application or agent (C, Rust, Python, JavaScript; the MCP server)
  bindings: Page, Frame, Locator, ... (Playwright's names)
        |  a function call: zebkit_automation_call(view, "Frame.click", params)
        v
browser process (libzebkit), the engine's thread
  automation.cc: Playwright's server, natively
    commands of Playwright's protocol, same parameters and results
    auto-waiting, retries, strict mode, timeouts (Progress)
    frames and lifecycle (WebContentsObserver), navigations (NavigationController)
    input: RenderWidgetHost::Forward*Event, acked by InputEventObserver
    screenshots, PDF, dialogs, file choosers, downloads, routes: zebkit's own parts
        |  Mojo: zebkit.mojom.AutomationAgent, one per frame (associated)
        v
renderer (zebkit-helper)
  automation_renderer.cc: the frame's agent
    Playwright's injected script in zebkit's isolated world
    one call does a whole step: resolve + check + scroll + point + hit target
    text committed through Blink's input method controller (fill, type)
```

### Decisions

- **Playwright's protocol is the API's specification.** Every command is
  named as in `packages/protocol/spec` (`Frame.click`, `Page.keyboardPress`,
  `ElementHandle.boundingBox`, ...) and takes and returns the same JSON.
  Playwright's documentation then documents zebkit's API, behaviour is
  compared with Playwright on Chrome command by command (the rule of
  following the reference implementation), and the endpoint (4.8)
  serves Playwright's own clients unchanged.
- **Playwright's injected script is reused, not rewritten.** It is the
  reference implementation of selectors, actionability and ARIA, and it is
  where most of Playwright's behaviour lives; a C++ rewrite would differ in
  a thousand details (text normalisation, the accessible-name algorithm,
  shadow DOM, `:has-text`). It is bundled from Playwright's source at a
  pinned commit (`upstream.lock`, `PLAYWRIGHT_COMMIT`) by
  `scripts/playwright-sources.sh`, Apache-2.0 with its notice, and runs
  in an isolated world of zebkit's own, which the page cannot see or
  change. What is native is everything around it: no CDP, the order of the
  steps, where they run, input, navigation and frames, and the waiting.
- **One renderer call per step, not per primitive.** Playwright's server
  sends primitives because it is browser-neutral and remote. The agent
  runs a step as one function in the renderer, so a click is two calls to
  the renderer (prepare; check what the interceptor saw) and three input
  events, instead of 17 messages.
- **Input is the browser's.** Mouse and keys go to the page's widget from
  the browser process, as from the system, through
  `RenderWidgetHost::ForwardMouseEvent`, `ForwardKeyboardEvent` and
  `ForwardWheelEvent`, the way `<zebkit/offscreen.h>` sends them and CDP's
  `Input.dispatch*Event` does. Each waits for the renderer's
  acknowledgement (`RenderWidgetHost::InputEventObserver`), as CDP's do.
  Events are trusted (`isTrusted`), go through Blink's hit testing,
  default actions and the browser's own routing.
- **Text goes in as an IME commit**, the same call CDP's
  `Input.insertText` ends in (`WebInputMethodController::CommitText`),
  made by the agent in the renderer in the same call as the injected
  script's `fill`: `beforeinput` and `input` are trusted, and a fill is one
  call.
- **In-process, asynchronous, the engine's thread.** Every command calls
  back on the engine's thread when done, as every zebkit callback does;
  many may be under way at once, on different views or the same.
  `zebkit_post_task()` reaches it from another thread. The parameters and
  results are JSON, once per command (not per step): small, in memory,
  microseconds.

## 3. The C API

`<zebkit/automation.h>`:

```c
typedef void (*zebkit_automation_func)(zebkit_view *view, const char *json,
                                       const char *error, void *data);
typedef void (*zebkit_automation_event_func)(zebkit_view *view, const char *event,
                                             const char *json, void *data);

/* Runs Playwright's command @method ("Frame.click", "Page.keyboardPress",
 * "ElementHandle.boundingBox", ...) on @view with @params, a JSON object as
 * Playwright's protocol has it, and calls @func once with its result as
 * JSON, or with @error, Playwright's message ("Timeout 30000ms
 * exceeded.\nCall log:\n  - waiting for #go", "strict mode violation:
 * ..."). A command acts on the main frame unless @params has "frame" (an
 * id from Page.frames or an event), on an ElementHandle or JSHandle when it
 * has "handle". */
void zebkit_automation_call(zebkit_view *view, const char *method,
                            const char *params, zebkit_automation_func func,
                            void *data);

/* The page's events as Playwright's protocol names them. */
void zebkit_automation_on_event(zebkit_view *view,
                                zebkit_automation_event_func func, void *data);

/* The commands this engine implements, as a JSON array of their names. */
char *zebkit_automation_commands(void);

/* Contexts and pages: zebkit_automation_call(NULL, "Browser.newContext",
 * {viewport, userAgent, ...}) answers {"context": "x1"}, a profile of its
 * own; "BrowserContext.newPage" ({"context": "x1"}) answers {"page":
 * "p2"}, an off-screen view under --headless, and zebkit_automation_page()
 * is that view. Any "BrowserContext.*" command with a "context" and no view
 * acts on the context: its profile, and its pages now and to come. */
zebkit_view *zebkit_automation_page(const char *page);
char *zebkit_automation_page_id(zebkit_view *view);

/* Playwright's own endpoint: Playwright's protocol over a WebSocket on
 * 127.0.0.1:@port (0 picks one), what chromium.connect(url) speaks. @func
 * gets the url, with its random token. */
void zebkit_automation_listen(int port, zebkit_automation_listen_func func,
                              void *data);
```

The C API is one entry point, so that a command added to the engine is in
every binding at once. Where zebkit differs from the protocol, and why:

- **Values are JSON.** An evaluation's `arg` and `value` are plain JSON
  (`undefined` is `null`, `NaN` and `Infinity` strings, a node its
  preview); a handle is `{"handle": "h3", "preview": "<button>", "element":
  true}`, and an argument refers to one the same way. Playwright's
  serialized values (`{v: "undefined"}`, `{d: date}`) are what the agent
  speaks underneath, and what a command takes with `"serialized": true`
  (`arg` serialized, `argHandles` the handles it refers to): the endpoint
  (4.8) passes Playwright's values through so.
- **Bytes are base64** in JSON (`Page.screenshot`'s `binary`, `Page.pdf`,
  `Response.body`, `Route.fulfill`'s `body` with `isBase64`), and
  `Page.screenshot` also takes `path`, a file the engine writes.
- **Objects are ids, not guids**: frames `f<n>`, handles `h<n>`, requests
  `q<n>`, routes `r<n>`, dialogs `d<n>`, file choosers `c<n>`, downloads
  `w<n>`, binding calls `b<n>`, all in the events that create them.

**Bindings**: Python's is `zebkit.automation` (`bindings/python`),
Playwright's async API with its names: `Page`, `Frame`, `Locator` (with
`get_by_role`, `filter`, `nth`, `and_`, `or_`, `frame_locator`, built as
`locatorUtils.ts` builds selectors), `ElementHandle`, `JSHandle`, `Route`,
`Request`, `Response`, `Dialog`, `FileChooser`, `Download`, `Clock`,
`page.request`, and `expect` (`to_have_text`, `to_have_count`,
`to_match_aria_snapshot`, ...), `new_context()` and `BrowserContext`,
`Worker`, `page.tracing` (traces and HAR), `page.start_video()`,
`page.touchscreen`, and `listen()` for the endpoint. Coroutines run on the
engine's thread with `zebkit.automation.run()`; an `await` is a command in
flight. Rust's is `zebkit::automation` (`Page`, `Frame`, `Locator`,
`BrowserContext`, `listen`, and `call` for any command).

## 4. The parts

### 4.1 The renderer's agent

`zebkit.mojom.AutomationAgent` (`automation.mojom`), an associated interface
of each `RenderFrame`, as `MediaCaptureAgent` is
(`automation_renderer.cc`):

- **Two runners per document**, one in the page's world and one in zebkit's
  isolated world (`ISOLATED_WORLD_ID_CONTENT_END + 1`), each the value of
  `automation_scripts.h`'s script: Playwright's injected script made with
  the options Playwright's server gives Chromium, its serializers, its
  ARIA renderer and parser, and zebkit's steps (`automation_steps.js`).
  The isolated world's globals are its own: the page cannot see or change
  it. The page's world has one for what Playwright runs there
  (`evaluate`, `dispatchEvent`, `evalOnSelector`), kept by the agent as a
  `v8::Global`, never on `window`.
- **Called from V8 directly**, as DevTools' `Runtime.callFunctionOn` is:
  `WebLocalFrame::CallFunctionEvenIfScriptDisabled` on the runner's
  method, a promise awaited with `v8::Promise::Then`, an answer of `[json,
  handles, previews]`. A document that goes (`WillReleaseScriptContext`)
  answers its calls with `kGone`, and a selector command tries again in the
  next document.
- **Expressions are compiled by the agent**, not by `eval()`, which a
  page's Content Security Policy may forbid: a function's source as an
  expression, else a script whose value is its last statement's.
- **Handles** are the agent's: objects a command keeps (an element, a
  `JSHandle`'s value, the hit-target interceptor) by number, in their world,
  until disposed or their document goes. A node passes between the worlds
  as the same node (`WebElement::FromV8Value`, `ToV8Value`), as Playwright
  adopts an `ElementHandle`; another object cannot.
- **Text** is committed as an IME commits it
  (`WebInputMethodController::CommitText`, where CDP's `Input.insertText`
  ends), in the same call as the injected script's `fill`.
- **Files** are given to an `<input type=file>` as a person choosing them
  would, with `WebInputElement::SetFilesFromPaths` (`patches/1036`, the
  call DevTools' `DOM.setFileInputFiles` makes).
- **Exposed functions** (`exposeBinding`) are V8 functions the agent
  defines on each new document's global before its own script, which call
  the browser's `AutomationHost` and resolve with its answer. Their arguments
  go as JSON and as Playwright's serialized values (`bindingsController.ts`:
  cycles, windows and nodes), which the endpoint gives its client; the
  client's answer comes back serialized, with its handles, and a rejection
  is the client's error with its stack, or the value it threw.

### 4.2 Commands and auto-waiting

`automation.cc` is Playwright's server's logic for each command (`frames.ts`,
`dom.ts`, `page.ts`, `input.ts`) around the agent's calls. What Playwright
does in its server between round trips, zebkit does in the renderer in one
step: a step polls with Playwright's waits between tries (0, 20, 50, 100,
100, 500 ms; 0, 20, 100, 100, 500 for actions) until the call's timeout, and
answers with its call log. A click is two renderer calls and three input
events:

| Step | Where | What |
|---|---|---|
| `pointerPrepare` | renderer, one call | resolve strictly; `checkElementStates(visible, enabled, stable, non-inert)`; scroll into view (alignments cycled on retries); the middle of the first box in the viewport, or `position`; `setupHitTargetInterceptor` |
| input | browser | move, press, release to the frame's widget, each acknowledged |
| `pointerFinish` | renderer, one call | the interceptor's verdict; another try if something else got the events |
| navigation | browser | a navigation the input started waited for until it commits, unless `noWaitAfter` |

Errors and timeouts are Playwright's: an action that times out fails with
`Timeout 30000ms exceeded.` and the call log the steps wrote; a selector
that matches several elements in a strict command fails at once with the
injected script's `strict mode violation`.

### 4.3 Frames, navigation, lifecycle

- **Frames** are their frame tree nodes (`f<n>`): `Page.frames`, the events
  `Page.frameAttached`, `Page.frameDetached`, `Frame.navigated`,
  `Frame.loadstate` (`domcontentloaded`, `load`, `networkidle`).
- **Frame locators** (`iframe >> internal:control=enter-frame >> button`)
  are resolved as `frameSelectors.ts` resolves them: the `<iframe>` waited
  for in its frame, its child frame found (`ContentFrame`: the child's
  index in the frame tree), the rest of the selector there.
- **Points in frames**: a point in a child frame is moved into its parent's
  coordinates by where the `<iframe>`'s content box is there (the
  `childFrameBox` step), up to the frame's local root. A frame of another
  site is its own widget, which gets the event in its own coordinates.
- **Navigation**: `goto`, `reload`, `goBack`, `goForward` through the
  `NavigationController`, followed with `WebContentsObserver` until the
  navigation commits, then until `waitUntil` (`commit`,
  `domcontentloaded`, `load`, `networkidle`); the answer is the main
  resource's response (url, status, headers). A failed load is Playwright's
  `net::ERR_CONNECTION_REFUSED at http://...`.

### 4.4 Input

From the browser to the page's widgets, as from the system:
`RenderWidgetHost::ForwardMouseEvent`, `ForwardKeyboardEvent`,
`ForwardWheelEvent`, each answered when the renderer acknowledges it
(`InputEventObserver::OnInputEventAck`), as CDP's `Input.dispatch*Event`
answers. Events are trusted and go through Blink's hit testing and default
actions.

A new top-level widget's input router takes nothing until its first frame is
activated, or Chromium's new-content timeout passes: until then the browser answers
each event itself with `kNoConsumerExists`, and the page hears nothing, as a
person cannot act on a page not yet shown. A native command is fast enough to
get there (one in five when navigating a view again and again); commands wait for the signal
(`patches/1062`, what Chromium's own browser tests use), and answer with an error if
input was dropped anyway. `tests/engine/automation_input.py` holds both.

- **Keyboard**: Playwright's US layout (`usKeyboardLayout.ts`, generated into
  `playwright/us_keyboard_layout.h` with the names `buildLayoutClosure()`
  gives its keys), modifiers held across commands, key events made as CDP's
  `Input.dispatchKeyEvent` makes them, to the focused frame's widget.
- **Mouse**: position, buttons and modifiers kept as `input.ts` keeps them;
  `move` with `steps`, `click` with `clickCount` and `delay`, `wheel`.
- **Touch**: `tap` as a touchscreen's: a touch started and ended at the
  point (`patches/1038`'s `ForwardTouchEvent`, as CDP's
  `Input.dispatchTouchEvent`), the page's `touchstart` and `touchend`, and
  the browser's gesture recognizer makes the tap and its click. A page's
  touch events are turned on first (`hasTouch`).
- **Frames of other sites**: a mouse event goes to the widget under its
  point (`patches/1038`'s `FindWidgetAtPoint`, the browser's own hit test
  for input), in that widget's coordinates, so `Page.mouse` reaches an
  out-of-process iframe as a person's mouse does.

### 4.5 Network

`automation_network.cc`: for a page with automation, each URL loader
factory its frames make (`ContentBrowserClient::WillCreateURLLoaderFactory`,
navigation and subresources) goes through a factory of zebkit's, as
extensions' webRequest and DevTools' Fetch domain put one:

- **Events**: `Page.request` (url, method, headers, resource type, frame,
  navigation, redirected from), `Page.response` (status, headers, MIME
  type, server address), `Page.requestFinished`, `Page.requestFailed`; a
  redirect is a request of its own, as Playwright reports it.
- **Routes**: a request whose URL matches a pattern
  (`Page.setNetworkInterceptionPatterns`, globs as `urlMatch.ts`'s
  `globToRegexPattern()`, regular expressions with RE2) is held and
  reported (`BrowserContext.route`) until `Route.continue` (with a changed
  URL, method, headers, body), `Route.fulfill` (status, headers, body) or
  `Route.abort` (Playwright's error codes).
- **Bodies**: a response's body is copied as it streams to the page and kept
  for `Response.body` (up to 10 MiB, the last 200 responses).
- **Extra headers** and **offline** (`BrowserContext.setOffline`: requests
  fail with `net::ERR_INTERNET_DISCONNECTED`).

- **WebSocket routes** (`Page.setWebSocketInterceptionPatterns`): Playwright's
  own mock (`injected/src/webSocketMock.ts`, `playwright/websocket_mock_script.js`)
  in every frame, reporting through a binding of the agent's;
  `Page.webSocketRoute`, `WebSocketRoute.connect`, `ensureOpened`,
  `sendToPage`, `sendToServer`, `closePage`, `closeServer`, and the
  `WebSocketRoute.messageFromPage`, `messageFromServer`, `closePage`,
  `closeServer` events, as `server/webSocketRoute.ts` does.

A page's first command makes its frames' subresource factories again
(`patches/1038`'s `UpdateSubresourceLoaderFactories`, as DevTools' Fetch
domain does when it starts), so the document already loaded is proxied
too, not only the ones after it.

### 4.6 Frames on demand

Where a view has no display (`--headless`, an off-screen view), nothing
needs frames at the display's 60 Hz. While a command is in flight on such a
page, its compositor's begin frames come every 2 ms instead of every
16.7 ms (`ui::Compositor::SetDisplayVSyncParameters`), and go back to the
display's rate when the last command answers; so a frame Playwright's
stability check or an input's delivery waits for costs what it takes to
make. Windowed views keep their display's rate. Section 6 has what it
saves.

### 4.7 The rest

| Playwright | zebkit |
|---|---|
| `Dialog`, `page.on('dialog')` | `Page.updateSubscription {event: "dialog"}` holds the page's dialogs (`dialog.cc`'s `DialogObserver`); `BrowserContext.dialog`, `Dialog.accept`, `Dialog.dismiss` |
| `FileChooser`, `page.on('filechooser')` | `patches/1035` gives the embedder the page's file choosers: `Page.fileChooser`, `FileChooser.setFiles` answers as the person's choice |
| `Download` | saved into a directory of the engine's without asking (`acceptDownloads`); `Page.download`, `Artifact.pathAfterFinished`, `saveAs`, `failure`, `cancel`, `delete` |
| `exposeBinding`, `exposeFunction` | 4.1; `BrowserContext.bindingCall`, `BindingCall.resolve`, `reject` |
| cookies, `storageState` | the profile's cookie manager (`automation_context.cc`); the local storage of the origins the page shows |
| `APIRequestContext.fetch` | `SimpleURLLoader` through the profile's network, its cookies. `maxRedirects`, 0 for the redirect itself as the answer; `Authorization`, `Proxy-Authorization` and `Cookie` a caller gave stay behind at another origin; `maxBodySize`, up to 256 MiB, zebkit's own |
| `grantPermissions`, `clearPermissions` | `patches/1037` lets the embedder answer for a permission at an origin; granted ones granted, the others denied, as CDP's `Browser.grantPermissions` |
| `clock` | Playwright's clock (`injected/src/clock.ts`, `playwright/clock_script.js`) installed and driven as `server/clock.ts` does |
| `emulateMedia` | the page's preferences: color scheme, reduced motion, contrast, forced colors |
| `screenshot` | the view's surface (`RenderWidgetHostView::CopyFromSurface`), the viewport, a clip or an element's box |
| `pdf` | `print.cc`, as `Page.printToPDF` prints |
| `addInitScript` | `zebkit_view_add_script` |
| `ariaSnapshot`, `expect` | the injected script's, YAML by Playwright's renderer and parser |
| `newContext`, `newPage` | a profile of its own (`zebkit_context_new`), with the context's options (viewport, userAgent, locale and timezone, geolocation, permissions, httpCredentials, offline, extraHTTPHeaders, colorScheme, hasTouch, javaScriptEnabled, storageState); its pages are off-screen views under `--headless` |
| `setGeolocation` | `patches/1038`'s `SetGeolocationOverride`, the position the page's Geolocation API reads, as `Emulation.setGeolocationOverride` |
| `setHTTPCredentials` | the context's login delegate (`ContentBrowserClient::CreateLoginDelegate`) answers HTTP authentication with them, once per request, as Playwright's |
| `emulateMedia({media})` | `patches/1039`'s `blink::SetMediaTypeOverride`, the media type CSS sees (`print`, `screen`), as `Emulation.setEmulatedMedia` |
| `Worker`, `page.on('worker')` | the agent on each dedicated worker's thread (`ContentRendererClient::DidInitializeWorkerContextOnWorkerThread`); `Page.worker`, `Worker.close`, `Worker.evaluateExpression` |
| `tracing` | `automation_trace.cc`: Playwright's trace format (version 10: `context-options`, `before`/`after` of each command with its log, `screencast-frame` JPEGs, `console`, `resource-snapshot`), the zip its trace viewer opens; `Tracing.tracingStart`, `tracingStartChunk`, `tracingStopChunk`, `tracingStop`, `tracingGroup` |
| HAR (`recordHar`, `routeFromHAR`'s recording) | HAR 1.2 from the network events, bodies embedded, omitted or attached; `Tracing.harStart`, `harExport` |
| `recordVideo` | the view's surface 25 times a second, VP8 (libvpx) in WebM (the engine's FFmpeg); `Page.startVideo`, `Page.stopVideo` |

### 4.8 Playwright's own endpoint

`automation_server.cc`, `zebkit_automation_listen()`: Playwright's protocol
over a WebSocket (`net::HttpServer` on a thread of its own), so that
Playwright's clients, in each of its languages and unchanged, drive zebkit
with `chromium.connect(url)`, and Playwright's test runner with
`connectOptions`. It is what Playwright's dispatchers
(`server/dispatchers`) are, on the commands above:

- **Objects**: `initialize` answers the `Playwright` object, whose
  `preLaunchedBrowser` is the engine; each object the protocol has (`Browser`,
  `BrowserContext` with its `Tracing` and `APIRequestContext`, `Page`,
  `Frame`, `ElementHandle`, `JSHandle`, `Request`, `Response`, `Route`,
  `Dialog`, `BindingCall`, `Artifact`, `Stream`, `Worker`,
  `WebSocketRoute`) is created (`__create__`) with its initializer as the
  protocol's spec has it, before the event that mentions it, and disposed
  (`__dispose__`) as the dispatchers do; a page's main frame is created
  before the page and adopted into it.
- **Commands** go to the native command of the same name with the object's
  id; **events** of the page become the protocol's, on the objects they
  belong to; **values** stay serialized end to end (`"serialized": true`).
- **Files** (traces, HAR, video, downloads) reach a remote client as the
  protocol's streams (`Artifact.saveAsStream`, `Stream.read`).
- A client's contexts close when it disconnects, as with
  `launchServer()`.
- **`navigator.webdriver`** is true on the pages of this endpoint, as under
  Playwright's `--enable-automation`. A host's own view driven through
  `<zebkit/automation.h>` says nothing of the kind, and has no DevTools session
  until a command needs one.
- **Who connects**: it listens on 127.0.0.1 only, on
  `ws://127.0.0.1:<port>/<random token>`, the url the host gets from
  `zebkit_automation_listen()`, new at each start, as `launchServer()`'s
  `wsPath`. Another path gets 403, and so does any request with an `Origin`
  header, as Chromium's DevTools handler refuses origins without
  `--remote-allow-origins`: a web page's WebSocket always has one. The url is
  the credential (a client writes files, with `Page.screenshot`'s `path`): the
  host gives it only to what should drive the engine. CDP's own endpoint
  (`--remote-debugging-port`) is Chromium's handler, unchanged: it refuses
  origins the same way, `--remote-allow-origins` lets the named ones in, and
  it has no token, as in Chrome.

`tools/zebkit-run --playwright-port=0` serves it;
`tests/engine/playwright_connect.mjs` is Playwright's client against it.

## 5. What is built, and what is not

**Built**: the commands `zebkit_automation_commands()` lists, and Playwright's
protocol over a WebSocket (4.8). This names what exists, not how closely it
behaves as Playwright's server does: section 7 has that, measured by
Playwright's own tests, and `tests/playwright-suite/expected-failures.txt`
what is known to differ, each with its reason.

- `Frame`: evaluateExpression(Handle), evalOnSelector(All),
  querySelector(All), queryCount, textContent, innerText, innerHTML,
  inputValue, getAttribute, is{Checked,Disabled,Enabled,Editable,Visible,
  Hidden}, waitForSelector, waitForFunction, waitForTimeout,
  waitForLoadState, title, content, setContent, addScriptTag,
  addStyleTag, ariaSnapshot(JSON), expect, click, dblclick, hover, tap,
  check, uncheck, dragAndDrop, fill, focus, blur, press, type,
  selectOption, setInputFiles, dispatchEvent, goto.
- `ElementHandle`: the same on its element, and boundingBox, contentFrame,
  ownerFrame, screenshot, scrollIntoViewIfNeeded, selectText, setChecked,
  waitForElementState; `JSHandle`: evaluate, getProperty(List), jsonValue,
  dispose.
- `Page`: reload, goBack, goForward, frames, keyboard{Down,Up,Press,Type,
  InsertText}, mouse{Move,Down,Up,Click,Wheel} (into frames of other sites
  too), touchscreenTap (touch events), screenshot, setViewportSize,
  addInitScript, pdf, close, bringToFront, emulateMedia (print media too),
  exposeBinding, setNetworkInterceptionPatterns,
  setWebSocketInterceptionPatterns, setExtraHTTPHeaders,
  updateSubscription, workers, startVideo, stopVideo.
- `Browser.newContext`; `BrowserContext`: newPage, pages, close, cookies,
  addCookies, clearCookies, storageState, setStorageState,
  grantPermissions, clearPermissions, setGeolocation, setHTTPCredentials,
  setOffline, exposeBinding, setNetworkInterceptionPatterns,
  setWebSocketInterceptionPatterns, setExtraHTTPHeaders, clock*;
  `Tracing` (traces, HAR); `Route`, `Response.body`, `Dialog`,
  `FileChooser`, `BindingCall`, `Artifact`, `Worker.evaluateExpression`,
  `WebSocketRoute`, `APIRequestContext.fetch`.

**What it does not do**, on purpose or as Playwright on Chromium does not
either:

- **Firefox and WebKit**: the endpoint's `firefox` and `webkit` are there
  because the protocol's `Playwright` object has them; they launch nothing.
  `launch()`, `launchServer()` and `connectOverCDP()` answer that this
  engine is connected to, not launched.
- **Service workers' own evaluation** (`ServiceWorker` objects): Playwright
  gives them only on Chromium over CDP's targets; the agent runs in
  dedicated workers.
- **The recorder, the inspector and `page.pause()`**: Playwright's tools,
  not the browser's; they need Playwright's own server.
- **Request measurements** now use Chromium load timing for the start,
  DNS, connect, TLS, send, receive and completion phases. A phase unavailable
  for a reused connection or a synthetic response remains -1. Encoded body
  sizes come from network completion statistics; header sizes follow the
  observed HTTP header representation, rather than HTTP/2 or HTTP/3 framing.
  Upload streams without a known length report -1 for their body size.
- **Protocol commands not implemented yet**, which fail with "no command" or
  are listed as expected failures: locator handlers, `Frame.drop`, custom
  selector engines, JS and CSS coverage, the context's `proxy` and
  `clientCertificates` options,
  `Route.redirectNavigationRequest`, WebMCP; a service worker's own requests
  (not through the network proxy) and its console messages.

**Console messages** come as `crPage.ts` has them: `Runtime.consoleAPICalled`
and `Log.entryAdded` of the page's DevTools session (a frame of another
process's from `WebContentsObserver`), the text from the arguments' previews
(`renderPreview()`), the location from the stack's top frame, and the arguments
as handles: `Runtime.callFunctionOn` leaves them on the global object for a
moment and the agent takes them from there. Messages, and the page errors
after them, keep their order; an evaluation answers after the page's DevTools
session has told what came before it, and a navigation after the messages
already told, as CDP's events come before its answers. A dedicated worker's
messages are its page's too, and its uncaught exception is the page's error.

**`CDPSession`** (`Browser.newBrowserCDPSession`, `BrowserContext.newCDPSession`
of a page, or of a frame of another site, which is a target of its own) is a
session of Chromium's DevTools in the engine's process, the browser's target or
the page's or frame's, attached as a trusted client as Playwright's pipe is:
`send` answers with CDP's result or `Protocol error (method): message`, its
events come as the session's `event`, and `detach` or the target's end closes it.

**Popups** are reported as `crPage.ts` reports them: once their first document
other than the initial empty one has committed (`Page.waitForInitialNavigation`),
so `page.url()` is the document's; with an opener only when they have one (a
page Chromium opened for a modified click has none). What they do meanwhile is
kept and given after; a route, a dialog, a binding's call, a file chooser or a
download reports the popup at once, since the client must answer it.
A popup opened in its opener's process is read by the opener in the same task
(`window.open()` returns its window), so it is held there as Chromium's DevTools
holds it for Playwright's browser session (`Target.setAutoAttach`, which makes
`CreateNewWindow` answer "wait for the debugger"): from an automated page's
allowed `window.open()` until the popup has its view, a browser session of the
engine's own auto-attaches to nothing, and the popup's view releases its page
target (`Runtime.runIfWaitingForDebugger`) after the context's init scripts,
viewport, user agent and offline state are in. The context's exposed functions
are there from its first script because the renderer's agent of the popup takes
them from its opener's.

**`page.frameLocator()` with no selector** (`internal:control=any-frame`) is
resolved as `frameSelectors.ts` resolves it: each frame of the starting frame's
subtree is asked how many elements the selector's first part has; one frame
with some is where the rest continues, more is "frameLocator() matched elements
in multiple frames"; none yet keeps waiting, except a query (count, `$$`) and an
assertion, which looks again after 250 ms until it passes or times out. Not yet:
a scope handle restricting the child frames, and hidden iframes.

**Network details from DevTools.** Each page's own DevTools session has its
`Network` domain on, for what only Blink and the network service's observer know:
the request's type (`xhr` or `fetch`), the headers as sent and received
(`Network.requestWillBeSentExtraInfo`, `responseReceivedExtraInfo`: `Cookie`,
`Set-Cookie`, for `allHeaders()` and HAR), each redirect hop's in order,
WebSockets as `WebSocket` objects with their frames, and offline as
`Network.emulateNetworkConditions` (`navigator.onLine`); routes answer before the
offline check, as Fetch's interception does.

**Service workers** are the context's `Worker` objects while they run (the
profile's `ServiceWorkerContext`), evaluated through the worker's DevTools target;
a page's request that its service worker answers, which the network never sees,
is reported from the page's DevTools `Network` events with `fromServiceWorker`
and its body, or as failed.

**Storage state** with `indexedDB` is collected as `browserContext.ts` does, with
Playwright's storage script in the pages that show each origin and, for an origin
given to the context that none shows, in a page of the origin whose requests are
all answered with an empty document; `newContext({storageState})` and
`setStorageState()` restore IndexedDB the same way. OPFS is not collected yet.

**Routes**: a fulfilled cross-origin response gets the CORS check the network
service would make (`network::cors::CheckAccess`) and, as `network.ts`, allows the
request's origin unless it says otherwise. Route globs resolve against the
context's `baseURL` (`ws:` for a WebSocket route of an `http:` base). WebSocket
routes get their page's and server's messages and closes, pass-through included.

**Frames**: a selector through frames works from an element handle; queries do
not wait for an `<iframe>`, assertions look for the frames again every 250 ms as
`frames.ts` polls, and elements in a hidden `<iframe>` are hidden. A main frame's
`load` is its load event's: none after `window.stop()`.

**Traces** of a remote client are recorded by the endpoint as
`trace/recorder/tracing.ts` records them: before and after events of each call
with its title and parameters, DOM snapshots before and after an action
(`snapshotterInjected.ts` run in each frame), screencast frames, console and
page events, the network as HAR resources, chunks and groups, `context.request`
traced apart; a zip of files only, as Playwright's archives.

**HAR** is recorded by the endpoint for a context, or a page, as
`har/harTracer.ts` does in Playwright's server, from the network events of all
its pages and its `APIRequestContext`'s fetches: pages with titles and timings,
cookies, query strings, post data, redirects, sizes and timings, server address
and port, TLS details, `route.continue()`'s overrides, contents embedded,
omitted or attached (a zip with `har.har` beside the contents), URL filters,
`minimal` mode. The native `Tracing.harStart` of a page uses the same recorder.

**The engine's switches**: `zebkit-run --playwright-port` gives the engine the
switches Playwright launches Chromium with (no back-forward cache, no popup
blocker, no third-party storage partitioning, a desktop pointer and no
scrollbars when headless), and third-party cookies sent as Chrome sends them
(zebkit's default blocks them); a host serving the endpoint for Playwright's clients
should do the same.

Native `Page.addInitScript`, `Page.exposeBinding` and their context versions
return `{registration, kind}`. Pass the registration token to
`Page.removeRegistration` or `BrowserContext.removeRegistration` (with the
owning context ID) to revoke that registration. Removing an already absent
token succeeds without changing other registrations. Context binding names
are checked across current pages before the replay queue is changed.

Initialization-script and exposed-binding registrations returned by the
Playwright protocol have real disposal. Disposing a script removes its
Chromium new-document registration; effects already executed in a document
remain. Disposing a binding removes it from live frames and future documents,
and saved references reject, including after same-name registration. Context
disposal also removes the replay entry used for future pages. Linux client
regressions cover these cases, compressed-response timings and sizes, POST
uploads, navigation response identity and response-body reads.

## 6. What it costs now

`scripts/automation-bench.sh` (`docs/reports/automation-bench.md`), the
page of section 1, 50 runs each (20 of `goto` and `screenshot`), headless,
on one machine, 2026-10-27. Medians; the endpoint is Playwright's own client
(playwright-core 1.64.1) connected to `zebkit_automation_listen()`, with no
CDP in between:

| Operation | zebkit native | zebkit endpoint, Playwright's client | zebkit, Playwright over CDP | Chrome 156, Playwright over CDP | native vs over CDP |
|---|---|---|---|---|---|
| `evaluate(() => 1 + 1)` | 0.12 ms | 0.59 ms | 0.60 ms | 0.45 ms | 5.0x |
| `locator.textContent()` | 0.22 ms | 0.65 ms | 1.10 ms | 0.90 ms | 5.0x |
| `locator.isVisible()` | 0.20 ms | 0.69 ms | 0.74 ms | 0.66 ms | 3.7x |
| `locator.count()` (200) | 0.24 ms | 0.77 ms | 0.78 ms | 0.76 ms | 3.3x |
| `allTextContents()` (200) | 0.26 ms | 2.05 ms | 2.21 ms | 1.81 ms | 8.5x |
| `locator.fill()` | 0.66 ms | 2.42 ms | 3.22 ms | 2.74 ms | 4.9x |
| `locator.press('Enter')` | 0.40 ms | 0.82 ms | 3.18 ms | 2.34 ms | 8.0x |
| `locator.click()` | 4.91 ms | 3.46 ms | 33.39 ms | 33.39 ms | 6.8x |
| `getByRole().click()` | 3.19 ms | 3.46 ms | 33.32 ms | 33.35 ms | 10.4x |
| `page.screenshot()` | 29.91 ms | 44.15 ms | 66.68 ms | 33.39 ms | 2.2x |
| `page.goto()` (local), 2026-11-02 | 13.46 ms | 13.88 ms | 15.77 ms | 18.31 ms | 1.2x |

- **A click no longer waits for the display**: frames on demand (4.6) make
  the frames Playwright's stability check and the input's delivery wait for
  at once, so a click takes 3 to 5 ms instead of the display's two frames
  (33 ms at 60 Hz, which Playwright over CDP still pays on zebkit and on
  Chrome alike). A click through the endpoint gets the same.
- **Everything else that is not a navigation is 3.3 to 8.5 times faster**
  natively: one renderer call where Playwright makes several round trips,
  no JSON between processes.
- **The endpoint sits between the two**: Playwright's client still runs in
  Node.js and speaks its protocol over a WebSocket, but the server's work is
  native; for commands that are round trips (press, textContent, fill) it
  is 1.3 to 3.9 times faster than Playwright over CDP, and the same for
  single-round-trip ones (evaluate, count). A screenshot through the
  endpoint costs a base64 PNG in JSON over the socket (44 ms against 30 ms
  natively).
- **`goto` answers at the load event, as Chromium's lifecycle does**: it
  waited for `DidFinishLoad` until 2026-11-02, which Blink sends only after
  it has laid the page out once more after the load event (`Document::
  CheckCompleted`), 6 ms later on this page; Playwright over CDP takes
  `load` from the lifecycle event fired at the load event itself. So
  native was 6% slower (16.2 against 15.2 ms, 600 runs of each, taking
  turns), and `--check`'s 20-run medians put the gap anywhere from 1% to
  13%. The main frame's load is now `DocumentOnLoadCompleted`, sent at the
  load event; a frame in a frame still loads at `DidFinishLoad`. 600 runs
  after: 13.3 against 15.3 ms. A page with nothing to lay out was never
  slower natively (5.4 against 8.3 ms).
- **No second runtime** natively: Playwright's Node.js process alone reached
  175 MiB and used 1.1 s of CPU for ten runs of the table.

## 7. How it is checked

- **Against Playwright on Chrome**: every command in
  `tests/engine/automation.c` has its expected result from running the
  same command through Playwright on Chrome (the reference);
  `tests/engine/playwright_connect.mjs` runs Playwright's own client
  against the endpoint (`scripts/engine-test.sh`).
- **Real CDP clients, Chrome as the reference**: `tests/engine/cdp_clients.mjs`
  (test `cdp-clients`) runs puppeteer-core and playwright-core's
  `connectOverCDP` against zebkit and Chrome in the same run, with no result
  hard-coded, and prints the differences; those expected have their reasons
  in `tests/engine/cdp_clients_expected.txt`. On 2026-11-01, engine commit
  b2c1ee2 against Chrome 156, 137 cases: 134 the same, 3 expected differences
  (two fields newer than Chromium 155; number formats of a locale the
  package's ICU data does not hold), none unexpected.
- **Playwright's own test suite**: `scripts/playwright-suite.sh` runs
  `tests/page` and `tests/library` at tag v1.64.1 through `chromium.connect()`,
  an engine for each worker, or the same tests on Chrome (`--target chrome`).
  What a test expects to fail is named with its reason in
  `tests/playwright-suite/expected-failures.txt`, by the tests that have the cause; a timeout
  or a crash is never explained by the list and is counted apart. 5262 tests, 30 s a test, four
  workers:

  | | Passed | Failed | Skipped | Listed | Timeout | Crash | Unexplained |
  |---|---|---|---|---|---|---|---|
  | zebkit, 2026-11-01 | 3028 | 1167 | 1067 | 580 | 197 | 8 | 587 |
  | zebkit, 2026-11-02 | 3233 | 962 | 1067 | 574 | 121 | 9 | 388 |
  | zebkit, 2026-11-03 | 3566 | 512 | 1184 | 234 | 69 | 8 | 209 |
  | Chrome (launched, 2026-11-01) | 4196 | 5 | 1061 | | | | |

  What moved it (2026-11-02): the engine started with Playwright's launch switches by
  `zebkit-run` (popups not blocked, no back-forward cache, no third-party partitioning, a desktop
  pointer); the injected script of v1.64.1 instead of a later commit; `CDPSession`; popups reported
  after their first commit; AbortSignal; Playwright's call logs; HAR recorded by the endpoint;
  CDP's cookies; storage state from the profile; request-based `networkidle`; Response bodies of
  fulfilled and in-flight responses; goto's referer. One engine aborted during the run (it was
  restarted, as `engines.mjs` does, at the cost of its worker's test).

  What moved it (2026-11-03): tests that launch a browser get `zebkit-run` (wrappers in the
  run's browsers folder), so `browserType.launch()`, `connectOverCDP` and Playwright's codegen
  run against the engine; in-process popups held as Chromium's DevTools holds them; `frameLocator()`
  without a selector; frames entered from handles, through hidden iframes, without waiting for
  queries; the page's DevTools `Network` events (XHR/fetch, raw headers per redirect hop, Cookie,
  Set-Cookie, WebSockets, offline); page errors from `Runtime.exceptionThrown`; service workers as
  `Worker`s and the requests they answer; CORS on fulfilled responses; WebSocket routes' events;
  globs under `baseURL`; IndexedDB in storage state; traces recorded by the endpoint as
  `tracing.ts` does; screenshots as `screenshotter.ts` and `crPage.ts` take them; no engine
  aborted (the form-params parser that threw out of range is gone). The skipped tests rose by
  117 because tests that launched nothing before now launch and skip what they skip on Chrome.

  **Not at parity: what is left, by cause** (the unexplained 209 and the 69 timeouts):
  - HAR (12): the popup's extra entry, connection and security details as Playwright reads them,
    entries served from the cache, API requests' redirects, `startHar` of an APIRequestContext.
  - Third-party cookies (9): partitioned cookies and third-party saving and loading.
  - `dispatchEvent` of device, drag and drop events (8); screenshots' pixels (8: WebGL, masks,
    a page navigating, fonts' wait message); routes (7: `sec-fetch-*`, Cookie and Set-Cookie as
    a route sees them, URLPattern options, the disk cache); `exposeFunction` with handles and
    cycles (6); `beforeunload` (6); clock (6); `page.close()` interrupting calls (6).
  - Timeouts: a service worker's own requests (12 in `chromium.spec.ts`), `Browser.startTracing`
    (6), the debugger (6), extensions (5), workers' console (5), mobile viewport (4), the trace
    viewer that launches a browser (4).
  Since that run, on the files concerned (counted at the next full run): third-party cookies
  (`zebkit-run` sends them, as Chrome does), console messages from CDP with their arguments,
  exposed functions' serialized arguments and answers, `beforeunload` and `page.close()`
  (pending calls and `request.response()` end with `TargetClosedError`), `dialogclosed` for
  dialogs dismissed without a listener, a worker's uncaught exception as a page error.
  The endpoint is not at parity with Playwright's server, and this counts how far.
- **The benchmark** (`scripts/automation-bench.sh`) runs section 1's
  operations four ways: native, Playwright's client through zebkit's
  endpoint and over CDP on the same build, and Playwright on Chrome. It is a standard test: a native command
  slower than Playwright over CDP on the same engine is a failure.

## 8. Risks and open questions

- **The injected script follows Playwright's server.** Its functions'
  signatures change between Playwright releases; zebkit pins a commit and
  moves with Playwright deliberately, re-running the comparison each time.
- **Playwright's behaviour that is in the server, not the script**
  (retry intervals, strict mode, what a navigation during an action does,
  error messages) is re-written in C++ and can drift; Playwright's test
  suite against the endpoint (4.8) is what will hold it; today the
  comparison with Chrome command by command and `playwright_connect.mjs` do.
- **Isolated world and CSP**: the injected script runs in an isolated
  world, which a page's Content Security Policy does not cover, as for
  Playwright.
- **Frames on demand** changes when frames are made, not what they show; a
  page that measures its frame rate sees a faster one while a command is in
  flight. Windowed views keep their display's rate.
- **The endpoint follows Playwright's protocol of the pinned release**
  (1.64): its object model, initializers and value forms change between
  releases, and a client of another release may not connect.
