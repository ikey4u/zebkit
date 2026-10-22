# macOS

> The engine is Chromium's Blink now (2026-10-17): its macOS port is
> `docs/MACOS.md`. This is the record of the WebKit port.

zebkit on macOS: what was decided, what is done, how large and how fast
it is, what is left, and where to start on a Mac. `docs/PROGRESS.md` has
the project as a whole; this is the part that is macOS's alone. Whoever
picks the work up, on whatever machine, should need nothing but this file
and the repository.

## Decisions taken

- **The engine is WebKit's own Mac port**, built by us from the same WebKit
  revision as the Linux engine, as our own `WebKit.framework`. Not WPE
  ported to macOS: WPE needs GLib, libsoup, GStreamer and a window backend
  that nobody maintains there. Not the WebKit that ships with macOS: the
  CDP endpoint is compiled into the engine and uses what that framework
  does not expose.
- **Networking, media, fonts, text and graphics are the system's** on
  macOS (CFNetwork, AVFoundation, CoreText, CoreGraphics, Metal). The
  libraries in `deps/` are for Linux.
- **FFmpeg is in the engine for applications**, as on Linux
  (`docs/VENDOR.md`): the same version, built the same way, behind the
  same `zebkit_vendor_ffmpeg_*` entry points and the same header. The
  engine itself does not use it on macOS. It costs 5.4 MiB.
- **The package is one framework, `Zebkit.framework`**, in `dist/release`
  as on Linux, which works wherever it is copied. A program links it and
  includes `<Zebkit/Zebkit.h>`. Its binary has no code and hands on what
  the engine exports; the engine is inside it, under the names WebKit's
  build gives it (`WebKit.framework`, `WebCore.framework`,
  `JavaScriptCore.framework`, the `com.apple.WebKit` services), not
  renamed and not re-linked.
- **Why the engine inside keeps WebKit's names:** it is found by them.
  macOS has a WebKit of its own, which its own libraries use. The loader,
  told a directory, takes the frameworks of those names there in the
  system's place, for the program and for those libraries, and the process
  has one engine. Tried and dropped: everything renamed and linked by
  path, with no word to the loader. It links and loads pages, but system
  libraries the engine uses (Lookup, QuickLook, data detectors) then load
  the system's WebKit, WebCore and JavaScriptCore as well, and the process
  has two of each, with the same class names and bundle identifiers.
- **A view's window is the engine's**, as on Linux: `zebkit-view.h`
  (`src/view`, built in by `patches/2010`) makes a view with a window of
  its own, on the screen when the machine has a display and nowhere
  otherwise. The window example and the Rust crate use it. An
  application that manages its own windows creates a `WKWebView` as
  always.
- **The same checks decide when it is done**: `tests/cdp/protocol.py`
  against the engine and against Chrome, the Puppeteer and Playwright
  suites, smoke and media tests.

## Not decided

| Question | Why it matters |
|---|---|
| Apple Silicon only, or Intel too | Build time doubles, and the release archive is per architecture or universal |
| Oldest macOS to support | Sets the deployment target, and with it which system APIs the engine may use. As built now it is macOS 27, the version of the SDK, which is what upstream builds. An older target means patching the code that assumes otherwise; how much there is has not been looked at |
| The C API | On macOS the engine's public API is Objective-C (`WKWebView`), not the GLib C API. The Rust crate hides the difference, with C functions of its own over the Objective-C API (`rust/zebkit-rs/src/cocoa/shim.m`); C applications see two APIs unless a common C layer is written, for which that file is a start |
| A debug build | `scripts/dist.sh debug` is the `slim-dev` profile on Linux. There is no such profile for macOS: it would be a second build tree of 30 GB |

## What is needed

- A Mac with Apple Silicon. Building the Mac port cannot be done from Linux.
- **Xcode 27** (Swift 6.3 or later, the macOS 27 SDK). Xcode 26.2 does not
  do: this revision of WebKit uses a Swift compiler flag that came with
  Swift 6.3, in two modules that cannot be switched off.
- **The Metal Toolchain**, a separate download, once per Xcode:
  `xcodebuild -downloadComponent MetalToolchain`. WebKit's graphics layer
  compiles its shaders with it. `scripts/build.sh` says so if it is missing.
- About 55 GB of disk for one profile's build tree (30 GB without
  link-time optimization).
- **mise**, to run the tasks (`mise run r`). It brings the one tool the
  system lacks, `xz`, which packs the release archive (`mise.toml`).
  `scripts/release.sh` run by hand needs an `xz` on the path and says so
  before it builds.
- For releases, later: an Apple Developer ID, to sign and notarize. The JIT
  needs the `com.apple.security.cs.allow-jit` entitlement in a hardened
  runtime. Not needed to develop.

## State

Done on a Mac (macOS 27.0, Xcode 27.0, arm64):

- **Source.** `upstream.lock` pins the commit of the release tag in WebKit's
  git repository, and `scripts/fetch-upstream.sh` on macOS fetches that
  (shallow, without the test suites, 1.7 GB) into
  `upstream/webkit-<version>-macos` and applies `patches/series.macos`. It
  updates the tree in place, so a patch change rebuilds only what it reaches.
- **Build.** `scripts/build.sh <profile>` runs Xcode on WebKit's workspace
  (`scripts/build-macos.sh`), with `profiles/macos/<profile>.xcconfig` laid
  over every target, into `out/<profile>/`. Nothing is written outside the
  checkout. A full build takes about 40 minutes on ten cores.
- **Baseline.** Builds, and `scripts/smoke.sh baseline` passes: layout,
  canvas, WebGL, the optimizing JIT, WebAssembly and Intl, over HTTP and
  HTTPS, in a view that is never shown (`tools/pageprobe/pageprobe.m`).
- **Two build fixes** the branch needs on macOS, `0007` and `0008` in
  `patches/`. The 2.54 branch is built upstream for the GLib ports only.
  The second one leaves pull-to-refresh out of the view, which needs a
  header only Apple has.

Baseline size, the binaries stripped of local symbols:

| | MiB |
|---|---:|
| WebCore | 54.2 |
| JavaScriptCore | 27.7 |
| WebKit | 23.5 |
| libwebrtc | 10.2 |
| libANGLE | 3.7 |
| WebKitLegacy | 3.0 |
| WebGPU | 2.4 |
| **Engine binaries** | **124.7** |
| WebInspectorUI (resources) | 18 |

That is more than the Linux package (83 MiB), not less as first assumed:
the system supplies networking, media and fonts, but nothing has been
taken out yet, and the Mac port carries WebRTC, WebGPU, the legacy API and
the inspector's interface.

- **The CDP endpoint.** Built into `WebKit.framework` (`patches/2009`,
  which also says how WebKit tells the endpoint what a view's delegates
  are told). Its macOS layer is `CDPViewCocoa.mm`, `CDPTransportCocoa.mm`
  and `CDPImageCocoa.mm`, with HTTP and WebSockets of its own
  (`CDPWebSocket.cpp`); what the Linux layer had that was not Linux's is
  now `CDPView.cpp`, for both. `tools/cdphost/cdphost.m` is the host for
  the tests. It adds 0.4 MiB to the framework.
  `scripts/cdp.sh baseline --clients`: raw checks **42 of 43** with no
  departures from the standard definition, Puppeteer **17 of 17**,
  Playwright **17 of 17**. The one raw check that fails expects a name
  that cannot be resolved; the Mac it ran on has a network tool that
  answers every name (`no-such-host.invalid` resolves to 198.18.0.29, and
  `curl` gets an empty reply from it), so the engine reports a closed
  connection, which is what happened. `docs/CDP.md` has what differs from
  Linux.

What exists on Linux and where it is on macOS:

| | Linux | macOS |
|---|---|---|
| Fetch, build, smoke test, benchmark, CDP tests | yes | yes, the same scripts |
| `slim` profile | yes | yes |
| Libraries built from source (`scripts/build-deps.sh`) | 29, in `deps/order` | FFmpeg, in `deps/order.macos`; the rest is the system's |
| FFmpeg for applications, and its test (`scripts/vendor.sh`) | yes | yes, the same test with the same results |
| Package (`scripts/package.sh`), `dist/release` (`scripts/dist.sh`, `mise run r`) | yes, `libzebkit.so.1` | yes, `Zebkit.framework` |
| Release archive (`scripts/release.sh`) | `x86_64-unknown-linux-gnu` | `aarch64-apple-darwin`; both are in the release `v0.0.1-alpha.2`, which on macOS still has the frameworks side by side and no `Zebkit.framework`. Each is built on its own system and they are published together from one |
| A view's window: shown when there is a display, not otherwise | the engine's (WPE's display backends) | the engine's (`zebkit-view.h`) |
| Window example | `examples/window.c` | `examples/window.m`, the same program, with the same bar (`examples/window-bar.h`) |
| FFmpeg example | `examples/ffmpeg.cpp` | the same file |
| Rust crates, their test and examples | yes | yes, the same interface, test and examples |
| CDP host for the tests | `tools/cdphost/cdphost.c` | `tools/cdphost/cdphost.m` |
| `dist/debug` (`mise run d`) | yes | no profile for it |
| Page probe for the tests | `tools/pageprobe/pageprobe.c` | `tools/pageprobe/pageprobe.m`, with the same options, questions (`probe-scripts.h`) and report |
| Media test (`scripts/media.sh`) | yes | yes, the same samples; the system's decoders play them |
| Site comparison (`scripts/compat.sh`, `compat-report.py`) | yes, `docs/reports/compat.md` | yes, `docs/reports/macos/compat.md` |
| Size report (`scripts/measure.sh`) | yes, `docs/reports/<profile>.md` | yes, `docs/reports/macos/<profile>.md` |
| Code size by subsystem, linker map (`codesize.py`, `mapsize.py`) | yes | no; `docs/reports/macos/slim-experiments.md` has the breakdown made by hand |

`dist/release` on macOS, 92.3 MiB (`docs/reports/macos/slim.md`), 27.9 MiB
as the archive:

```
Zebkit.framework/
    Zebkit                  re-exports the engine
    Headers/                <Zebkit/Zebkit.h>: WebKit's API, zebkit-view.h, zebkit-cdp.h
    Frameworks/
        WebKit.framework  WebCore.framework  JavaScriptCore.framework
        WebKitLegacy.framework  WebGPU.framework
        libwebrtc.dylib  libANGLE-shared.dylib
        com.apple.WebKit.WebContent.xpc  com.apple.WebKit.Networking.xpc  com.apple.WebKit.GPU.xpc  ...
include/                    what the Linux package has there: zebkit-cdp.h, zebkit/vendor/
```

A program is linked with `-F<directory> -framework Zebkit`,
`-Wl,-rpath,<directory>` and
`-Wl,-dyld_env,DYLD_FRAMEWORK_PATH=<directory>/Zebkit.framework/Versions/A/Frameworks`,
the directory given relative to the executable (`@executable_path/zebkit`)
for a program that is shipped. Without the last option it does not start:
the loader says a zebkit symbol is missing from the system's WebKit.
`scripts/example.sh` and the Rust crates' build scripts do this. The
headers are Objective-C; there is no module for Swift yet. The tests and
their hosts (`tools/`) run against the build's own products in
`out/<profile>/Release`, where the frameworks are side by side as Xcode
leaves them.

Everything in the package is signed ad hoc, which is what runs on the
machine that built it. Seen once and not explained: an engine directory
whose links were broken was replaced, at the same path, by a good one, and
pages still did not load from that path, while the same files at any
other path worked.

## Slim

`profiles/macos/slim.xcconfig`. Engine binaries stripped of local symbols,
MiB; `scripts/bench.sh` in milliseconds, lower is better. Each build, what
it showed and what was tried and dropped is in
`docs/reports/macos/slim-experiments.md`.

| Build | Size | loop | objects | json | regex | sort | dom |
|---|---:|---:|---:|---:|---:|---:|---:|
| `baseline` (upstream's settings) | 125.1 | 18 | 19 | 83 | 74 | 42 | 82 |
| `-Os`, link-time optimization | 109.4 | 19 | 17 | 85 | 75 | 38 | 80 |
| the same, features off | 102.0 | 33 | 18 | 85 | 78 | 37 | 79 |
| the same, checks without their text | 96.8 | 33 | 18 | 86 | 75 | 38 | 77 |
| `-Oz`, link-time optimization, nothing else | 82.2 | 19 | 29 | 220 | 139 | 73 | 212 |
| `-Oz`, features off but all JIT tiers on, checks without their text | 75.2 | 18 | 30 | 222 | 140 | 74 | 208 |
| the same, the JavaScript engine at `-Os` | 81.7 | 19 | 18 | 85 | 77 | 38 | 201 |
| the same, WebCore inlining as `-Os` does | 83.9 | 18 | 17 | 84 | 76 | 38 | 137 |
| the same, with FFmpeg for applications: **`slim`** | **89.4** | 18 | 18 | 86 | 78 | 37 | 137 |

| `slim`, by binary | MiB |
|---|---:|
| WebCore | 33.3 |
| JavaScriptCore | 22.5 |
| WebKit, FFmpeg in it | 20.0 |
| libwebrtc | 6.3 |
| libANGLE | 3.3 |
| WebKitLegacy | 2.4 |
| WebGPU | 1.4 |

Compressed, the seven binaries without FFmpeg (83.9 MiB) are 24.5 MiB
with xz, 28.8 with zstd and 36.9 with gzip (Linux's 80.3 MiB build: 23.0,
25.1 and 32.0). The whole package with FFmpeg, 92.3 MiB, is 27.9 MiB as
the release archive.

- **`-Oz`**, the owner's choice, as on Linux. Against `-Os` it takes
  about 25 MiB off and makes JSON and DOM work two and a half times
  slower (Linux pays 1.8 times). Function outlining is not the reason:
  switching it off changes neither size nor speed.
- **`-Os` for the JavaScript engine** (JavaScriptCore, WTF, bmalloc), for
  6.5 MiB: JSON, regular expressions, sorting and object work are then as
  fast as upstream's build.
- **WebCore inlines as with `-Os`** (the inlining threshold raised from
  5 to 50), for 2.2 MiB: DOM work is 1.7 times slower than upstream's
  build instead of 2.5 times. A profile of the DOM benchmark has no one
  hot place; a third of the time is in small accessors, reference counts
  and destructors called from all over style, layout and rendering,
  which `-Oz` leaves as calls. All of WebCore at `-Os` would cost
  11.7 MiB and be as fast as upstream's.
- **All JIT tiers on**, the owner's choice, unlike Linux. Without the top
  tiers the benchmark's hot loop was 1.7 times slower, for 4.1 MiB at
  `-Os`.
- **Features off**, as on Linux: encrypted media, gamepad, speech
  synthesis, web extensions with their part of the inspector (2.7 MiB at
  `-Os`), the resource-usage page. `patches/0009` is what it takes to
  build that way.
- **Checks without their text** (5.2 MiB at `-Os`): a failed run-time
  check stops the process and no longer carries its file name and C++
  signature (`patches/1004`). Linux does the same with a header put in front of
  every file, which Xcode's precompiled headers do not allow.
- **Left on though off on Linux**, because the Mac port does not build
  without them: Safari's remote inspector, WebDriver, content blockers
  (also an API an application may want), WebCodecs (tied to WebRTC).
- **Not decided:** WebRTC and media capture (about 8 MiB; off on
  Linux, but the same library may carry WebM, VP9 and Opus on macOS, so
  the media test comes first), and what the Linux engine never had:
  WebAuthn, Apple Pay, the PDF viewer, `<model>`, WebGPU, the legacy API.
- **Against Linux.** The Linux engine is 76.7 MiB, and about 24 MiB of
  that is what macOS takes from the system (ICU data, media libraries) or
  does not have (relocation tables). What only the Mac port has makes up
  for it: the three libraries above that Linux is without (10.1 MiB), the
  top JIT tiers, and the code in the table below.
- The inspector's interface (18 MiB of resources) is built and will not be
  packaged.

What features weigh inside WebCore, WebKit and JavaScriptCore, from the
names of the symbols in the 102.0 MiB build (`-Os`; about a quarter less
at `-Oz`). Lower bounds: code shared with something else, and text, are
not counted.

| Feature | MiB |
|---|---:|
| WebRTC and media capture (and libwebrtc) | 1.9 |
| GPU process, which graphics and media run in on macOS | 1.4 |
| WebGPU (and its framework) | 0.9 |
| WebAuthn | 0.7 |
| Web Push and notifications | 0.6 |
| Writing tools, text recognition, data detectors | 0.6 |
| The Objective-C API | 0.6 |
| Apple Pay | 0.5 |
| PDF viewer | 0.4 |
| WebDriver | 0.4 |
| AirPlay, picture in picture | 0.3 |
| Content blockers | 0.3 |
| WebCodecs | 0.3 |
| `<model>` | 0.1 |
| Remote inspector | 0.03 |

The four that do not build when off (WebDriver, remote inspector, content
blockers, WebCodecs) are about 1 MiB together, which is not worth the
patches.

Known gaps on macOS:

- WebP screenshots (no encoder; libwebp would have to be built in).
- Right-click: WebKit shows the system's context menu, which waits for the
  user. Not tested over CDP yet.
- Error names depend on the system language (see `docs/CDP.md`).
- Middle-button events are made with AppKit's general mouse-event call and
  have not been checked to arrive as button 1.
- A view on a Mac with no display, or in a process outside a login
  session (ssh, a daemon): `zebkit-view.h` gives it a window that is on
  no screen, and whether a page then loads has not been tried, for want
  of such a machine. With a display, both kinds of window are tested.

## The work that is left, in order

1. **CDP, the rest.** The gaps above, and a run on a Mac without a
   name-answering network tool.
2. **Slim, the rest.** The decisions listed under "Slim".
3. **Signing and notarization**, with the hardened runtime: whether a
   program may then still name its frameworks' directory the way it does
   now has to be checked.

## Starting on a Mac

```sh
git clone git@github.com:ikey4u/zebkit.git && cd zebkit
scripts/dist.sh release            # source, FFmpeg, the slim engine, dist/release
scripts/smoke.sh slim
scripts/cdp.sh slim --clients
scripts/vendor.sh slim
scripts/example.sh macos-basic     # a window
(cd rust && cargo test)
scripts/media.sh slim
scripts/measure.sh slim            # docs/reports/macos/slim.md
scripts/compat.sh baseline && scripts/compat.sh slim && scripts/compat-report.py slim
```

`scripts/build.sh baseline` builds the engine with upstream's settings,
for comparison; the site comparison needs it.
