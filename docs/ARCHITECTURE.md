# Architecture

How zebkit is made: what runs where, and which part of this repository
does it. The API itself is `docs/API.md`.

![zebkit's architecture](images/architecture.svg)

(`scripts/architecture-svg.py` draws the picture.)

## The engine as a library

Chromium's content layer is the multi-process browser without Chrome:
navigation, the renderer processes, the GPU process, the network service,
Mojo IPC and the DevTools backend, with Blink and V8 in the renderers.
zebkit builds it as a library the application links, `libzebkit.so.1` on
Linux and `Zebkit.framework` on macOS, and a helper program,
`zebkit-helper` (`Zebkit Helper.app`), that the engine starts for every
child process. The application's own executable is never re-run as a
renderer, as Electron's is.

```
application process                          child processes (zebkit-helper)
+---------------------------------------+    +-----------------------------+
| the application's code                |    | renderer: Blink, V8         |
|   zebkit_main(), views, callbacks     |    |   the page, zebkit.* object |
| libzebkit: content's browser process  |<-->| GPU: viz, ANGLE, SwiftShader|
|   views and windows, navigation,      |Mojo| utility: network, audio,    |
|   DevTools server, schemes, PDFium,   |    |   storage                   |
|   zebkit.* answers, zebkit.ui (views) |    +-----------------------------+
|   V8 for zebkit-js (script outside)   |
+---------------------------------------+
```

- **Starting.** `zebkit_main()` starts the content layer as Android
  WebView does: the embedder initializes `BrowserMainRunner` and runs its
  loop, and calls the application's `ready` once views can be made. It
  returns after `zebkit_quit()`. Chromium's switches in `argv` work as for
  Chrome. The application's thread is the browser's UI thread: every
  callback runs there, and `zebkit_post_task()` reaches it from any thread.
- **Windows.** Views are the content shell's windows for now (Aura and
  Views on Linux, Cocoa on macOS): titles, sizes and limits, and views
  attached inside another view's window, as a browser inside an
  application. Without a display, or with `--headless`, a view has no
  window and its page still loads, runs and draws (`zebkit_view_capture`).
- **The allocator.** A library must not take over the application's
  `malloc`, so PartitionAlloc does not (`use_partition_alloc_as_malloc =
  false`); Blink and V8 keep their own heaps, with the thread-local model
  a shared library needs (`patches/1011`).
- **Relocations.** The engine applies its own packed relocations when it
  is loaded (`patches/1005`), so that they work on glibc older than 2.36.
- **What it exports.** zebkit's own functions and the libraries' entry
  points under `zebkit_vendor_*`, nothing else (`src/engine/zebkit.ver`;
  `scripts/engine-test.sh` checks).

## A page's calls to the application

A page the application grants it to gets a global `zebkit`. The object is
made by a script the view's internal DevTools session adds before the
page's own (`Page.addScriptToEvaluateOnNewDocument`). A call is a `POST`
from the page to `zebkit://call`, a scheme of the engine's own that every
frame's URL loader factory serves for that frame alone:

```
page script                renderer               browser process
zebkit.pdf.render(...) --> fetch POST zebkit://call --> the frame's factory (call.cc)
                           one frame of bytes:          knows the frame, so its origin
                           u32 length, JSON, parts      checks the grant (view.cc)
                                                        does the work (bridge.cc,
                                                        pdf_bridge.cc) on the thread pool
<-- Uint8Array, JSON <---- response body, Mojo data pipe <--
```

- **The origin is the browser's**, from the factory of the frame that
  called, never what the page's script says. Frames of another site are
  other DevTools targets, which the view's session does not attach to, and
  never get the object.
- **Bytes are bytes.** A frame is a 32-bit length, JSON, then binary parts
  the JSON points to (`{"$part": [offset, size]}`), through Mojo's data
  pipes: no base64, and many calls under way at once.
- **PDF work is ordered by urgency.** A page's PDF calls go to one sequence
  of the thread pool with three levels: rendering and hit tests first,
  reads of one page next, bulk reads (a whole document's text) last, so a
  reader's visible page is never behind the indexing of the rest.
- **The renderer stays sandboxed.** Files, PDFs, compression and the
  network are reached in the browser process, and only what the grant
  allows.

`zebkit.app.<name>()` calls the application's own function
(`zebkit_view_bind`), with JSON arguments and result.

## URL schemes of the application's own

`zebkit_scheme_register()` makes a scheme known to every process (as
standard, secure, open to `fetch()`, past pages' CSP, as its flags say), and
`zebkit_scheme_handle()` answers it. It is Chromium's way for its own
schemes, a URL loader factory per scheme in the browser process
(`scheme.cc`, modelled on `content/browser/data_url_loader_factory.cc`), and
what Electron calls `protocol.handle`. A file is answered as it is read, in
the range a `Range` header asks for, so `<video>` seeks in local files
without a server.

## The libraries inside the engine

Chromium builds zlib, Brotli, Zstandard, BoringSSL, SQLite, ICU, HarfBuzz,
libxml2, libwebp, libjpeg-turbo, FFmpeg, dav1d, Opus, libvpx and PDFium
into the engine. `src/engine/vendor/gen_vendor.py` runs at the end of the
build: a function is exposed when the library's public headers declare it
and the library's compiled objects define it. It writes an entry point
`zebkit_vendor_<library>_<function>` for each, and a header per library
that maps the library's own names to them, so another copy of the same
library in the process (the system's, the application's) is a different one
and does no harm. Chromium's own renames (zlib's `Cr_z_`, SQLite's
`chrome_sqlite3_`, ICU's version suffix) are followed. C++ libraries
(Skia, V8, `net`) are not exported: they are compiled against Chromium's
own libc++, whose types an application cannot share, and are reached
through the services instead.

FFmpeg is built with zebkit's configuration (`src/ffmpeg`, generated by
`scripts/ffmpeg-config.sh` with Chromium's own scripts, `patches/1017`):
Chrome's demuxers and decoders, plus libswresample, libswscale, the file
protocol and the MP4, MOV, Matroska and WebM muxers that applications need.

## PDF

PDFium is in the engine at the revision Chromium pins, without XFA and
without its own V8. `pdf.cc` is `<zebkit/pdf.h>` over PDFium's public API;
`pdf_internal.cc` reaches PDFium's classes where the public API does not
(an annotation's dictionary entries, nested signature fields), and
`patches/1014`, `1015` and `1020` add to PDFium what it lacked. Signatures
are checked with BoringSSL: the bytes a signature covers, its signed
attributes, and the signer's key. PDFium is not thread-safe, so every call
takes one lock.

## Script outside any page, and `zebkit.ui`

`zebkit_script_new()` makes a V8 isolate in the application's process, on
the engine's thread, with the whole of `zebkit.*` and `zebkit.View`
(`script.cc`); `tools/zebkit-js` runs an ES module with it, as Node runs
one. `zebkit.ui` builds windows of Chromium's views from that script
(`ui.cc`, `ui_script.h`): components run once, signals update the
properties that read them, and changes reach the native side as one batch
of mount operations per turn of the loop, as in Native Shell and Solid.
Layout is views' box layout (a direction, gap, padding, alignment, grow); a
web view is one widget among the others.

## CDP

The endpoint is content's own DevTools server, what Chrome serves with
`--remote-debugging-port`: `zebkit_cdp_listen()` starts it, and the
switch works as for Chrome. Puppeteer and Playwright connect to the
browser target's WebSocket address (`zebkit_cdp_get_url()`).

## Source

| Path | What it does |
|---|---|
| `src/engine/engine.cc`, `engine.h` | `zebkit_main()`, `zebkit_helper_main()`, the classes that start the engine; where its files are |
| `src/engine/engine_mac.mm` | macOS: the framework's and helper's places, the helper's sandbox, the menu bar, headless views |
| `src/engine/view.cc`, `view.h` | `zebkit_view`: the window and its page, grants, bindings, the page's script |
| `src/engine/attach.cc` | Views inside another view's window (Linux) |
| `src/engine/offscreen.cc`, `offscreen_mac.mm` | `<zebkit/offscreen.h>`: views drawn off-screen, their frames from viz's video capturer, their input, popups and cursor (from `patches/1021`'s hooks) |
| `src/engine/call.cc`, `call.h` | `zebkit://call`: the page's calls and their answers as frames of bytes |
| `src/engine/bridge.cc` | What `zebkit.*` calls do once the browser has checked them |
| `src/engine/pdf.cc`, `pdf_internal.cc`, `pdf_bridge.cc`, `pdf_script.h` | `<zebkit/pdf.h>`, PDFium's classes, `zebkit.pdf` and its queue |
| `src/engine/scheme.cc`, `scheme.h` | The application's URL schemes |
| `src/engine/services.cc` | `<zebkit/services.h>` |
| `src/engine/script.cc` | Script outside any page |
| `src/engine/ui.cc`, `ui.h`, `ui_script.h` | `zebkit.ui` |
| `src/engine/app.cc`, `app.h` | The application's menu, opened files, reopen and quit |
| `src/engine/cdp.cc` | The DevTools endpoint |
| `src/engine/helper.c` | `zebkit-helper` |
| `src/engine/vendor/gen_vendor.py` | The libraries' entry points and headers |
| `src/engine/include/zebkit/` | The public headers |
| `src/engine/BUILD.gn` | The library, the helper, the framework; linked into Chromium's tree as `//zebkit/engine` |
