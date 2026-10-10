# Chrome DevTools Protocol

zebkit is meant to be embedded in agent applications, and the tools agents
drive a browser with (Puppeteer, Playwright, and libraries that speak the
protocol directly) all talk the Chrome DevTools Protocol. WebKit does not.
It has its own Web Inspector Protocol, which reports much of the same
information in different shapes and has nothing for the browser-level parts:
opening pages, navigating, mouse and keyboard.

The engine therefore carries a CDP endpoint of its own (`src/cdp`, compiled
into `libWPEWebKit`). It serves the HTTP and WebSocket interface CDP clients
expect, answers what it can from the WebKit API, and translates the rest to
and from the Web Inspector Protocol.

**This is a subset of CDP, not Chrome.** It covers what automation needs and
is tested with real clients; the tables below say exactly what is in and
what is not. A method that is not implemented answers with CDP's "wasn't
found" error instead of pretending to succeed.

## Using it

Start the endpoint in one of two ways:

- Set `ZEBKIT_CDP_PORT` in the environment of any application that embeds
  the engine. `ZEBKIT_CDP_ADDRESS` chooses the address (default
  `127.0.0.1`); port `0` lets the system pick.
- Call `webkit_zebkit_cdp_listen(address, port, &error)` from the
  application. The header is `src/cdp/zebkit-cdp.h`; `scripts/package.sh`
  puts a copy in the package's `include/`. `webkit_zebkit_cdp_stop()`
  disconnects every client, closes the pages the endpoint opened and stops
  listening.

On start the engine prints the line Chrome prints,
`DevTools listening on ws://127.0.0.1:PORT/devtools/browser/ID`, and every
`WebKitWebView` of the process becomes a page a client can attach to.

```js
// Puppeteer
const browser = await puppeteer.connect({ browserURL: 'http://127.0.0.1:9222' });
// Playwright
const browser = await chromium.connectOverCDP('http://127.0.0.1:9222');
```

`tools/cdphost` is a complete embedding application in 90 lines: one view
and a call to `webkit_zebkit_cdp_listen()`.

### Pages the client opens

`Target.createTarget`, `/json/new` and `window.open()` need a new view. By
default the endpoint creates one itself, with the settings, web context and
display of the application's first view, and destroys it when it is closed.
A window a page opens becomes such a view only while a client is connected;
with nobody to see or close it, it would stay for the life of the process.
An application that wants to own these views (to put them in its own
windows, say) registers a function with
`webkit_zebkit_cdp_set_create_view_func()`.

`Target.createBrowserContext` gives a context with its own cookies, cache
and storage, backed by an ephemeral `WebKitNetworkSession`.

### Who can connect

The endpoint gives full control of the browser to whoever reaches it, as
Chrome's does. The same precautions apply and the same defaults are built
in:

- It listens on `127.0.0.1` unless told otherwise. Do not bind it to a
  public address; put an authenticated tunnel in front if it must be
  reached from another machine.
- HTTP requests are refused unless their `Host` header is an IP address or
  `localhost`, so a web page cannot reach the endpoint through a host name
  it controls.
- WebSocket connections and HTTP requests that carry an `Origin` header
  (that is, come from a web page) are refused. `ZEBKIT_CDP_ALLOW_ORIGINS`
  takes a comma-separated list of origins to allow, or `*`.
- `/json/new` needs the `PUT` method, as in Chrome since version 111: a
  page can send a `GET` anywhere without asking.

**There is no password.** With the defaults, any program running on the
same machine, under any user, can ask `/json/version` where the sockets are
and take over the browser, which includes reading its cookies and, through
file inputs and `file:` addresses, the files the application can read. That
is how Chrome's port behaves too, and it is fine on a machine that only the
agent's owner uses. Where it is not:

- Call `webkit_zebkit_cdp_set_discovery_enabled(FALSE)` before listening,
  or set `ZEBKIT_CDP_DISCOVERY=0`. The `/json` pages are then not served,
  and the browser socket is reachable only at the address
  `webkit_zebkit_cdp_get_url()` returns (the same one the engine prints on
  its standard error), which contains a random 122-bit id. Hand that
  address to the client: `puppeteer.connect({ browserWSEndpoint })`,
  `chromium.connectOverCDP(wsEndpoint)`.
- A message larger than 256 MiB closes the connection.

## How it works

```
CDP client ── WebSocket ──> src/cdp (UI process)
                              ├─ WebKit API: navigation, input, views, cookies, dialogs
                              └─ Web Inspector Protocol ──> page (web process)
```

| File | What is in it |
|---|---|
| `CDPServer.cpp` | HTTP discovery, sockets, sessions, and the `Target`, `Browser` and `Storage` domains |
| `CDPPage.cpp` | One page: the inspector connection, and the page's events translated to CDP |
| `CDPDomains.cpp` | `Page`, `Runtime`, `DOM`, `Network`, `Emulation` commands |
| `CDPInput.cpp` | Navigation, mouse and keyboard, window size, dialogs, file choosers |
| `CDPImage.cpp` | JPEG and WebP screenshots |

Things worth knowing when something behaves unexpectedly:

- **Input is real input.** Mouse and key events are delivered to the view
  as platform events, the path a physical mouse and keyboard take. Pages see
  trusted events, and focus, text entry and link activation happen as they
  would for a user. A command is answered after the page has handled the
  event.
- **Navigating to another site can move the page to a new process.** WebKit
  then creates a second inspector target and commits it later. The endpoint
  sets up each new target (enabled domains, scripts for new documents,
  bindings, extra headers, user agent) before it is allowed to load, and
  keeps the page's frame id the same throughout, so clients do not notice.
- **Isolated worlds are emulated.** WebKit's protocol cannot create one.
  When a client asks for a world (`Page.createIsolatedWorld`, or a
  `worldName` on a script), it is given a second execution context id for
  the frame, and commands sent to it run in the frame's main world.
  Puppeteer and Playwright need their helper world to exist; they do not
  depend on it being separate. Page script can, however, see what is put
  there.
- **Frames are part of their page.** There are no separate targets for
  iframes or workers, also for frames of another site; they are reached
  through the page's session, with their own execution contexts.
- **Ids are the bridge's.** The main frame has the target's id, as in
  Chrome. WebKit numbers other frames, loaders and requests per process,
  starting over in each one, so those ids get a prefix that changes when
  the page changes process.
- **The back/forward cache is off while a client is attached.** WebKit's
  inspector does not report the script contexts of a page restored from it,
  so history navigations load the page again. The setting is the
  application's `WebKitSettings`, which views may share; it gets its own
  value back when the last attached page is released.
- **What a client changes outside the page is undone when it leaves:** the
  view's size (`Emulation.setDeviceMetricsOverride`), ignored certificate
  errors, and a dialog it was shown and did not answer, which would
  otherwise hold the page's script for ever.
- **When the page's process dies** clients get `Inspector.targetCrashed`,
  commands that were waiting and commands sent afterwards fail with
  "Target crashed", and `Page.navigate` or `Page.reload` brings the page
  back in a new process with the client's setup applied again.
  `Page.crash` kills the process on purpose.
- **Answers to input commands keep the order of the commands.** Clients
  send a press and its release together and work out which buttons are down
  from the order of the answers.
- **A failed navigation** is reported (`errorText` from `Page.navigate`)
  after WebKit's error page has committed, so that the commit is not taken
  for part of the next navigation.
- **"Network idle" is computed**, as Chrome defines it: after the load
  event, `networkAlmostIdle` when at most two requests have been in flight
  for 500 ms and `networkIdle` when none has.
- **`/devtools/wip/<targetId>`** is a WebSocket that carries WebKit's own
  protocol untranslated, for clients that speak it and for debugging the
  bridge. Do not use it on a page that also has CDP sessions.

## What is covered

Commands not listed are not implemented.

| Domain | Commands |
|---|---|
| HTTP | `/json/version`, `/json`, `/json/list`, `PUT /json/new?URL`, `/json/close/ID`, `/json/activate/ID` |
| Target | `setDiscoverTargets`, `getTargets`, `getTargetInfo`, `setAutoAttach` (with `waitForDebuggerOnStart`), `attachToTarget` (flat sessions), `detachFromTarget`, `createTarget`, `closeTarget`, `activateTarget`, `createBrowserContext`, `getBrowserContexts`, `disposeBrowserContext` |
| Browser | `getVersion`, `close` (closes the pages the endpoint opened), `getWindowForTarget`, `getWindowBounds` |
| Page | `enable`, `disable`, `navigate`, `reload`, `stopLoading`, `getFrameTree`, `getNavigationHistory`, `navigateToHistoryEntry`, `setLifecycleEventsEnabled`, `addScriptToEvaluateOnNewDocument`, `removeScriptToEvaluateOnNewDocument`, `createIsolatedWorld`, `captureScreenshot` (PNG, JPEG, WebP; viewport or clip), `getLayoutMetrics`, `handleJavaScriptDialog`, `bringToFront`, `close`, `crash` |
| Runtime | `enable`, `disable`, `evaluate`, `callFunctionOn`, `awaitPromise`, `getProperties`, `releaseObject`, `releaseObjectGroup`, `addBinding`, `removeBinding`, `runIfWaitingForDebugger` |
| DOM | `enable`, `disable`, `getDocument` (whole tree with `depth: -1`), `describeNode`, `resolveNode`, `requestNode`, `getBoxModel`, `getContentQuads`, `scrollIntoViewIfNeeded`, `focus`, `getOuterHTML`, `setFileInputFiles`, `getFrameOwner`, `querySelector`, `querySelectorAll`, `getAttributes`, `setAttributeValue`, `setAttributesAsText`, `removeAttribute`, `removeNode`, `setNodeValue`, `setNodeName`, `setOuterHTML`, `requestChildNodes`, `performSearch`, `getSearchResults`, `discardSearchResults` |
| Input | `dispatchMouseEvent` (move, press, release, wheel), `dispatchKeyEvent`, `insertText` |
| Network | `enable`, `disable`, `getResponseBody`, `setExtraHTTPHeaders`, `setCacheDisabled`, `setUserAgentOverride`, `getCookies`, `getAllCookies`, `setCookie`, `setCookies`, `deleteCookies`, `clearBrowserCookies` |
| Storage | `getCookies`, `setCookies`, `clearCookies` |
| Emulation | `setDeviceMetricsOverride` (resizes the view), `clearDeviceMetricsOverride`, `setUserAgentOverride`, `setEmulatedMedia` (media type) |
| Security | `setIgnoreCertificateErrors` |
| Log | `enable`, `disable` |

Events: `Target.targetCreated`, `targetInfoChanged`, `targetDestroyed`,
`attachedToTarget`, `detachedFromTarget`; `Page.frameAttached`,
`frameNavigated`, `frameDetached`, `frameStartedLoading`,
`frameStoppedLoading`, `navigatedWithinDocument`, `domContentEventFired`,
`loadEventFired`, `lifecycleEvent`, `javascriptDialogOpening`,
`javascriptDialogClosed`; `Runtime.executionContextCreated`,
`executionContextDestroyed`, `executionContextsCleared`, `consoleAPICalled`,
`exceptionThrown`, `bindingCalled`; `Network.requestWillBeSent`,
`responseReceived`, `dataReceived`, `loadingFinished`, `loadingFailed`;
`Log.entryAdded`; `DOM.documentUpdated`, `setChildNodes`, `attributeModified`,
`attributeRemoved`, `characterDataModified`, `childNodeCountUpdated`,
`childNodeInserted`, `childNodeRemoved`; `Inspector.targetCrashed`.

Some requests are answered with success and do nothing, because client
libraries send them while setting up and stop if they fail. Most only turn
on reporting this endpoint does not have. A few ask for an effect that then
does not happen, and a client is not told: downloads are not redirected,
permissions are not granted, touch is not emulated, the screenshot
background is not made transparent. They are: `CSS.enable`, `Overlay.enable`, `Performance.enable`,
`Security.enable`, `ServiceWorker.enable`, `Audits.enable`,
`Accessibility.enable`, `Inspector.enable`, `Fetch.disable`,
`Page.setInterceptFileChooserDialog`, `Page.setDownloadBehavior`,
`Browser.setDownloadBehavior`, `Browser.setWindowBounds`,
`Browser.grantPermissions`, `Emulation.setFocusEmulationEnabled`,
`Emulation.setTouchEmulationEnabled`, `Emulation.setScrollbarsHidden`,
`Emulation.setDefaultBackgroundColorOverride` and a few like them. The
whole list is in `CDPDomains.cpp` (`acknowledged`) and `CDPServer.cpp`.

## What is not covered

| Missing | What it means for a client |
|---|---|
| `Fetch` domain, `Network.setRequestInterception` | No request interception or mocking: `page.setRequestInterception()`, `page.route()` fail. WebKit's protocol has interception, so this can be added. |
| `Accessibility.getFullAXTree`, `DOMSnapshot` | Clients that read the page through the accessibility tree or DOM snapshots need another way (script in the page, `DOM.getDocument`). Playwright's `getByRole` works: it is computed by script. |
| `Input.dispatchTouchEvent`, drag-and-drop interception | Mouse and keyboard only. |
| `Emulation`: device scale factor, mobile mode, touch, geolocation, timezone, locale, CPU and network throttling | `setDeviceMetricsOverride` changes the size only. |
| `Page.printToPDF`, screencast, `Tracing`, `Performance.getMetrics`, `Profiler`, `HeapProfiler`, `Debugger`, `CSS` | No PDF output, tracing, profiling or script debugging over CDP. |
| Downloads | `setDownloadBehavior` is acknowledged; downloads follow the application's WebKit download handling, and no download events are sent. |
| Workers and service workers as targets | Not visible to the client. They run. |
| Permissions (`Browser.grantPermissions`) | Acknowledged, not applied; permission requests go to the application. |
| `fromSurface`, `scale` in clips | Screenshots are software-rendered snapshots of the page at CSS pixel size, at most 16384 pixels on a side (Chrome's limit too); a taller page is cut off there. |
| Old-style sessions (`Target.sendMessageToTarget`) | Only flat sessions, which is what current clients use. |

Smaller differences from Chrome:

- `Browser.getVersion` reports `Zebkit/<WebKit version>`, and the user agent
  is WebKit's (Safari-like). Sites are served what they serve Safari.
- Response objects carry status, headers and MIME type, not timing, remote
  address or TLS details. `encodedDataLength` is the body size.
- Load progress (`DOMContentLoaded`, `load`) is reported by WebKit for the
  main frame only; for child frames both lifecycle events are sent when the
  frame commits.
- Error descriptions and stack traces are JavaScriptCore's, which word some
  messages differently from V8.
- While a JavaScript dialog is open the page's process is blocked; commands
  to that page wait until `Page.handleJavaScriptDialog`.
- `Target.closeTarget` destroys a page the endpoint created without asking
  it (no `beforeunload`), as Chrome does. A view that belongs to the
  application can only be asked to close, like `Page.close`.
- State set through one session of a page (scripts for new documents,
  bindings, headers, user agent) belongs to the page and lasts until its
  last session is gone.
- `DOM.describeNode` keeps the nodes it gave a `backendNodeId` for alive
  until the document is left; beyond 20000 per document the oldest ids stop
  resolving.
- A page that never answers (script in an endless loop) leaves commands
  without an answer, as in Chrome. Clients have their own timeouts.

## Debugging

`ZEBKIT_CDP_LOG=1` prints every CDP message to stderr; `2` adds the Web
Inspector Protocol messages behind them, tagged with the first characters
of the page's target id.

## Testing

```sh
scripts/cdp.sh slim              # raw protocol checks
scripts/cdp.sh slim --clients    # also Puppeteer and Playwright
scripts/cdp.sh slim --serve      # leave the browser running on port 9333
```

- `tests/cdp/protocol.py` speaks CDP directly (a small client on Python's
  standard library) and checks answers against what Chrome gives: discovery,
  access control, evaluation, contexts and worlds, bindings, console,
  navigation events and failures, changing origin, history, screenshots,
  dialogs, mouse, keyboard, DOM geometry (including inside frames), file
  inputs, cookies, targets, sessions, browser contexts and popups. It also
  checks what happens when things go wrong: requests a web page could
  forge, ids chosen to break the bookkeeping, a stopped navigation, a
  crashed page and its recovery, a client that leaves a dialog open, a file
  upload aimed at the wrong element, and the order of input answers.
- `tests/cdp/private.py` runs against an endpoint started without
  discovery.
- `tests/cdp/puppeteer.js` and `tests/cdp/playwright.js` run the same kind
  of script an agent would: connect, open a page, navigate, evaluate, click,
  type, work in frames, take screenshots, go back and forward, handle
  dialogs and popups, upload a file, use cookies and an isolated context.
  `--clients` downloads a pinned Node.js and the two client libraries into
  `.cache/`; they are not part of the engine.

Results are recorded in `docs/PROGRESS.md`. `profiles/slim-dev.cmake` builds the
engine without link-time optimization; with it a change to `src/cdp` is
running again in about 15 seconds.
