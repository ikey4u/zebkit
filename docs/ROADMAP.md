# Roadmap

Where zebkit stands and what comes next. How the engine is made is
`docs/ARCHITECTURE.md`; its size and speed, `docs/SIZE_AND_SPEED.md`;
what `zebkit.ui` is for, `docs/UI.md`. How an application decides what
the engine does, through hooks rather than patches, is `docs/HOOKS.md`.

## Requirements

zebkit is a library that applications ship and integrate, agents among
them: an application keeps its own process, main loop, windows and
toolkit, and calls zebkit where it wants a web page, a PDF, media or
the window, prompts and dialogs around a page (`zebkit.ui`, `docs/UI.md`). It is not a browser, and not a base that applications
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
| A library that applications integrate, not a base they are built on: the application owns its process, loop, windows, menus and identity; `zebkit.ui` is only the window, prompts and dialogs around a page, for programs without a UI of their own and for the engine's defaults (`docs/UI.md`) | Owner, 2026-10-25 |
| Real web pages cost less than in the latest stable Electron, and less still over time: CPU while loading and at rest, memory, processes. A medium page in an application is not a browser's workload | Owner, 2026-10-23 |
| Off-screen views: frames as shared surfaces (IOSurfaces, shared memory, dmabufs) with their damage, input sent by the application, popups drawn off-screen, frames passable to another process | Owner, 2026-10-23 |

Kept by zebkit's own choice, open to the owner's overrule: WebGL,
WebAssembly, HTTP/3 and SwiftShader. Camera and microphone are a page's by
permission, and WebUSB, WebHID and Web Serial through the application's
device chooser (0.4.0); Web Bluetooth and physical printing are not in the
engine (`docs/SYSTEM_INTEGRATION.md`, section 19).

## Done

| | What | When |
|---|---|---|
| Engine | Chromium's content layer (157 since 2026-11-07) as `libzebkit.so.1` and `zebkit-helper`, started by the application (`zebkit_main`) | 2026-10-20 |
| Size | 214.8 MiB to 134.8 MiB with no web feature lost; then 157.3 MiB for speed | 2026-10-17 to 2026-10-21 |
| Speed | Chromium's PGO profile, Blink's layout code at `-O2`, link-time optimization: text layout 13.2 to 8.7 ms (Chrome's own build: 6.8) | 2026-10-21 |
| Libraries | 17 C libraries inside the engine exported under `zebkit_vendor_*`, about 7,490 functions on each of Linux, macOS and Windows (7,487 on all three; FreeType's BDF and synthesis on every system since `patches/1080`), found in the build | 2026-10-20, FreeType and LevelDB 2026-10-29, 2026-10-30 |
| Services | Compression, cryptography, URLs, images, ICU's data, HTTP through the network stack | 2026-10-20 |
| Bindings | Rust (`zebkit`, `zebkit-rs`, `zebkit-build`), Python (`ctypes`); Go (`bindings/golang`, cgo) on Linux, macOS and Windows; Rust, Python, Go and host JavaScript each reach all of the C API (`scripts/check-bindings.py`) | 2026-10-20, Go 2026-11-09, all of the API 2026-11-10 |
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
| Windows | `zebkit.dll`, sandboxed by default with the application's executable as the children; the engine tests pass on Windows 10 with the Rust, Python and Go bindings (`docs/WINDOWS.md`, `docs/releases/0.4.0.md`) | 2026-11-02, 0.4.0 |
| System integration | The window's frame, menus, dialogs, device choosers, screen capture, notifications, the tray, the Dock and taskbar, sharing, the system's theme, power and displays, each the application's to decide (`docs/SYSTEM_INTEGRATION.md`) | 0.4.0, 2026-11-08 |
| MCP server | `tools/zebkit-mcp`, on Linux, macOS and Windows, in every release archive (`docs/MCP.md`) | 2026-10-31; on every system since 0.3.0 |
| Late loading | The library loaded after the host's threads have started: Linux (`patches/1045`), and on a Mac on the main thread (`docs/MACOS.md`) | 2026-10-29, macOS 2026-10-31 |
| Hooks | Every decision the engine makes, in C and every binding, on Linux, macOS and Windows (`docs/HOOKS.md`) | 2026-10-24 to 2026-11-03 |
| Input methods in X11 windows | IBus (ibus-daemon, fcitx5's IBus frontend), composition in the page (`docs/ARCHITECTURE.md`, `tests/engine/input_method.py`) | 2026-11-10 |
| Own embedder | The content shell out of the engine: `zebkit::Window` (`window.h`, `window_views.cc`, `window_mac.mm`), `zebkit::Profile`, the content clients directly on content's interfaces (`engine.cc`, `client_defaults.cc`), a `zebkit.pak` of the engine's own; 19 patches of `content/shell` gone | 2026-11-01 |
| A window's native handle | `zebkit_view_get_native_window()` (an X11 window, an `HWND`, an `NSWindow*`) for parenting the system's dialogs, and `zebkit_view_export_native_window()` as a portal names its parent; in every binding | 2026-11-07 |
| CDP against real clients | `tests/engine/cdp_clients.mjs`: 137 cases with Puppeteer and Playwright, Chrome's results recorded by the same run (`docs/AUTOMATION.md`, section 7) | 2026-11-01 |
| Off-screen views beside windows | On Linux and macOS in one process (`tests/engine/mixed_views.c`); each view's scale its own (`tests/engine/offscreen_scale.c`) | 2026-11-01 |
| More of media capture | Elements outside the document (`media_inventory.py`), one file through a change of resolution (`media_resolution.py`), frames of other sites, live streams' source UTC | 2026-10-29 to 2026-10-31 |
| Input after a navigation | A command waits for a new widget's first frame, and fails if its input was dropped anyway (`patches/1062`, `docs/AUTOMATION.md` 4.4) | 2026-10-31 |
| Settings at runtime | `zebkit_set_engine_settings()`, `zebkit_view_set_runtime_settings()`, the MCP group `settings`; what CDP changes is checked in `cdp_overrides.py` | 2026-10-31 |
| Chrome as sites see it | `window.chrome` by Chrome's code, `chrome.app`'s queries, popups blocked without a click, a real screen under `--headless`, V8's inspector running none of the page's code (`patches/1026`, `cdp_browser.py`) | 2026-10-24, 2026-10-26 |
| Network quality | Chromium's network estimator feeding renderers; 3G and Slow-2G test modes (`network_quality.py`) | 2026-10-31 |
| Playwright's API natively | `<zebkit/automation.h>` and Playwright's protocol over a WebSocket (`zebkit_automation_listen`, `zebkit-run --playwright-port`), in every binding (`docs/AUTOMATION.md`) | 2026-10-27 |
| Find in page, download outcomes | `zebkit_view_find()` on Chrome's own find, and each download's outcome and error (`docs/API.md`, `docs/HOOKS.md`) | 2026-11-11 |

How some of the finished items were made, where that is not written
elsewhere:

- **PDF in pages: Chromium's plugin, zebkit's viewer.** Chrome's viewer is two
  parts: the plugin (`//pdf`, `PdfViewWebPlugin` and `PDFiumEngine`, which draw only visible pages,
  select, find, follow links, fill forms and print) and an extension (`chrome/browser/resources/pdf`)
  with its scrolling and toolbar, which needs the extensions system and WebUI that `slim` removed. So
  the plugin is Chromium's, in a sandboxed renderer: a dedicated `--pdf-renderer` one without JIT as
  Chrome's, and the browser process never parses a page's PDF (sandboxed: the release archive runs it
  in the seatbelt on macOS, checked 2026-11-09; the suite runs it with `--no-sandbox`, as this build
  machine has no user namespaces and the Mac's test bundle does not enter the seatbelt,
  `docs/MACOS.md`); and the part the extension does is a small viewer of zebkit's
  (`src/engine/pdf_viewer.js`, 12 KB of script), in an isolated world and a closed shadow
  root of Chrome's stub document, so the page sees what Chrome's page sees. A viewer of zebkit's over
  `zebkit.pdf` would be smaller by the plugin's code but would parse in the browser process, or need
  PDFium bound into renderers all the same, and would write selection, find, forms and accessibility
  again. Size and what was measured: `docs/SIZE_AND_SPEED.md`.
- **Input lost after a navigation.** A new top-level widget takes no input until its first frame: the
  browser's `RenderInputRouter::FilterInputEvent` answers each event itself with `kNoConsumerExists`,
  and the page never hears. Commands wait for that signal (`patches/1062`). The two
  `tests/engine/hook_policies.py` modes that failed had another cause, in the test: two readers on one
  pipe took the DevTools line from each other, so no input was sent. Unexplained: in an earlier
  harness, a page stayed at `about:blank` for over ten seconds in 2 of 40 runs of those modes while the
  machine was loaded by a build; not seen in 80 runs after.
- **A spare renderer at runtime.** `spareRenderer` is made again at each load's end, not after each
  commit as `--zebkit-spare-renderer` keeps it, since content fixes that choice at startup. A
  navigation that needs no new process drops the spare at its commit (`PrepareForFutureRequests`), as
  content does while the spare's feature is off.
- **Legacy `chrome.app` queries** (2026-10-26), for the uninstalled hosted-app state, without an
  extension runtime. The same slim binary passed Google's email/Next check with these queries,
  was rejected without them, and passed again after restoring them
  ([the investigation](notes/google-signin.md)).
- **The sandbox where there is none** (owner, 2026-10-31; `docs/SANDBOX.md`). Chromium's
  set-user-ID helper is in the package, not set-user-ID until root runs `zebkit-sandbox-install`,
  and `--no-sandbox` is documented for containers and said on standard error each time. Without
  any of them the engine refuses to start, saying what to do, instead of Chromium's fatal message.
- **Text layout.** Level with Electron 45.2.1 within noise since 2026-11-06: 7.5 against 7.4 ms
  on Chromium 157, 2026-11-08 (`docs/reports/electron-compare.md`). Before, on Chromium 155
  (2026-10-27), a chapter of a book took 4.4 ms of CPU to lay out in zebkit `slim` and 3.5 ms in
  Electron 44.9 (`scripts/layout-bench.sh`, `docs/SIZE_AND_SPEED.md`): the engines did the same
  work, and `slim`'s code ran slower for being compiled for size. `slim-speed` (`-O2` throughout,
  no size-trained inliner) took 3.9 ms for 30 MiB more and 20 to 50 MB more a page. The release
  stays `slim` (owner, 2026-10-27). What closed the gap was Chrome's PGO profile matched
  (2026-11-06: CFI on virtual and indirect calls, `patches/1078` keeping the checks off the
  application's functions, and the size model kept to what the profile calls cold below 99.9%,
  for 5.1% of the package).

## Next

Open work only; what is finished is in the table above.

### The library

zebkit fits into the application (owner, 2026-10-25): the application's loop,
its windows and its process are its own.

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
- **Native unloading** of the library is not supported.

### The engine

- **Wayland** (`--ozone-platform=wayland`; X11 is the default): windows and the
  portal export run in a headless labwc (2026-11-07). Not done: off-screen
  views beside Wayland windows (refused, `zebkit_view_new_offscreen()` returns
  NULL: their root's stub platform window crashed there), attaching to an
  application's window, input methods; not run with a person or a GPU.
- **Input methods in X11 windows:** XIM-only input methods (fcitx 4 without an
  IBus frontend) are left; IBus is done (Done table).
- **The set-user-ID sandbox helper** has not been run from this package, as the
  build machine has no root.

### Off-screen views

- **Beside windows:** not done on Windows, which still needs `--headless` for
  off-screen views; on Wayland (above); GPU compositing of such a root on
  Linux (no GPU here); a window's own input on macOS in the test. On Linux with
  a display a view's root has a stub platform window and viz draws it in memory
  (`patches/1072`, `1073`); its screen, with its scale and sRGB, is its own per
  widget (`patches/1053`, `1074`).
- **viz's own IOSurfaces on macOS.** Frames are read back and copied into
  IOSurfaces of the engine's; viz's blit into mappable shared images
  (`ZEBKIT_BUFFERS_GPU`) gives no frame for a page in no window. With it,
  the read-back and the copy go.
- **GPU validation on Linux.** Done on macOS (2026-10-31, Apple M4): hardware
  compositing and WebGL on Metal, attached views drawn with it, IOSurface frames
  from a page composited on the GPU (`tests/engine/gpu_status.py`,
  `docs/MACOS.md`). Open on Linux, where no machine with a GPU exists: dmabufs
  (`ZEBKIT_BUFFERS_GPU`, the planes, strides and modifier passed on; only seen to
  fall back to shared memory) and attached windows (`zebkit_view_attach_native()`,
  only software compositing has drawn in them).
- **What a window does besides:** drags and drops in and out, touch and
  accessibility remain. Native editing commands accompany one key via
  `zebkit_view_send_key_with_commands`; word deletion, Unicode command batches
  and `preventDefault()` pass Linux behaviour tests, and `edit_commands.c` on a Mac;
  AppKit's selectors from real keys remain.

### Media capture

- Real HEVC playback, and a change of resolution checked at run time on macOS
  (Linux `slim` does not enable HEVC decoding). Incompatible changes of
  configuration and explicit Matroska still give separate files.

### Agents

- **Network hooks, still partial**: what is open is listed under the hook
  (`docs/HOOKS.md`), among it early hints, prefetch and background fetch, which
  do not pass the hook.
- **An off-screen view's frame timing out under load** was seen and not
  investigated further.
- **Playwright's own test suite against the endpoint: not at parity**
  (`scripts/playwright-suite.sh`): 4047 passed and 148 failed on Chromium 157 (2026-11-08),
  Chrome 4196; each run's counts and what is left are in `docs/AUTOMATION.md`, section 7.
- **The MCP server** (`docs/MCP.md`): the device flow against the real providers,
  refresh tokens, and a client of the protocol's current revision (no SDK speaks
  it yet).
- **The privacy defaults tested on every change**: browserleaks.com's
  checks (WebRTC, permissions, client hints, TLS, JavaScript) in
  `scripts/web-bench.sh`, against what zebkit's defaults promise; a real
  network for the network quality estimates.

### Measurements

- **The comparison with Electron** (`scripts/electron-compare.sh`, the current
  numbers in `docs/reports/electron-compare.md`: Electron 45.2.1 against zebkit
  on Chromium 157.0.8103.38 as released in 0.3.0, 2026-11-08). Left of it:
  - **A headless capture one frame later than Chrome's** over CDP
    (`Page.captureScreenshot`, JPEG too).
  - **Conditions not met yet**: one real application on both hosts, an EPUB,
    the sandbox on (this machine allows neither engine's), cold start on macOS.
  - **Reddit** showed zebkit a reCAPTCHA in place of the page in the web pages
    comparison (`scripts/web-bench.sh`), and Electron a challenge it passed; until
    both are shown the page, it is out of the comparison.
- **The conditions of a comparison of whole applications.** The comparison of
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
  - **Linux, macOS and Windows**, each on its own machine.
  - **Repeatable:** the application, both hosts and the harness in
    `tests/bench/`, one command to run it; the results with the date, the
    versions and the machine in `docs/reports/`, losses shown as
    plainly as wins.

### Platforms

- **Windows**: D3D11 textures for off-screen frames on a real GPU, Windows' own
  WebAuthn route, `zebkit.ui` beyond its script's test, off-screen views beside
  windows (Windows still needs `--headless` for them).
- **macOS**: what a person at the Mac has to check (`docs/MACOS.md`: real input and
  devices, the real Keychain, WebRTC's peers, drops, the menu bar's keys); Chromium's PGO profile for macOS;
  signing with a Developer ID and notarization.

### Releases

- The latest release is 0.4.0 (`docs/releases/0.4.0.md`), for Linux, macOS and
  Windows; what is on `main` since is in `docs/releases/next.md`, the draft of
  the next release's notes. The Rust crates download a release only from a
  repository anyone can read.

### The API

- **Annotations as a full PDF editor has them: what is left.** Implemented
  (2026-10-30, in `<zebkit/pdf.h>`, `zebkit.pdf` and every binding; `docs/API.md`).
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
  `zebkit_pdf_hook_*`, every binding, `zebkit.pdf.use()`; `docs/API.md`,
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
  renamed, a page's JavaScript actions. PDFium in a sandboxed utility process
  for pages' documents, as Chrome's printing runs it.
- **Services, later areas**: decoding media frames; the desktop's clipboard
  and keychain as services of their own. Left of `<zebkit/canvas.h>` and
  `<zebkit/text.h>`: colour fonts draw nothing in a canvas (outlines only;
  HarfBuzz's own drawing is compiled out of the engine, so glyphs come from Skia),
  no image filters or text decoration, and `zebkit.canvas` keeps its operations
  in the script until pixels are asked for.
- **Bindings** for more languages: C#, Node's N-API.

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

- **Builds.** The checkout and a build take about 120 GB of disk; a full build takes hours, and a
  change of compiler flags rebuilds everything. Changes are batched into
  one rebuild where possible.
- **Patches.** Chromium releases every four weeks, and every patch has to
  apply to each release. Switches are preferred to patches.
- **Security.** Fixes come from upstream continually; zebkit has to follow
  stable releases closely, another reason to keep the patch set small.
  How to report a vulnerability, and which versions get fixes: `SECURITY.md`.
