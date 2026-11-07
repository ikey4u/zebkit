# Roadmap

Where zebkit stands and what comes next. How the engine is made is
`docs/ARCHITECTURE.md`; its size and speed, `docs/SIZE_AND_SPEED.md`;
what `zebkit.ui` is for, `docs/UI.md`. How an application decides what
the engine does, through hooks rather than patches, has its own roadmap:
`docs/ROADMAP_HOOKS.md`.

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
| No UI layer of zebkit's own (the PocketJS and Native Shell plan of 2026-10-23 is retired): rich UI is a page, `zebkit.ui` is the window, the prompts and the dialogs around it (`docs/UI.md`) | Owner, 2026-10-28 |
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
| Libraries | 17 C libraries inside the engine exported under `zebkit_vendor_*`, 7,489 functions in Linux `slim`, found in the build | 2026-10-20, FreeType and LevelDB 2026-10-29, 2026-10-30 |
| Services | Compression, cryptography, URLs, images, ICU's data, HTTP through the network stack | 2026-10-20 |
| Bindings | Rust (`zebkit`, `zebkit-rs`, `zebkit-build`), Python (`ctypes`) | 2026-10-20 |
| Page API | `zebkit.*` in pages the application grants, per origin and capability; calls over each frame's Mojo channel, bytes as they are, large ones in shared memory | 2026-10-20, 2026-10-21, 2026-11-02 |
| Script outside a page | `zebkit_script_*` and `zebkit-js`: an application in JavaScript on V8, without Node | 2026-10-20 |
| Schemes | The application's own URL schemes, answered by it, files in ranges for media | 2026-10-20 |
| Views | Windows with titles, sizes and limits, views attached in another's window, profiles, user agents, pop-ups and drops to the application, the application's menu, files opened by the system | 2026-10-20, 2026-10-21 |
| PDF | Chromium's PDFium in the engine; `<zebkit/pdf.h>` and `zebkit.pdf`: documents, rendering, text, search, outline, links, annotations with appearances, forms, redaction, page editing, attachments, metadata, images, signatures checked for integrity; PDFium patched where it had nothing | 2026-10-20, 2026-10-21 |
| PDF annotations | Appearance (endings, leaders, captions, dashed and cloudy borders, opacity, blend, rotation), free text, stamps, carets, notes, attachments, discussion, measurement, viewers' helpers, XFDF and FDF, without a patch to PDFium. Checked in PDFium's pixels with save-and-reopen in `tests/engine/pdf_annotations.c`; one sample page of them looked at in poppler and Ghostscript, which draw it alike (poppler prints one "Bad appearance" warning, not explained yet) | 2026-10-30 |
| PDF hooks | `zebkit_pdf_hook_*`, Rust, Python, `zebkit.pdf.use()`: kinds, appearance providers, events as operations with inverses, render layers, OCR, fonts, trust, actions, input filters; `examples/pdf-plugin/` in C and JavaScript | 2026-10-30 |
| Canvas and text | `<zebkit/canvas.h>` on Skia, `<zebkit/text.h>` on HarfBuzz. With the PDF work above: library +0.41 MB (146.04 to 146.45 MB, 0.28%), package +0.44 MB (0.26%), the library in xz +0.14 MB. No effect on page speed is expected, none was measured | 2026-10-30 |
| FFmpeg | zebkit's configuration: Chrome's decoders, and for applications the muxers, libswscale and libswresample | 2026-10-20 |
| macOS | `Zebkit.framework` for Apple silicon, built on Linux. The whole suite, cross-built, run on a Mac (M4, macOS 27.0.1): all pass; what could not run, the real Keychain, real input and devices, WebRTC between peers, is listed in `docs/MACOS.md`. Hardware compositing on Metal, `zebkit_init()`, a view on an `NSView` and the AppKit example seen working | 2026-10-21, again 2026-10-31 |
| Off-screen views | `<zebkit/offscreen.h>`: frames as IOSurfaces (macOS) or memfd shared memory (Linux) with damage, sequence, size and scale, given back by the application, held back when it holds its allowance, none while idle, a rate limit; mouse, wheel, keys, an input method, focus, cursor and caret; `<select>` lists and pickers as a popup layer; the Rust crate's `View::new_offscreen`; `examples/offscreen-host`, the frames shown by another process over a socket (fds, mach ports). Tested on Linux and on a Mac (`tests/engine/offscreen.c`) | 2026-10-23 |
| Media capture | `<zebkit/media.h>`: what a page's `<audio>` and `<video>` play copied into MP4, WebM or Matroska as it plays, without decoding: files, Media Source Extensions (DASH, HLS players), the engine's HLS; the stream's times, seeks, configuration changes, audio alone, encrypted media refused (`patches/1031`, `1032`; `tests/engine/media.c`) | 2026-10-25 |
| Application's loop | `zebkit_init()`, `zebkit_pump()`, `zebkit_set_wakeup()`, `zebkit_shutdown()`; `zebkit_main()` stays. Host state left alone (signals, locale, thread names, process title, crash handlers, environment); startup settings as validated JSON. C, Rust and Python tests on Linux; a concurrent monitor shows the host's signal handlers are never replaced | 2026-10-30 |
| Application's windows, frames | `examples/native-host`: web frames and a PDF view drawn in GTK, Qt, AppKit and winit windows through off-screen views, with no window of the engine's. GTK, Qt and winit run with real input on Linux; AppKit draws both panes and shuts down cleanly on a Mac, its input not driven | 2026-10-30 |
| Application's windows, handles | `zebkit_view_attach_native()`: the engine's window inside an X11 window of the host's, or on an `NSView` on macOS (`tests/engine/native_parent_mac.m`, 43 checks on a Mac). C, Rust, Python (`examples/native-host/x11.c`; JavaScript has no host handle). `tests/engine/native_parent.c` under Xvfb: the tree, the pixels, XTEST clicks and keys, focus, resizing, a `<select>`'s list, either side closed first. Refused on Wayland and under `--headless`, naming off-screen views | 2026-10-30 |
| PDF in pages | A page that opens a PDF, or embeds one, shows it as Chrome's viewer does: Chromium's plugin in a dedicated sandboxed renderer, a small viewer of zebkit's, continuous scrolling drawing only visible pages; `navigator.pdfViewerEnabled`, `plugins` and the document as Chrome has them (`docs/API.md`, `tests/engine/pdf_in_page.py`, a 756-page document). Library +0.25% | 2026-11-01 |
| Old Linux | glibc 2.17 and newer (CentOS 7): the one newer symbol, Rust's weak `getrandom`, answered inside the engine; `scripts/package.sh` refuses a package that needs more; the C tests pass on CentOS 7's glibc and libraries (`scripts/glibc-test.sh`) | 2026-10-26 |

## Next

### The library

zebkit fits into the application (owner, 2026-10-25). The application's
loop, its windows and its process are its own: `zebkit_init()`,
`zebkit_pump()`, `zebkit_set_wakeup()` and `zebkit_shutdown()`; the examples
in `examples/native-host` (Done table). What is left:

- **AppKit with a person.** The AppKit example, the macOS startup settings and
  `zebkit_view_attach_native()` on an `NSView` ran on a Mac on 2026-10-31
  (`docs/MACOS.md`). Input from the system itself and input methods are left.
- **Win32 integration.** `zebkit_pump()` under the application's message loop and an
  `HWND` as a view's parent ran on Windows 10 on 2026-11-01 (`docs/WINDOWS.md`);
  Windows' own input in attached views is left.
- **A dedicated engine thread, for loops the engine cannot join.** Chromium's
  UI thread is whichever thread initializes it, and it stays that thread: the
  engine cannot move, and each call that needs the UI thread is made there.
  What that allows differs by system:
  - **Linux: possible today, by the host.** `zebkit_init()` on a thread of the
    host's, which pumps, and `zebkit_post_task()` to reach it
    (`tests/engine/late_load.c` runs it on a worker). The engine has its own X11
    connection, so its windows do not share a thread with the host's toolkit.
    Not done: a convenience for it in the API, and a marshalling of calls; every
    view call has to be posted by the host.
  - **macOS: not possible.** AppKit's windows and events belong to the process's
    main thread, which the toolkit owns, and `zebkit_init()` is for that thread
    only on macOS (`<zebkit/zebkit.h>`). There the only way is to join the loop with `zebkit_pump()`.
  - **Windows: not known.** Nothing has been tried.
- **Views with a window and off-screen views in one process: done on Linux and
  macOS** (2026-11-01). See "Off-screen views" below. Windows: not yet.

### The engine

- **zebkit's own embedder: done** (2026-11-01). The content shell is out of the
  engine: `zebkit::Window` (`window.h`, `window_views.cc`, `window_mac.mm`) for the
  windows, `zebkit::Profile` for the context, the content clients directly on
  content's interfaces (`engine.cc`, `client_defaults.cc`), `zebkit.pak` of the
  engine's own. 19 patches of `content/shell` are gone. What stays copied from it,
  small and now the engine's: the permission and download delegates, the file
  chooser, background sync and accept-language delegates, the protocol handler
  registry's delegate. Left out: test services, DevTools front-end windows, the
  "Inspect Element" menu, crash reporter setup, metrics and variations prefs.
- **A window's native handle** given to the application, so that it can parent
  the system's dialogs to it (file dialogs through the XDG portal need it). The
  other way, an application's handle as a view's parent, is done.
- **A real input method in X11 windows.** None: see `docs/ARCHITECTURE.md`.
- **The sandbox where there is none: answered** (owner, 2026-10-31; `docs/SANDBOX.md`).
  Both: Chromium's set-user-ID helper is in the package, not set-user-ID until root
  runs `zebkit-sandbox-install`, and `--no-sandbox` is documented for containers and said
  on standard error each time. Without any of them the engine refuses to start, saying
  what to do, instead of Chromium's fatal message. Left: the helper has not been run from
  this package, as the build machine has no root.
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
- **CDP checked against Puppeteer and Playwright: done** (2026-11-01). `tests/engine/cdp_clients.mjs`,
  137 cases with real clients, Chrome's results recorded by the same run: `docs/AUTOMATION.md`, section 7.

### Off-screen views

- **Mixed: off-screen views in a process with windows: done on Linux and macOS**
  (`tests/engine/mixed_views.c`). On Linux with a display a view's root has a stub
  platform window and viz draws it in memory (patches/1072, 1073), so no X11 or
  Wayland window exists for it, its `<select>` lists or what its page opens; its
  screen, with its scale and sRGB, is its own per widget (patches/1053, 1074). On
  macOS it is a view in no window with the same screen, per view rather than per
  process. Not done: Windows, which still needs
  `--headless`; Wayland has not been run, nor GPU compositing of such a root on
  Linux (no machine for either here); a window's own input on macOS in the test.
- **Independent scale: Linux foundation implemented.** Separate headless
  displays and root hosts preserve each view's DPR, CSS layout and frame
  pixels, including fractional scales. Pixel/DIP space reservation prevents
  large resizes overlapping other views. Multi-view input/frame regressions,
  the complete Linux/glibc 2.17 suites and Rust integration pass. The Mac
  per-widget screen snapshot path (patch 1053) passes `offscreen_scale.c` on a
  Mac (2026-10-31).
- **viz's own IOSurfaces on macOS.** Frames are read back and copied into
  IOSurfaces of the engine's; viz's blit into mappable shared images
  (`ZEBKIT_BUFFERS_GPU`) gives no frame for a page in no window. With it,
  the read-back and the copy go.
- **GPU validation.** Done on macOS (2026-10-31, Apple M4): hardware compositing
  and WebGL on Metal, attached views drawn with it, IOSurface frames from a page
  composited on the GPU (`tests/engine/gpu_status.py`, `docs/MACOS.md`). Open on
  Linux, where no machine with a GPU exists: dmabufs below, and attached windows
  (`zebkit_view_attach_native()`, only software compositing has drawn in them).
- **dmabufs checked on a GPU.** The path is there (`ZEBKIT_BUFFERS_GPU` on
  Linux, the planes, strides and modifier passed on), but the build
  machine has no GPU: it has only been seen to fall back to shared
  memory.
- **What a window does besides:** drags and drops in and out, the context
  menu (`ShowContextMenu` to the application), touch and accessibility remain.
  Native editing commands now accompany one key via
  `zebkit_view_send_key_with_commands` (C and Rust); the AppKit example
  collects system editing selectors. Word deletion, Unicode command batches
  and `preventDefault()` pass Linux behavior tests, and `edit_commands.c` on a Mac;
  AppKit's selectors from real keys remain.

### Media capture

- **Elements outside the document: done** (2026-10-29). The inventory
  uses Blink's weak document-to-media registry: detached `Audio` objects,
  open/closed and nested shadow roots, stable DOM node IDs without retaining
  unreachable elements. Active captures retain their element. Held by
  `tests/engine/media_inventory.py`; the existing capture suite still passes.
- **One file through a change of resolution: implemented.** Compatible
  H.264/HEVC configurations use in-band parameter sets (`avc3`, `hev1`) in
  one MP4. H.264 up/down changes, AAC preservation, decoding and browser
  replay pass `tests/engine/media_resolution.py`. Incompatible changes and
  explicit Matroska remain separate files. Real HEVC playback and macOS
  runtime validation remain (Linux slim does not enable HEVC decoding).
- **`zebkit_view_media_at()` in frames of other sites: implemented.** Each
  frame is asked in its own coordinates. Its existing implementation and
  cross-site assertion in `tests/engine/media.c` were verified on Linux on
  2026-10-29; the old description of this gap was stale.
- **Live streams' source UTC: implemented.** Native HLS program dates
  and WebM DateUTC are propagated through the demuxer; MSE/DASH applications
  can supply a media/Unix anchor. Piecewise mappings survive buffered date
  discontinuities and closed gaps, in result JSON and container metadata.
  Native HLS, buffered jumps, WebM, explicit anchors, opt-out and unknown
  dates pass on Linux and on a Mac (2026-10-31).

### Agents

- **Hooks for every decision the engine makes: implemented on Linux** (owner,
  2026-10-24; `docs/ROADMAP_HOOKS.md` has each one). Permissions, navigation,
  popups, console, certificates, sign-in, user agent, settings, links to other
  programs, downloads (saved in the profile's `Downloads` without a function),
  isolated worlds, file choosers, camera and microphone, keys, context menu and
  fullscreen, in C, Rust, Python and host JavaScript. Still partial: network
  requests (the headers the network adds are seen, a WebSocket's Cookie too since 2026-11-02;
  `docs/ROADMAP_HOOKS.md`). Client certificates from the system's stores (NSS, Keychain, the
  Windows store) behind the engine setting `clientCertificateStores`, the key staying in the store:
  built 2026-11-03 and tested on Linux, the Mac and Windows (`hook-python-certstore`).
  The hook sources compile for macOS (framework built on 2026-10-31); the suite ran there on 2026-11-01.
- **Input lost after a navigation: found and fixed** (2026-10-31; `docs/AUTOMATION.md` 4.4).
  A new top-level widget takes no input until its first frame: the browser's
  `RenderInputRouter::FilterInputEvent` answers each event itself with
  `kNoConsumerExists`, and the page never hears. Commands wait for that signal (`patches/1062`)
  and a command whose input was dropped anyway fails. The two `tests/engine/hook_policies.py`
  modes that failed had another cause, in the test: two readers on one pipe took the DevTools line
  from each other, so no input was sent. Their retry and the MCP server's wait are gone.
  Unexplained: in an earlier harness, a page stayed at `about:blank` for over ten seconds in 2 of
  40 runs of those modes while the machine was loaded by a build; not seen in 80 runs after; and
  an off-screen view's frame timing out under load was not investigated further.
- **Every setting changeable at runtime: implemented** (2026-10-31). What Chromium's CDP
  changes is checked in `tests/engine/cdp_overrides.py`, by what the page or the server saw.
  What zebkit decides: `zebkit_set_engine_settings()` and `zebkit_view_set_runtime_settings()`,
  in C, Rust, Python and host JavaScript, and the MCP group `settings` (`docs/API.md`,
  `docs/MCP.md`): `thirdPartyCookies`, `spareRenderer`, `dialogs`, `downloads`, `newWindows`.
  Defaults are today's; changing them is an explicit act. A spare renderer kept by
  `spareRenderer` is made again at each load's end, not after each commit as
  `--zebkit-spare-renderer` keeps it, since content fixes that choice at startup. A navigation that needs no
  new process, as to a site a frame of that view has been on, drops the spare at its commit
  (`PrepareForFutureRequests`), as content does while the spare's feature is off.
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
- **PDF in pages: design (2026-11-01).** Chromium's plugin, zebkit's viewer. Chrome's viewer is two
  parts: the plugin (`//pdf`, `PdfViewWebPlugin` and `PDFiumEngine`, which draw only visible pages,
  select, find, follow links, fill forms and print) and an extension (`chrome/browser/resources/pdf`)
  with its scrolling and toolbar, which needs the extensions system and WebUI that `slim` removed. So
  the plugin is Chromium's, in a sandboxed renderer: a dedicated `--pdf-renderer` one without JIT as
  Chrome's, and the browser process never parses a page's PDF (sandboxed in principle: this build machine has no user namespaces and the Mac's seatbelt does not start in the test bundle, `docs/MACOS.md`, so it has been run with `--no-sandbox`); and the part the extension does is a small
  viewer of zebkit's (`src/engine/pdf_viewer.js`, 12 KB of script), in an isolated world and a closed shadow
  root of Chrome's stub document, so the page sees what Chrome's page sees. A viewer of zebkit's over
  `zebkit.pdf` would be smaller by the plugin's code but would parse in the browser process, or need
  PDFium bound into renderers all the same, and would write selection, find, forms and accessibility
  again. Size and what was measured: `docs/SIZE_AND_SPEED.md`.
- **Network quality estimates: implemented.** Chromium's network-service
  estimator now feeds its native tracker and render-process observer, for
  existing and new renderers. Chromium's 3G and Slow-2G test modes pass
  `tests/engine/network_quality.py`, with standard web privacy rounding;
  they pass on a Mac too (2026-10-31); a real network remains.
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
  - **Playwright's own test suite against the endpoint: running, not at parity** (2026-11-01;
    `scripts/playwright-suite.sh`, counts and what is left in `docs/AUTOMATION.md`, section 7).
    2026-11-02: the engine gets Playwright's launch switches from `zebkit-run`, the injected
    script is pinned to the release the endpoint speaks (v1.64.1), and the endpoint gained
    CDP sessions, popups reported as `crPage.ts` reports them, AbortSignal, Playwright's call
    logs, HAR recorded by the endpoint as `harTracer.ts` does, CDP's cookies, request-based
    `networkidle`; counts before and after in section 7. 2026-11-03: 3566 passed, 512 failed
    (234 listed, 69 timeouts, 209 unexplained), from 3233 and 962: tests that launch a browser get
    `zebkit-run`; popups held as Chromium's DevTools holds them; `frameLocator()`; DevTools'
    network details; page errors as CDP gives them; service workers; CORS on fulfilled routes;
    WebSocket routes; IndexedDB in storage state; traces by the endpoint. Chrome: 4196 and 5.
    2026-11-06 (engine with CFI): 3962 passed, 233 failed (52 listed, 43 timeouts, 181
    unexplained): locator handlers, `toHaveScreenshot`, drop and the page's drags, custom selector
    engines, JS/CSS/istanbul coverage, Chromium's own trace, the context's proxy and client
    certificates, WebMCP, passkeys, HAR's WebSockets, ARIA snapshots across frames, traces as
    `tracing.ts` writes them. Left: a service worker's own requests (23), the recorder and the
    debugger (24), trace details (17), client certificates through Playwright's own TLS proxy
    (10), and a long tail (section 7).
    2026-11-06, later: 4005 passed, 190 failed (44 listed, 25 timeouts, 146 unexplained):
    service workers over their own DevTools targets (requests, routes, HAR, offline, console,
    evaluation), fulfilled redirects followed and routed again (`routeFromHAR`), the Debugger
    and `page.pause()`, the trace's "action" snapshot at the input point. Left: workers' own
    targets, the recorder, trace details, client certificates' TLS proxy, CORS preflights
    below the proxy, and a tail (section 7).
    `Disposable.dispose` now removes binding/script registrations and context
    replay entries. Request timing and encoded sizes use Chromium network
    measurements; compressed responses and POST uploads pass client tests.
- **zebkit as an MCP server: built on Linux** (2026-10-31, `docs/MCP.md`).
  One program, `tools/zebkit-mcp`,
  that gives an AI agent the engine's capabilities as MCP tools, over
  standard input and over HTTP, on Linux and macOS alike. CDP stays for
  clients that speak it; MCP is what agents are given. 68 tools, the protocol's
  current revision and the handshake ones; a program of its own in Rust, about
  1 MiB, outside the library. An outside agent (Claude Code, headless, this server its
  only tools) did five real tasks on the open web on 2026-11-02 (a form, research on three
  sites, a 15-page PDF, a site's own search, a public Google Doc): all done; what it hit was
  fixed the same day (stale references failing at once, actions answering after their
  navigation, `page_read`'s `find`, a clearer missing-directory error; `docs/MCP.md`). Pages
  drawn on a canvas have coordinates (`page_mouse`, `page_keyboard`, a capture at one pixel per
  CSS pixel). Still to do: running on macOS
  and Windows' standard input; the device flow against the real providers,
  and refresh tokens. The engine fixes it needed were made on 2026-10-31 (`docs/MCP.md`).
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
  - **Sign-in to a provider's API without a screen:** OAuth's device flow
    (RFC 8628) as a tool of the server, for its own tools that need a
    Google, Microsoft or GitHub token: the code and URL shown to the
    person, approved on any device. A tool of the server only (owner,
    2026-10-28), not an API of the engine: it is three HTTPS requests any
    host can make itself, and it gives a token, not a signed-in page.
  - **Checked by:** an MCP client's own test suite and an agent doing
    real tasks (research on the web, filling a form, reading a long PDF),
    with every tool covered by `tests/engine/`. Today: the official client
    over both transports with every tool and bounds' escapes
    (`mcp_server.mjs`), four scenarios of the project's conformance suite
    (`mcp_conformance.mjs`), and those tasks as a scripted run on a local site,
    no model deciding (`mcp_agent_run.mjs`), and an outside agent's runs on the open web
    (2026-11-02, by hand: they cost a model's tokens). Not yet a client of the current
    revision: no SDK speaks it yet.
### Measurements

- **Web pages against Electron 44.9: done for 14 pages, one left over**
  (`scripts/web-bench.sh`; Electron 45.2.1 in `scripts/electron-compare.sh`).
  `slim` costs 16% less CPU and 16% less memory on a page, over 14 of the
  15 pages of `scripts/web-bench.sh --sites`, both engines in a 1280 by
  800 window (`docs/SIZE_AND_SPEED.md`); less memory on every page, less
  CPU on 12, level on BBC News and Stack Overflow.
  - **TODO: Reddit** showed zebkit a reCAPTCHA in place of the page, and
    Electron a challenge it passed; until both are shown the page, it is
    out of the comparison.
- **A fully equal comparison with Electron: done for thin hosts, Linux and
  macOS, 2026-11-02** (`scripts/electron-compare.sh`, `docs/SIZE_AND_SPEED.md`,
  "Against Electron, measured the same way"): Electron 45.2.1, one harness,
  the same pages, two thin hosts, every process counted. zebkit wins size,
  startup, memory, CPU while loading and, since 2026-11-02, calls between
  the page and its host (level to 64 KiB, a fifth faster from 1 MiB, over
  each frame's Mojo channel); it loses the DOM workload (25%), text layout
  (9%) and the long document's first layout (16 to 19%), which is how the
  code is compiled, and runs one process more (storage, as Chrome).
  - **Done 2026-11-02: calls between the page and the host** as fast as
    Electron's IPC, and faster for large ones.
  - **TODO: the DOM and layout gap**, what the size rule lets the build
    close: initial-exec thread-locals measured on the DOM, Chrome's
    compiler settings for more of Blink one directory at a time.
  - **TODO: a headless capture one frame later than Chrome's** over CDP
    (`Page.captureScreenshot`, JPEG too).
  - **TODO: what is left of the conditions below**: one real application
    on both hosts, an EPUB, the sandbox on (this machine allows neither
    engine's), cold start on macOS.
- **The conditions of that comparison.** The comparison of
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

- **Windows**: `zebkit.dll`, sandbox on by default and pages checked by their pixels on
  a Windows 10 machine (`docs/WINDOWS.md`). Left: the test suite and bindings, D3D11
  textures for off-screen frames, system interfaces of WebAuthn, printing and `zebkit.ui`.
- **macOS**: what a person at the Mac has to check (`docs/MACOS.md`: real input and
  devices, the real Keychain, WebRTC's peers, drops, the menu bar's keys); Chromium's PGO profile for macOS;
  signing with a Developer ID and notarization.

### Releases

- The latest engine, 0.1.0 (with off-screen views, dialogs, printing to PDF, drag and drop, sign-in popups, media capture, persistent cookies, glibc 2.17, passkeys, Playwright's API natively and its own endpoint, Windows with its sandbox; `docs/releases/0.1.0.md`), is published for Linux, macOS and Windows;
  the Rust crates download a release only from a repository anyone can
  read, so the repository has to be public for them.

### The API

- **Annotations as a full PDF editor has them: what is left.** Implemented
  (2026-10-30, in `<zebkit/pdf.h>`, `zebkit.pdf`, Rust and Python; `docs/API.md`).
  Not done:
  - Checked against Acrobat's and PDF.js's rendering of the same file: not done.
    Appearances are checked in PDFium's pixels, and looked at once in poppler
    and Ghostscript (the Done row says what).
  - XFDF and FDF carry no appearances and no embedded files: an image stamp, a
    stamp from a page and a file attachment's file do not travel; what reads the
    file draws the generated ones. XML rich text in XFDF is written only when it
    is well formed.
  - Standard stamps are drawn by zebkit (a frame and the name), not Acrobat's
    artwork. A rotation is of generated appearances; text markup and ink are not
    turned. Rich text covers paragraphs, spans, colour, size, family, bold,
    italic, underline and alignment, not all of the XHTML subset the reference has.
  - Fonts: without one given for it, text outside WinAnsi comes out as question
    marks. Free text's own glyphs are not shaped (no ligatures, no right-to-left);
    `<zebkit/text.h>` and a canvas appearance are.
- **PDF extensible by applications: what is left.** Implemented (2026-10-30,
  `zebkit_pdf_hook_*`, Rust, Python, `zebkit.pdf.use()`; `docs/API.md`,
  `examples/pdf-plugin/`). Not done:
  - The canvas an appearance is drawn on is not Skia written into the stream: it
    writes PDF operators through the same `<zebkit/canvas.h>` calls, because Skia
    has no way to write one form XObject (its PDF backend makes whole documents)
    and pixels would not be vectors. Text is outlines; gradients are opaque
    shadings, their stops' alpha not used; strokes do not take gradients.
  - In JavaScript, plugins run on the script's own thread around the calls it
    makes: what the engine does inside one call (an annotation added by an import)
    is told to `after` hooks, but is not asked of `before` hooks.
  - `zebkit_pdf_annotation_copy()`, a stamp from a page and attaching a file are
    not events. Page deletion, redaction, flatten and page import have no inverse;
    a move of several pages has none.
- **PDF, beyond annotations**: trust in a signer's certificate (the chain,
  revocation), encrypted documents saved with new permissions, form fields
  renamed, a page's JavaScript actions. Outline replacement now supports
  hierarchy, page positions and URLs through C/Rust/Python/host JavaScript;
  atomic validation and full/incremental-save regressions pass on Linux. PDFium in
  a sandboxed utility process for pages' documents, as Chrome's printing
  runs it.
- **Services, later areas**: decoding media frames; the desktop's clipboard,
  notifications, file dialogs, screens, power and keychain. 2D drawing
  (`<zebkit/canvas.h>`) and shaping a line of text (`<zebkit/text.h>`) are
  implemented. Left of them: colour fonts draw nothing in a canvas (outlines only;
  HarfBuzz's own drawing is compiled out of the engine, so glyphs come from Skia),
  no image filters or text decoration, and `zebkit.canvas` keeps its operations
  in the script until pixels are asked for.
- **LevelDB export: implemented** (2026-10-30). The engine's own LevelDB,
  exported through its C API as `zebkit_vendor_leveldb_*` in `leveldb/c.h`,
  like the other libraries, with no key-value API of zebkit's beside it.
  Non-ASCII paths, compaction without the engine's thread pool, binary values,
  snapshots, batches and iterators pass in `tests/engine/vendor.c`.
- **FreeType export: implemented** (2026-10-29). The engine's 189 public
  compiled functions, package-relative macro headers and actual configuration;
  C/C++ initialization, outline rasterization and TrueType glyph loading pass
  on Linux and glibc 2.17. macOS framework packaging and C/C++ ARM64
  cross-linking pass, and `vendor.c` on a Mac (2026-10-31).
- **Bindings** generated from the headers for more languages: Go, C#,
  Node's N-API.
- **Loading after threads have started: implemented on Linux** (2026-10-29).
  Patch 1045 removes all deferred TLS pointer initializers. ELF audit, C/Python
  main/worker loops and the glibc 2.17 C regressions pass. Native unloading is
  not supported; macOS late loading remains to be validated.

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
