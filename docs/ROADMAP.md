# Roadmap

Where zebkit stands and what comes next. How the engine is made is
`docs/ARCHITECTURE.md`; its size and speed, `docs/SIZE_AND_SPEED.md`. The
UI layer, applications in React, Solid or Vue drawn natively with each
system's look, and iOS, has its own roadmap: `docs/ROADMAP_UI.md`.

## Requirements

zebkit is an engine to embed in applications, agents among them, that
drive it over the Chrome DevTools Protocol. It is not a browser and not an
application framework.

| Requirement | From |
|---|---|
| HTML, CSS and JavaScript as in Chrome. Features beyond that core (WebRTC, WebGPU, WebXR, WebNN, device APIs) may go | Owner, 2026-10-17 |
| CDP, compatible with Puppeteer, Playwright and raw clients | Owner |
| Media playback, H.264 and AAC included | Owner, 2026-10-10 |
| The JIT stays; no cut that costs much speed for little size | Owner, 2026-10-09 |
| ICU's data for English, Chinese, Japanese and Korean at least | Owner |
| Linux on old systems: glibc 2.31 first, then 2.17 | Owner, 2026-10-17 |
| The same functions on Linux, macOS and Windows | Owner |
| Far smaller than Electron, uncompressed and compressed. The 150 MiB limit of 2026-10-19 gave way to speed on 2026-10-21 | Owner, 2026-10-18 to 2026-10-21 |
| Chromium's libraries, and what else in it is useful, exported for applications as a C API, with bindings and a JavaScript API | Owner, 2026-10-19 |
| A full PDF API on Chromium's PDFium, with EmbedPDF's features as the reference | Owner, 2026-10-20 |

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
| An application | A document reader moved from Electron to zebkit. Against its Electron version on Linux, medians of five runs: start 0.50 s (1.08), idle CPU 0.35% (51.5%), idle memory 290 MB (958); a 756-page PDF's first page 1.1 s (5.3), its longest freeze 0.10 s (2.9), its scrolling frames 22 ms at the median (170) | 2026-10-22 |

## Next

### The engine

- **zebkit's own embedder.** The engine still runs on the content shell's
  browser classes for windows and the browser context, which Chromium
  keeps for tests. Its own `ContentBrowserClient`, window and context,
  started from the shell without its test hooks; Electron's
  `native_window_views.cc` is the reference for the windows.
- **A window's native handle**, so that an application can parent the
  system's dialogs to it (file dialogs through the XDG portal need
  it).
- **Linux on older systems.** The package runs on glibc 2.25 and newer; the
  requirement is 2.17. What is left is building on an older sysroot and
  replacing what newer symbols the engine takes.
- **The sandbox where there is none.** Chromium's Linux sandbox needs
  unprivileged user namespaces or a set-user-ID helper, which containers
  and build machines often lack; the engine then refuses to start without
  `--no-sandbox`. Agents run in such places: zebkit needs a stated answer
  (ship the helper, document `--no-sandbox` for containers, or both).
- **Text layout at Chrome's speed.** An EPUB chapter takes 8.7 ms in
  zebkit and 6.8 ms in Electron 44.9, Chrome's own build
  (`docs/SIZE_AND_SPEED.md`). What is left at `-Os` for size, and the
  inliner trained for size at the link (8.2 ms without it, 14 MiB more),
  are where to look; every comparison is with the latest stable Electron.
- **The application measured against the latest Electron.** The document
  reader's comparison (`README.md`) is against its build on Electron 36;
  it is to be measured again with its Electron build moved to the latest
  stable release.
- **CDP checked against Puppeteer and Playwright**, with Chrome's
  results as the reference: the engine serves Chromium's own DevTools
  endpoint, and a suite of client tests against it is still to be written.

### Platforms

- **Windows**: the same profile and the same API.
- **macOS**: real windows checked with a person at the Mac (attached
  views, drops, the menu bar's keys); Chromium's PGO profile for macOS;
  signing with a Developer ID and notarization.

### Releases

- The first Blink engine, 0.0.1-alpha.4, is published for Linux and macOS;
  the Rust crates download a release only from a repository anyone can
  read, so the repository has to be public for them.

### The API

- **PDF**: trust in a signer's certificate (the chain, revocation), editing
  the outline, encrypted documents saved with new permissions, appearances
  for stamps, carets and redaction marks, form fields renamed, a page's
  JavaScript actions, flattening or exporting one annotation, stamps made
  from another document's pages. PDFium in a sandboxed utility process for
  pages' documents, as Chrome's printing runs it.
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
