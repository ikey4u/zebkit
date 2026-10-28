# Roadmap

Where zebkit stands and what comes next. How the engine is made is
`docs/ARCHITECTURE.md`; its size and speed, `docs/SIZE_AND_SPEED.md`. The
UI layer, applications in React, Solid or Vue drawn natively with each
system's look, and iOS, has its own roadmap: `docs/ROADMAP_UI.md`. How an
application decides what the engine does, through hooks rather than
patches, has its own: `docs/ROADMAP_HOOKS.md`.

## Requirements

zebkit is a library that applications ship and integrate, agents among
them: an application keeps its own process, main loop, windows and
toolkit, and calls zebkit where it wants a web page, a PDF, media or
zebkit's native UI. It is not a browser, and not a base that applications
are built on, as Electron is.

| Requirement | From |
|---|---|
| HTML, CSS and JavaScript as in Chrome. Features beyond that core (WebRTC, WebGPU, WebXR, WebNN, device APIs) may go | Owner, 2026-10-17 |
| CDP, compatible with Puppeteer, Playwright and raw clients | Owner |
| Media playback, H.264 and AAC included | Owner, 2026-10-10 |
| The JIT stays; no cut that costs much speed for little size | Owner, 2026-10-09 |
| A build that costs more in size than it gains in speed is not an improvement: `slim-speed` (19% for 7 to 11%) stays a profile, the release stays `slim` | Owner, 2026-10-27 |
| ICU's data for English, Chinese, Japanese and Korean at least | Owner |
| Linux on old systems: glibc 2.31 first, then 2.17 | Owner, 2026-10-17 |
| The same functions on Linux, macOS and Windows | Owner |
| Far smaller than Electron, uncompressed and compressed. The 150 MiB limit of 2026-10-19 gave way to speed on 2026-10-21 | Owner, 2026-10-18 to 2026-10-21 |
| Chromium's libraries, and what else in it is useful, exported for applications as a C API, with bindings and a JavaScript API | Owner, 2026-10-19 |
| A full PDF API on Chromium's PDFium, with EmbedPDF's features as the reference | Owner, 2026-10-20 |
| For agents, a general MCP tool with the engine's capabilities, not only a CDP endpoint for browser drivers | Owner, 2026-10-23 |
| A library that applications integrate, not a base they are built on: the application owns its process, loop, windows, menus and identity; zebkit.ui is one part of the library, usable inside the application's own windows | Owner, 2026-10-25 |
| Real web pages cost less than in the latest stable Electron, and less still over time: CPU while loading and at rest, memory, processes. A medium page in an application is not a browser's workload | Owner, 2026-10-23 |
| Off-screen views: frames as shared surfaces (IOSurfaces, shared memory, dmabufs) with their damage, input sent by the application, popups drawn off-screen, frames passable to another process | Owner, 2026-10-23 |

Kept by zebkit's own choice, open to the owner's overrule: WebGL,
WebAssembly, HTTP/3 and SwiftShader. Removed by zebkit's choice: WebRTC's
camera and microphone capture, and the device APIs.

## Done

| | What | When |
|---|---|---|
| Engine | Chromium 155's content layer as `libzebkit.so.1` and `zebkit-helper`, started by the application (`zebkit_main`) | 2026-10-20 |
| Size | 214.8 MiB to 134.8 MiB with no web feature lost; then 157.3 MiB for speed | 2026-10-17 to 2026-10-21 |
| Speed | Chromium's PGO profile, Blink's layout code at `-O2`, link-time optimization: text layout 13.2 to 8.7 ms (Chrome's own build: 6.8) | 2026-10-21 |
| Libraries | 15 C libraries inside the engine exported under `zebkit_vendor_*`, 7,230 functions, found in the build | 2026-10-20 |
| Services | Compression, cryptography, URLs, images, ICU's data, HTTP through the network stack | 2026-10-20 |
| Bindings | Rust (`zebkit`, `zebkit-rs`, `zebkit-build`), Python (`ctypes`) | 2026-10-20 |
| Page API | `zebkit.*` in pages the application grants, per origin and capability; calls over `zebkit://call`, bytes as they are | 2026-10-20, 2026-10-21 |
| Script outside a page | `zebkit_script_*` and `zebkit-js`: an application in JavaScript on V8, without Node | 2026-10-20 |
| Schemes | The application's own URL schemes, answered by it, files in ranges for media | 2026-10-20 |
| Views | Windows with titles, sizes and limits, views attached in another's window, profiles, user agents, pop-ups and drops to the application, the application's menu, files opened by the system | 2026-10-20, 2026-10-21 |
| PDF | Chromium's PDFium in the engine; `<zebkit/pdf.h>` and `zebkit.pdf`: documents, rendering, text, search, outline, links, annotations with appearances, forms, redaction, page editing, attachments, metadata, images, signatures checked for integrity; PDFium patched where it had nothing | 2026-10-20, 2026-10-21 |
| FFmpeg | zebkit's configuration: Chrome's decoders, and for applications the muxers, libswscale and libswresample | 2026-10-20 |
| macOS | `Zebkit.framework` for Apple silicon, built on Linux, every test passing on a Mac (`docs/MACOS.md`) | 2026-10-21 |
| Off-screen views | `<zebkit/offscreen.h>`: frames as IOSurfaces (macOS) or memfd shared memory (Linux) with damage, sequence, size and scale, given back by the application, held back when it holds its allowance, none while idle, a rate limit; mouse, wheel, keys, an input method, focus, cursor and caret; `<select>` lists and pickers as a popup layer; the Rust crate's `View::new_offscreen`; `examples/offscreen-host`, the frames shown by another process over a socket (fds, mach ports). Tested on Linux and on a Mac (`tests/engine/offscreen.c`) | 2026-10-23 |
| Media capture | `<zebkit/media.h>`: what a page's `<audio>` and `<video>` play copied into MP4, WebM or Matroska as it plays, without decoding: files, Media Source Extensions (DASH, HLS players), the engine's HLS; the stream's times, seeks, configuration changes, audio alone, encrypted media refused (`patches/1031`, `1032`; `tests/engine/media.c`) | 2026-10-25 |
| Old Linux | glibc 2.17 and newer (CentOS 7): the one newer symbol, Rust's weak `getrandom`, answered inside the engine; `scripts/package.sh` refuses a package that needs more; the C tests pass on CentOS 7's glibc and libraries (`scripts/glibc-test.sh`) | 2026-10-26 |

## Next

### The library

zebkit fits into the application, not the application into zebkit
(owner, 2026-10-25). Today it is closer to a base: `zebkit_main()` takes
over `main()` and runs Chromium's loop, settings are Chromium's command
line, and on macOS the engine sets the application's delegate, menu bar
and Dock policy.

- **TODO: `zebkit_init()` and `zebkit_shutdown()`**, from any point of the
  application's life, in place of `zebkit_main()` as the way in
  (`zebkit_main()` stays for programs that want the engine's loop).
- **TODO: the engine in the application's loop**: GTK (GLib's main
  context), Qt, AppKit (the run loop), Win32 and winit, through one
  integration point (a file descriptor or a wake-up callback and a
  `zebkit_pump()`), and the engine on a thread of its own for loops it
  cannot join. Calls from the application's thread, as today.
- **TODO: nothing taken from the application.** On macOS no delegate, menu
  bar or Dock policy of zebkit's unless asked (`zebkit_app_*` becomes
  opt-in); no signal handlers, crash reporter or log file of the engine's
  over the application's; the helper program the only process zebkit adds.
- **TODO: settings as an API**, a struct or JSON given to `zebkit_init()`
  (profile directory, proxy, cookies, user agent, GPU, logging), with
  Chromium's switches left as an escape hatch.
- **TODO: views in the application's own windows and widgets as the main
  path**: a view attached to a native window or widget handle (`NSView`,
  GTK widget, `HWND`, Qt `QWindow`), or off-screen with the application
  drawing its frames, rather than windows of zebkit's.
- **TODO: zebkit.ui in the application's windows too**: its windows and
  widgets usable as a part of an application written in another toolkit,
  not only as a whole application (`docs/ROADMAP_UI.md`).
- **TODO: examples of integration**: a GTK, a Qt, an AppKit and a winit
  application each adding a web view and a PDF view to a window of their
  own.

### The engine

- **zebkit's own embedder.** The engine still runs on the content shell's
  browser classes for windows and the browser context, which Chromium
  keeps for tests. Its own `ContentBrowserClient`, window and context,
  started from the shell without its test hooks; Electron's
  `native_window_views.cc` is the reference for the windows.
- **A window's native handle**, so that an application can parent the
  system's dialogs to it (file dialogs through the XDG portal need
  it).
- **The sandbox where there is none.** Chromium's Linux sandbox needs
  unprivileged user namespaces or a set-user-ID helper, which containers
  and build machines often lack; the engine then refuses to start without
  `--no-sandbox`. Agents run in such places: zebkit needs a stated answer
  (ship the helper, document `--no-sandbox` for containers, or both).
- **Text layout at Chrome's speed.** A chapter of a book takes 4.4 ms of
  CPU to lay out in zebkit `slim` and 3.5 ms in Electron 44.9
  (`scripts/layout-bench.sh`, `docs/SIZE_AND_SPEED.md`). Taken apart on
  2026-10-27: the engines do the same work; `slim`'s code runs slower
  for being compiled for size. `slim-speed` (`-O2` throughout, no
  size-trained inliner) takes 3.9 ms for 30 MiB more and 20 to 50 MB more
  a page; Google's own build of Chromium 155 takes 3.8. The release stays
  `slim` (owner, 2026-10-27): a build that costs more in size than it
  gains in speed is not an improvement. Beyond that the gap is Chromium
  155's against Electron's Chromium 152, not the build's.
- **CDP checked against Puppeteer and Playwright**, with Chrome's
  results as the reference: the engine serves Chromium's own DevTools
  endpoint, and a suite of client tests against it is still to be written.

### Off-screen views

- **Off-screen views in a process with windows.** They need `--headless`
  now: a view is a content shell window, drawn by the headless screen. A
  `RenderWidgetHostView` of zebkit's own with its own compositor, as
  CEF's and Electron's off-screen views have, would let one process have
  both.
- **A scale for each view.** The off-screen views of a process share the
  headless screen's scale; a display of its own for each needs the
  headless screen to place a display's DIPs apart from its pixels.
- **viz's own IOSurfaces on macOS.** Frames are read back and copied into
  IOSurfaces of the engine's; viz's blit into mappable shared images
  (`ZEBKIT_BUFFERS_GPU`) gives no frame for a page in no window. With it,
  the read-back and the copy go.
- **dmabufs checked on a GPU.** The path is there (`ZEBKIT_BUFFERS_GPU` on
  Linux, the planes, strides and modifier passed on), but the build
  machine has no GPU: it has only been seen to fall back to shared
  memory.
- **What a window does besides:** drags and drops in and out, the context
  menu (`ShowContextMenu` to the application), touch, accessibility, and
  editing commands on macOS (⌘A, ⌘C) as the system sends them.

### Media capture

- **Elements outside the document**: `new Audio()` that is never
  inserted, and media elements inside shadow roots, play but are not
  listed (an element under the point is found in a shadow root, by
  `zebkit_view_media_at()`). The players could be listed instead of the
  elements.
- **One file through a change of resolution.** An adaptive stream that
  changes quality during a capture goes on in another file, since a track
  of MP4 or Matroska has one configuration; H.264 and HEVC could stay in
  one file with their parameter sets in band (`avc3`, `hev1`).
- **`zebkit_view_media_at()` in frames of other sites**: it hit-tests the
  main frame's process, so a video in a cross-site frame is found by the
  list only.
- **Live streams' wall-clock times** (HLS's program date, DASH's
  availability start) are not written into the file.

### Agents

- **TODO: hooks for every decision the engine makes** (owner,
  2026-10-24): permissions, popups, navigation, network requests,
  downloads, certificates and the rest, each with zebkit's default, from
  native code and without patching zebkit: `docs/ROADMAP_HOOKS.md`. The
  settings below are its first part.
- **TODO: every setting changeable at runtime, by the application and by
  agents** (owner, 2026-10-24), per view and for the whole engine, in the C
  API, the bindings, CDP where Chromium has a method and the MCP server:
  - What Chromium's CDP already changes, checked to work in zebkit: user
    agent and client hints (`Emulation.setUserAgentOverride` with
    `userAgentMetadata`), language, time zone, geolocation, screen and
    device metrics, permissions (`Browser.setPermission`), cookies,
    extra headers, blocked URLs, offline and throttled networks.
  - What zebkit decides and Chromium's CDP does not change: third-party
    cookies (`Network.setCookieControls` only marks issues in Chromium
    155), the spare renderer, JavaScript dialogs held or dismissed (being
    built), downloads, new windows; each gets a runtime setting.
  - Defaults chosen for privacy (third-party cookies blocked, location,
    sensors, idle detection, NFC, MIDI, payment handlers and background
    work denied) stay defaults: changing them is the application's or the
    agent's explicit act.
- **Chrome as sites see it, and nothing of a CDP client: done**
  (2026-10-24, measured against rebrowser-bot-detector's checks and
  Electron 44.9): `window.chrome` by Chrome's own code, popups blocked
  without a click, a real screen under `--headless`, and V8's inspector
  running none of the page's code when it describes the page's errors
  (`patches/1026`; Chrome and Electron still do). Held by
  `tests/engine/cdp_browser.py`.
  - **Legacy `chrome.app` queries: done** (2026-10-26), implemented natively
    for the actual uninstalled hosted-app state, without an extension runtime.
    The same slim binary passes Google's email/Next check with these queries,
    rejects it without them, and passes again after restoring them
    ([comparison](GOOGLE_SIGNIN.md)).
- **TODO: PDF in pages**, on zebkit's PDFium: a page that opens a PDF shows
  it, as Chrome's viewer does, and `navigator.pdfViewerEnabled` and
  `navigator.plugins` then say what Chrome says. Today both are empty,
  which browserleaks.com shows and bot checks read as a headless browser.
- **TODO: network quality estimates**: `navigator.connection` reports `rtt`
  0 and `downlink` 10, fixed values, where Chrome reports measured ones.
- **The privacy defaults tested on every change**: browserleaks.com's
  checks (WebRTC, permissions, client hints, TLS, JavaScript) in
  scripts/web-bench.sh, against what zebkit's defaults promise.

- **Playwright's API, natively: built** (owner, 2026-10-27;
  `docs/AUTOMATION.md`). `<zebkit/automation.h>` runs Playwright's
  protocol commands inside the engine, without CDP or a driver: Playwright's
  injected script in an isolated world of each frame, input to the
  widgets, routes in front of the network; 3.4 to 10 times faster than
  Playwright over CDP for what does not wait for a frame. Python
  (`zebkit.automation`) and Rust (`zebkit::automation`) have it with
  Playwright's names. Contexts as profiles of their own, workers,
  WebSocket routes, geolocation, HTTP credentials, print media, touch
  events, mouse input into frames of other sites, traces, HAR, video and
  frames on demand for views without a display are built (2026-10-27), and
  Playwright's own protocol over a WebSocket (`zebkit_automation_listen`,
  `zebkit-run --playwright-port`), so Playwright's clients drive zebkit
  unchanged with `chromium.connect()`. Held by `tests/engine/automation.c`,
  `automation_python.py`, `playwright_connect.mjs`,
  `scripts/automation-bench.sh`.
  - **TODO**: Playwright's own test suite against the endpoint;
    `Disposable.dispose` (exposed bindings and init scripts stay until the
    page closes); request timing and sizes.
- **TODO: zebkit as an MCP server.** One program, `tools/zebkit-mcp`,
  that gives an AI agent the engine's capabilities as MCP tools, over
  standard input and over HTTP, on Linux and macOS alike. CDP stays for
  clients that speak it; MCP is what agents are given.
  - **Pages:** open, navigate, wait for a load; read a page as text,
    Markdown or its accessibility tree, with references to its elements;
    click, type, select, scroll and upload by those references; run
    script; a capture of the page or an element; the network's requests;
    several pages and profiles at once, with or without windows.
  - **PDF:** open, read text with its layout, search, render pages, the
    outline and links, fill forms, annotate, redact, edit pages, check
    signatures, and print a page to PDF.
  - **Files and data:** read and write within the directories granted,
    fetch through the engine's network stack, compress, hash, decode and
    encode images, parse URLs.
  - **Media:** probe, cut, convert and extract audio and frames with the
    FFmpeg in the engine.
  - **Native windows:** `zebkit.ui`'s tree read, clicked, typed into and
    captured, as pages are.
  - **Bounds set by whoever runs it:** which tools, which origins, which
    directories, with or without a window; every tool's arguments
    checked, as `zebkit.*` checks a page's calls.
  - **Checked by:** an MCP client's own test suite and an agent doing
    real tasks (research on the web, filling a form, reading a long PDF),
    with every tool covered by `tests/engine/`.
- **TODO: signing in without a screen: OAuth's device flow** (owner,
  2026-10-24). An application or agent that runs headless, on a server or
  without a display, gets an account's tokens without a browser on its
  own machine (RFC 8628): it asks the provider for a code, shows the
  person a short code and a URL, the person approves on their phone or
  laptop, and the tokens come back. This is how Google, Microsoft and
  GitHub let a device with no browser sign in; it gives access to the
  account's APIs, not a session on the provider's website (see the notice
  on Google in `docs/API.md`).
  - **One function for every provider.** `zebkit_oauth_device_start()`
    takes the provider's two endpoints (device authorization and token),
    the application's client ID and the scopes, as JSON, with presets for
    Google, Microsoft and GitHub. It calls back with the code to show
    (`user_code`, `verification_uri`, `verification_uri_complete` when the
    provider has it, and when it expires), then polls at the provider's
    `interval`, slowing down on `slow_down`, and calls back once with the
    tokens or with why not (`access_denied`, `expired_token`). A refresh
    function renews an access token from its refresh token.
  - **Through the engine's network stack**, as `fetch` from zebkit's
    script outside pages goes: its proxy settings, certificates and
    HTTP/3. Cancelled with a handle; nothing is kept on disk by zebkit,
    the tokens are the application's.
  - **Shown as the application likes:** the code and URL as text, and a
    QR code of `verification_uri_complete` as a PNG from zebkit's image
    encoder, for a panel, a terminal or a message to the person.
  - **The application's own registration.** Each provider wants a client
    ID registered for devices ("TVs and limited input devices" at Google,
    a public client at Microsoft, device flow turned on at GitHub); zebkit
    ships none. Google allows only some scopes in this flow (its sign-in,
    Drive's files the application made, YouTube and a few more): the
    document says which, and the browser flow (`docs/API.md`) is for the
    rest.
  - **In C, Rust, Python and JavaScript** (`zebkit.oauth.device()` in
    script outside pages, a promise and an event for the code), and an
    MCP tool, so that an agent asks its person to approve and goes on.
  - **Checked by:** a test provider in `tests/engine/` that answers as
    RFC 8628 says (pending, slow down, denied, expired, granted), and once
    by hand against Google, Microsoft and GitHub with test clients.

### Measurements

- **Web pages against Electron 44.9: done for 14 pages, one left over.**
  `slim` costs 16% less CPU and 16% less memory on a page, over 14 of the
  15 pages of `scripts/web-bench.sh --sites`, both engines in a 1280 by
  800 window (`docs/SIZE_AND_SPEED.md`); less memory on every page, less
  CPU on 12, level on BBC News and Stack Overflow.
  - **TODO: Reddit** showed zebkit a reCAPTCHA in place of the page, and
    Electron a challenge it passed; until both are shown the page, it is
    out of the comparison.
- **TODO: a fully equal comparison with Electron.** The comparison of
  whole applications that was published (a document reader against its
  build on Electron 36) was withdrawn on 2026-10-23: an old Electron, and
  two builds of an application that differed in more than the runtime. To
  be made again, and published only when every condition below holds:
  - **One application, two hosts.** The same interface and the same work
    on both: one source for the pages and their logic, and two thin hosts
    that do the same things, Electron's main process and zebkit's. What
    the application calls (files, PDF, HTTP, bytes between page and
    host) is the same call on both, or both use the same library.
  - **The latest stable Electron** at the time of measuring, named with its
    Chromium version beside zebkit's.
  - **Production builds of both**, packaged as they ship, on the same
    machine, at rest, the same display and the same files.
  - **The same harness** drives both and reads both: the same steps, the
    same clock, medians of at least five runs with the spread, cold start
    and warm start apart.
  - **What is measured:** start to the first frame; CPU and memory at rest
    (every process counted); page work (text layout, JavaScript, DOM);
    heavy documents (a long PDF and an EPUB: first page, fully loaded,
    longest freeze, scrolling frames at p50, p95 and max, CPU time,
    memory); calls between page and host (latency and throughput, small
    and large bytes); the download and the installed size.
  - **Linux and macOS**, each on its own machine; Windows when zebkit
    runs there.
  - **Repeatable:** the application, both hosts and the harness in
    `tests/bench/`, one command to run it; the results with the date, the
    versions and the machine in `docs/SIZE_AND_SPEED.md`, losses shown as
    plainly as wins.

### Platforms

- **Windows**: the same profile and the same API.
- **macOS**: real windows checked with a person at the Mac (attached
  views, drops, the menu bar's keys); Chromium's PGO profile for macOS;
  signing with a Developer ID and notarization.

### Releases

- The latest engine, 0.0.1-alpha.12 (with off-screen views, dialogs, printing to PDF, drag and drop, sign-in popups, media capture, persistent cookies, glibc 2.17, passkeys, Playwright's API natively and its own endpoint), is published for Linux and macOS;
  the Rust crates download a release only from a repository anyone can
  read, so the repository has to be public for them.

### The API

- **TODO: annotations as a full PDF editor has them.** Today every type
  is read, the common ones are made with their appearance, and redaction
  removes content for good; what an editor needs beyond that, in this
  order, each in `<zebkit/pdf.h>`, `zebkit.pdf` and its tests:
  1. **Appearance**: line endings (open and closed arrows, circle,
     square, diamond, butt, slash), leader lines and captions on lines;
     dashed and cloudy borders on squares, circles and polygons; opacity,
     fill and blend drawn into every generated appearance; an
     annotation's own rotation.
  2. **Free text**: font, size, colour and alignment (`/DA`, `/Q`), rich
     text (`/RC`, `/DS`), background and border, the callout line with its
     ending (`/CL`, `/LE`), rotation; the appearance drawn from all of it.
  3. **Stamps and the rest made**: the standard stamps (Approved, Draft,
     Confidential, ...) drawn, text stamps, stamps from another
     document's page as vectors; carets; notes with each icon; file
     attachments with the file embedded.
  4. **Discussion**: replies (`/IRT`), review and marking states
     (accepted, rejected, completed), groups (`/RT /Group`), and their
     popups, read and made.
  5. **Measurement**: distance, perimeter and area, with a scale
     (`/Measure`) and the value as the caption.
  6. **For viewers and editors**: the annotation at a point; markup made
     from a range of the text; the order on the page changed; copying to
     another page or document; one annotation flattened or exported.
  7. **Interchange**: XFDF and FDF in and out, the formats Acrobat and
     other tools exchange annotations in.
  Every appearance checked against Acrobat's and PDF.js's rendering of
  the same file; patches to PDFium where it lacks a generator, as
  `patches/1015` does for lines and polygons.
- **TODO: PDF extensible by applications, with hooks everywhere.** The
  PDF library is the application's to extend, not a closed set of calls:
  the same hooks in C (`zebkit_pdf_hook_*`), Rust, Python and
  `zebkit.pdf` (`zebkit.pdf.use(plugin)` for a plugin of several).
  - **Custom annotation types**: an application registers a kind of its
    own (`/Subtype` or an application prefix): its properties and their
    checks, the drawing of its appearance (a canvas API on Skia, written
    into the appearance stream), its hit testing, and its data, kept in
    the document under the application's prefix and carried through save,
    copy, flatten and XFDF.
  - **Appearance providers**: any type's appearance drawn by the
    application instead of the built-in generator (its own stamps, fonts,
    arrowheads), per document or for all.
  - **Events, before and after**: opening, saving, a page rendered, an
    annotation added, changed or removed, a form field changed, a
    redaction applied, pages inserted, moved or deleted. A "before" hook
    may change what is about to happen or refuse it; an "after" hook sees
    what changed. Changes come as operations, so that an application can
    keep an undo history or send them to collaborators.
  - **Render layers**: drawing above or below a page's content when it is
    rendered, in page space, for selections, search results, a
    collaborator's cursor or a watermark.
  - **Providers for what the library does not decide**: text for scanned
    pages (an OCR engine of the application's), fonts for appearances and
    free text, trust in a signer's certificate, and actions (links,
    named, launch, JavaScript) handled by the application.
  - **Tools for viewers**: input filters for drawing (smoothing ink,
    snapping lines, constraining shapes) that a viewer's tools run through.
  - **Checked by:** an example plugin that adds an annotation kind with
    its own appearance, keeps an undo history and supplies OCR text, the
    same in C and in JavaScript, with tests for every hook.
- **PDF, beyond annotations**: trust in a signer's certificate (the chain,
  revocation), editing the outline, encrypted documents saved with new
  permissions, form fields renamed, a page's JavaScript actions. PDFium in
  a sandboxed utility process for pages' documents, as Chrome's printing
  runs it.
- **Services, later areas**: 2D drawing on Skia's canvas, shaping a line of
  text, decoding media frames, a key-value store; the desktop's
  clipboard, notifications, file dialogs, screens, power and keychain.
- **FreeType** among the exported libraries.
- **Bindings** generated from the headers for more languages: Go, C#,
  Node's N-API.
- **Loading the engine with `dlopen()`** after threads have started (one
  of `base`'s thread-local pointers goes stale); linking it, or loading it
  first, is what works now.

## Decided

| Decision | When |
|---|---|
| Chromium's content layer and Blink, one engine on every system | 2026-10-17 |
| The engine is a library with a helper program, started by the application | 2026-10-19 |
| The size limit is 150 MiB; 134.8 MiB accepted, no feature cut for size | 2026-10-19 |
| Speed before the 150 MiB limit | 2026-10-21 |
| PDF on Chromium's own PDFium, patched where needed; EmbedPDF's feature list is the reference, not its code | 2026-10-20 |
| Porting Android's engine: not done (`docs/SIZE_AND_SPEED.md`) | 2026-10-18 |

## Costs to plan for

- **Builds.** The checkout is about 30 GB; a full build takes hours, and a
  change of compiler flags rebuilds everything. Changes are batched into
  one rebuild where possible.
- **Patches.** Chromium releases every four weeks, and every patch has to
  apply to each release. Switches are preferred to patches.
- **Security.** Fixes come from upstream continually; zebkit has to follow
  stable releases closely, another reason to keep the patch set small.
