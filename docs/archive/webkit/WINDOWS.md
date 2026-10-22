# Windows

zebkit on Windows is started: WebKit's Windows port is built here, on
Linux, with the functions of the other platforms switched on as far as the
port has them and with the CDP endpoint in it, and under Wine it passes
the endpoint's tests. Nothing is slimmed for size, packaged or released
yet, and nothing has run on Windows itself. `docs/PROGRESS.md` has the
project as a whole; this is the state of Windows, what the port is made
of, and what is left.

## State

| | |
|---|---|
| Engine | WebKit's Windows port, from the same release as the others (`upstream.lock`), out of WebKit's git repository as on macOS |
| Built on | Linux. There is no Windows machine in this project yet; the port is cross-built, which WebKit supports itself at this release |
| Target | x86_64, Windows 10 1809 and later (the port's own minimum) |
| Tried on | Wine 11.19, on the build machine (`scripts/wine.sh`). Not on Windows |
| Profiles | `baseline`, the port as it is: 165.8 MiB. `slim`, the functions of the other platforms and the CDP endpoint, not yet made small: 160.3 MiB |
| Patches | seven (`patches/series.windows`): three fixes, the inspector patch the endpoint shares with the other platforms, the endpoint's hooks, timers for applications, and wheel events with exact distances |
| CDP | raw checks 43 of 43, Puppeteer 17 of 17, Playwright 17 of 17 (`ZEBKIT_TARGET=windows scripts/cdp.sh slim --clients`) |
| Missing against Linux and macOS | Web Audio and Media Source Extensions (below, "Media") |
| Not ported | the smoke and media tests, the top-100 comparison, the benchmark, the size reports, FFmpeg for applications |
| Package, release, examples, Rust crates | none |

What runs under Wine:

- **JavaScript**, with the JIT: `jsc.exe` runs, and ICU answers (number
  and date formats, time zones).
- **Pages** in MiniBrowser: a local one (HTML, CSS, script, canvas and
  Chinese, Japanese and Korean text) and a Wikipedia article over HTTPS,
  images included, with the web, network and GPU processes running.
- **The CDP endpoint**, in an application of our own (`tools/cdphost`):
  everything its tests ask of Linux and macOS, input included.

## Size of the baseline

| | MiB |
|---|---|
| `WebCore.dll` | 54.1 |
| `JavaScriptCore.dll` | 31.5 |
| `WebKit2.dll` | 17.1 |
| `libGLESv2.dll`, `libEGL.dll` (ANGLE) | 5.0 |
| The three helper programs | under 0.1 |
| `WebKit.resources` | 8.1 |
| 29 library DLLs from vcpkg, of which ICU's data is 30.4 | 49.8 |
| **Total** | **165.8** |

This is the port as WebKit builds it: no link-time optimization, identical
code not folded, every library a DLL with all of its code. Linux's
baseline is 213.0 MiB by the same kind of count and its `slim` 83.1.
Not counted: Microsoft's C++ runtime, which the engine expects on the
machine (`msvcp140.dll`, `vcruntime140.dll` and two more), MiniBrowser,
`jsc.exe`, WebDriver and the debug symbols (`.pdb`).

From the system the engine takes Direct3D, DirectWrite, Media Foundation,
Winsock and the window system, and nothing else.

## Building

Everything for Windows is asked for with `ZEBKIT_TARGET=windows` on a
Linux machine, and lands under `out/windows`:

```sh
scripts/fetch-toolchain-windows.sh                  # compiler, Microsoft's SDK, vcpkg -> out/toolchain/windows
ZEBKIT_TARGET=windows scripts/fetch-upstream.sh     # WebKit's git tree -> upstream/webkit-<version>-windows
ZEBKIT_TARGET=windows scripts/build.sh slim         # -> out/windows/slim (baseline: the port unchanged)
scripts/wine.sh slim jsc.exe -e 'print(1 + 1)'      # run something of it under Wine
ZEBKIT_TARGET=windows scripts/cdp.sh slim --clients # the CDP tests, under Wine
```

A first build takes under an hour on 24 cores, and about 19 GB: 8 for
the build tree, 8 for the toolchain and its downloads, 2 for WebKit's
source.

What the build is made of:

- **The compiler is clang-cl**, with lld-link and LLVM's stand-ins for
  Microsoft's `lib`, `rc` and `mt`. WebKit no longer builds with
  Microsoft's compiler, and its cross-build asks for LLVM 20; that is the
  version pinned in `toolchain.lock`. Debian's own LLVM is older and is not
  used.
- **Microsoft's SDK and C++ runtime** (headers and import libraries) are
  fetched from Microsoft by [xwin](https://github.com/Jake-Shadle/xwin),
  at the versions in `toolchain.lock`. They are under Microsoft's license
  for the Visual Studio Build Tools, which `fetch-toolchain-windows.sh`
  accepts by running. None of it goes into a package except the runtime
  DLLs Microsoft allows to be redistributed, and whether those are shipped
  or expected on the machine is not decided.
- **The libraries the port links** are built by vcpkg from WebKit's own
  manifest (`vcpkg.json` in its tree) with zebkit's triplet,
  `deps/windows/x64-windows-zebkit.cmake`. The first configure builds them
  (about seven minutes) into `out/windows/<profile>/vcpkg_installed`;
  built packages are cached in `.cache/vcpkg`. They are DLLs, as WebKit
  builds them. On Linux the same libraries are static and built by
  `scripts/build-deps.sh` from `deps/`; bringing the two together is part
  of the slimming.
- **The profile** is a CMake initial-cache file in `profiles/windows`, as
  on Linux.

## What the port is

Read from the port's build files at this release, not yet from measuring it.

| | Windows port | Linux (WPE) | macOS |
|---|---|---|---|
| Build | CMake, clang-cl | CMake, clang | Xcode |
| Graphics | Skia, ANGLE on Direct3D | Skia, system OpenGL ES | CoreGraphics, Metal |
| Network | curl, with LibreSSL, HTTP/2 and HTTP/3 | libsoup, GnuTLS | the system's |
| Media | Media Foundation, the system's | GStreamer and FFmpeg, shipped | AVFoundation, the system's |
| Text | HarfBuzz and DirectWrite, ICU shipped | HarfBuzz and FreeType, ICU shipped | CoreText, system ICU |
| API | WebKit's C API (`WKView` with a window handle, `WKPage`) | WebKit's GLib API | `WKWebView` |
| Processes | web, network and GPU processes, `.exe` files beside the DLLs | web and network processes | XPC services |

The port's own defaults are another set of functions than zebkit's.
`profiles/windows/slim.cmake` brings them to what `slim` has on Linux, and
says where it cannot or does not; "Media" below is the one function
missing. WebRTC is off here as on Linux.

Media is the system's, as on macOS, so the codec licensing question
(`docs/PROGRESS.md`) does not come up for playback. FFmpeg for
applications (`docs/VENDOR.md`) has to be built for Windows separately.

## CDP

`src/cdp` is portable code over a layer for each engine API
(`CDPPlatform.h` lists what a layer supplies). The layer for Windows:

| File | What it holds |
|---|---|
| `CDPViewWin.cpp` | The view (`WebKit::WebView`, what a `WKViewRef` is): its life, the views the endpoint makes, navigation history, window size, input |
| `CDPViewProxy.cpp` | What needs nothing of the platform: loads, dialogs, file choosers, network sessions, cookies. macOS has the same code in `CDPViewCocoa.mm` from before this file existed and is to be moved onto it |
| `CDPTransportWin.cpp` | The listening socket and the clients' connections, Winsock sockets that report to a window of the main thread; HTTP and WebSockets are `CDPWebSocket.cpp`'s, as on macOS. The public API (`zebkit-cdp.h`) |

How it differs from the other two, and why:

- **Where WebKit tells the endpoint.** As on macOS the endpoint cannot be
  the application's callbacks, of which a page has one set. It is told from
  where the C API calls them (`patches/2011`), and every view is given
  empty callbacks so that those places exist before the application sets
  its own.
- **Input is made as WebKit's events, not as window messages.** A message
  says less than CDP does: no click count, no key name, no Meta key, and
  modifiers only as the state of the real keyboard. The endpoint lets
  WebKit make its event from a message and puts into it what the client
  said. Pages see trusted events, and default actions happen.
- **Timers.** The port fires its timers only from its own message loop,
  which an application does not run and cannot: it is not in the API.
  Nothing in an embedding application's process that waits ever went on,
  the endpoint's own waits included. `patches/2012` makes them fire under
  any message loop. This is a fix for every application, not for the
  endpoint alone.
- **Wheel events carry exact distances.** The port's wheel events could
  only be a wheel's notches, which are animated when scrolling is smooth,
  so a client that scrolled read the position in the middle of the
  movement. `patches/2013` lets them say that the distance is exact, as
  the GLib ports and macOS can; the endpoint's are, and scroll at once,
  as on the other platforms. Scrolling by the keyboard and by a real
  wheel is animated, as on Linux.
- **The engine's version** in `Browser.getVersion` is the release's
  (`upstream.lock`), handed to the build, since the port has no version of
  its own to ask for.

## Media

`<video>` and `<audio>` play through Media Foundation, the system's, as
macOS uses the system's. The port has no implementation of Web Audio (no
audio output, no decoder for audio files) and none of Media Source
Extensions, which Linux and macOS have. There are two ways to get level:
Linux's stack (GStreamer and FFmpeg built for Windows, and WebKit's
GStreamer code ported to this port), or writing the missing parts on Media
Foundation. Decided on 2026-10-13: later, after CDP, slimming, packaging
and the Rust crates. Until then it is the one gap in function against the
other platforms.

Media Foundation itself is not tried: Wine's is not Windows'.

## To decide

| Question | Why it matters |
|---|---|
| ARM64 as well as x86_64 | One archive for each; the toolchain can target both |
| Oldest Windows | The port's minimum is Windows 10 1809. Going older is the port's fight, not ours |
| The C++ runtime | Ship Microsoft's redistributable DLLs in the package, or link the runtime statically, or expect it installed |
| The API applications see | WebKit's C API is what the port has. It is the third API of three, which makes the common C layer (`docs/MACOS.md`, "Not decided") more pressing |
| Web Audio, Media Source Extensions | Put off (see "Media"); which of the two ways is to be decided then |
| The JIT tiers | All of them here, as on macOS; Linux has dropped the top ones for size after measuring. Not measured here |
| A Windows machine | Wine runs the build here and says whether it starts. It does not say whether Direct3D, Media Foundation, fonts or input behave as on Windows, and it is no place to measure speed |

## Found on the way

- **ICU's data library exported nothing.** Built on Linux, the tool that
  packs ICU's data writes assembly for a GNU toolchain, and the DLL made
  from it has no exports; Windows would not load ICU at all, and under
  Wine every use of a locale failed. `deps/windows/icu-data-export.cmake`
  asks for the export. WebKit's own cross-build has the same recipe.
- **Two things missing for a DLL link** that Linux never notices: a Skia
  source file left out of the build list (`patches/0010`) and five WebCore
  functions the port's compositor calls without their being exported
  (`patches/0011`).
- **Every process died half a minute after it started**, under Wine: the
  memory monitor's first measurement used a number the system had not
  written, when the call that should write it fails (`patches/0012`). This
  was the crash first seen on a real site.
- **Timers do not fire in an embedding application** (`patches/2012`; see
  "CDP").
- **Precompiled headers and the compiler cache.** A precompiled header
  remembers the dates of the files it was made from, and the cache hands
  out headers made at another time. The build passes `-fno-pch-timestamp`,
  and the SDK's files are given one fixed date when fetched.
- **The compiler crashes now and then**, a different file each time, as
  on Linux; the build retries.
- **Wine needs a display and fonts.** `scripts/wine.sh` gives it a
  virtual display, so that nothing appears on the screen of the machine
  it runs on, and names this machine's Liberation fonts as stand-ins for
  the ones every Windows has. Without them the text of a form control has
  no height, because the engine asks for those fonts by name. Whether the
  dialog font the engine asks for (`MS Shell Dlg`) resolves on Windows
  itself is not known.
- **WebKit's C headers are not C** by themselves: they say `bool` and do
  not include `<stdbool.h>`. An application includes it first
  (`tools/cdphost/cdphost-win.c`).

## Next

1. The tests the other platforms have, on the endpoint that now exists:
   smoke, media, the top-100 comparison against `baseline`, the benchmark.
2. Size: static libraries from `deps/` instead of vcpkg's DLLs, ICU data
   trimmed as on Linux, link-time optimization, and measuring in the form
   of the other platforms (`docs/reports/windows/`).
3. A package that runs from any directory, the release archive, a view
   with a window of its own (`zebkit-view.h`), the window example, FFmpeg
   for applications, the Rust crates.
4. Web Audio and Media Source Extensions ("Media").
5. Moving macOS onto `CDPViewProxy.cpp`.
6. A run on Windows itself, as soon as there is a machine.
