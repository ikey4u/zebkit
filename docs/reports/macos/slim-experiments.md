# Slimming the macOS engine: the experiments

How `profiles/macos/slim.xcconfig` came to be what it is: every build that
was measured, what it showed, and what was tried and dropped. The summary
is in `docs/MACOS.md`; this is the record behind it.

- Upstream: WebKit's Mac port at the tag `wpewebkit-2.54.1`
- Measured: 2026-10-11 and 12 on macOS 27.0.1, Apple Silicon, Xcode 27.0
- **Result: 83.9 MiB**, against 125.1 MiB with upstream's settings;
  24.5 MiB compressed with xz. With FFmpeg added for applications, which
  is what ships: 89.4 MiB, 92.2 as a package, 28.2 as its archive

## How it was measured

- **Size.** The seven engine binaries of a build, each copied and stripped
  of local symbols (`strip -x`), added up: WebCore, JavaScriptCore, WebKit,
  libwebrtc, libANGLE, WebKitLegacy, WebGPU. Helper processes are a few
  kilobytes each and not counted; neither are the inspector's interface
  (18 MiB of resources, not to be packaged) or headers.
- **Speed.** `scripts/bench.sh <profile>`: `tests/bench/index.html` loaded
  three times, the best time of each test kept. Milliseconds, lower is
  better. These are small loops of one kind of work each, not page loads.
- **Correctness.** `scripts/smoke.sh` and `scripts/cdp.sh <profile>
  --clients` after every change that was kept. The CDP result was the same
  each time: 42 of 43 raw checks (the one failure is the test machine's
  network tool answering for a host that does not exist), Puppeteer 17 of
  17, Playwright 17 of 17, no departures from the protocol definition.
- **Cost of a measurement.** A change of compiler settings or of defines
  rebuilds everything: 28 to 40 minutes, and about 55 GB of build tree.

## The builds

| # | Build | Size | loop | objects | json | regex | sort | dom |
|---:|---|---:|---:|---:|---:|---:|---:|---:|
| 1 | `baseline`: upstream's settings | 125.1 | 18 | 19 | 83 | 74 | 42 | 82 |
| 2 | `-Os` everywhere, thin link-time optimization | 109.4 | 19 | 17 | 85 | 75 | 38 | 80 |
| 3 | 2, features off, top JIT tiers among them | 102.0 | 33 | 18 | 85 | 78 | 37 | 79 |
| 4 | 3, checks without their text | 96.8 | 33 | 18 | 86 | 75 | 38 | 77 |
| 5 | `-Oz` everywhere, thin link-time optimization | 82.2 | 19 | 29 | 220 | 139 | 73 | 212 |
| 6 | 5, features off but all JIT tiers on, checks without their text | 75.2 | 18 | 30 | 222 | 140 | 74 | 208 |
| 7 | 6, the JavaScript engine at `-Os` | 81.7 | 19 | 18 | 85 | 77 | 38 | 201 |
| 8 | 7, WebCore inlining as `-Os` does | 83.9 | 18 | 17 | 84 | 76 | 38 | 137 |
| 9 | 8, with FFmpeg for applications: **`slim`** | **89.4** | 18 | 18 | 86 | 78 | 37 | 137 |

By binary, MiB:

| Binary | 1 | 4 | 6 | 7 | 8 |
|---|---:|---:|---:|---:|---:|
| WebCore | | 42.8 | 31.1 | 31.1 | 33.3 |
| JavaScriptCore | | 18.7 | 16.0 | 22.5 | 22.5 |
| WebKit | | 19.3 | 14.6 | 14.6 | 14.6 |
| libwebrtc | | 7.6 | 6.3 | 6.3 | 6.3 |
| libANGLE | | 3.7 | 3.3 | 3.3 | 3.3 |
| WebKitLegacy | | 2.8 | 2.4 | 2.4 | 2.4 |
| WebGPU | | 1.9 | 1.4 | 1.4 | 1.4 |
| Total | 125.1 | 96.8 | 75.2 | 81.7 | 83.9 |

Build 1 was not broken down by binary.

Build 8 compressed, the seven binaries in one archive at each
compressor's highest level:

| Compressor | macOS, build 8 | Linux, its 80.3 MiB package |
|---|---:|---:|
| none | 83.9 | 80.3 |
| xz | 24.5 | 23.0 |
| zstd | 28.8 | 25.1 |
| gzip | 36.9 | 32.0 |

## What each step showed

### 1 to 2: `-Os` and link-time optimization, 15.7 MiB, no cost

Upstream builds JavaScriptCore, WTF and bmalloc with `-O3`, WebCore with
`-O2` and WebKit with `-Os`. `-Os` for all of them, with thin link-time
optimization, is 15.7 MiB smaller and as fast in every test.

### 2 to 3: features off, 7.4 MiB

The switches `profiles/slim.cmake` has off on Linux, where the Mac port
has them:

- The top JIT tiers (FTL, and WebAssembly's BBQ and OMG): 4.1 MiB. The
  benchmark's hot loop went from 19 ms to 33, nothing else moved. **Taken
  back in build 6** at the owner's word: all tiers are on.
- Web extensions with their part of the inspector: 2.7 MiB.
- Encrypted media (both generations), gamepad, speech synthesis, the
  resource-usage page: the rest.

Building this way took four small fixes (`patches/0009`): the Mac port is
only ever built upstream with everything on.

Four of Linux's switches do not build when off on macOS, and stayed on:
`ENABLE_REMOTE_INSPECTOR`, `ENABLE_WEBDRIVER_BIDI`,
`ENABLE_CONTENT_EXTENSIONS`, `ENABLE_WEB_CODECS`. Together what they
guard is about 1 MiB (see "What features weigh"), which is not worth the
patches.

### 3 to 4: checks without their text, 5.2 MiB, no cost

`RELEASE_ASSERT` and its relatives hand the file name and the full C++
signature of the function they are in to the crash handler. Every
template instantiation has its own signature. Counted in build 3 with
`strings`: 3.6 MiB of signatures and 0.6 MiB of paths in the three main
binaries, before the code that loads them.

`patches/1004` makes a failed check stop the process at the same place
and say nothing, when `ZEBKIT_TERSE_CRASH_INFO` is defined. Paths that
remain are made relative with `-ffile-prefix-map`, which also keeps the
builder's home directory out of the binaries.

Two things went wrong on the way:

- Linux does this with `profiles/include/size-prelude.h`, put in front of
  every file with `-include`. Xcode's precompiled header has to be the
  first thing included, and the build stops with "was ignored because ...
  is not first '-include'". Hence a patch and a define.
- The first form of the patch defined the macro before upstream's
  `#ifndef CRASH_WITH_INFO` block. That block also defines
  `VA_OPT_SUPPORTED`, which `JavaScriptCore/tools/Integrity.h` tests, so
  the build failed there. The patch now sits inside the block.

### 5: `-Oz`, 27 MiB, and what it costs

`-Oz` for everything, measured against build 2: 27.2 MiB smaller; JSON
and DOM work two and a half times slower, regular expressions and sorting
1.8 times, object work 1.5 times. JavaScript the JIT compiles is not
affected (the loop). Linux pays about 1.8 times for the same flag.

Function outlining was the first suspect and is not the reason: with
`-mno-outline` neither size nor speed changed.

### 6: the owner's choice

`-Oz`, and all JIT tiers on. 75.2 MiB, with build 5's speed.

### 6 to 7: the JavaScript engine at `-Os`, 6.5 MiB

JSON, regular expressions, sorting and object work are all in
JavaScriptCore and what it stands on. With those three projects
(JavaScriptCore, WTF, bmalloc) at `-Os` and the rest at `-Oz`, the four
tests are as fast as upstream's build. Only JavaScriptCore's binary grew,
from 16.0 to 22.5 MiB. DOM work did not move: it is WebCore's.

The profile does it with a setting that names the project:
`$(ZEBKIT_OPTIMIZATION_$(PROJECT_NAME):default=z)`.

### 7 to 8: WebCore inlining as `-Os` does, 2.2 MiB

What was asked was a narrower part of WebCore to build with `-Os`. A
profile says there is none. The DOM test (20,000 elements created, each
given text and a style, then laid out) was run in a loop in build 7 and
its web process sampled for eight seconds with `sample`:

- Of 3,927 samples with the main thread at work, 2,768 were in the layout
  that reading `offsetHeight` forces (1,663 layout, 1,105 style
  resolution and building the render tree) and 578 in removing the old
  elements.
- 1,442 samples, 37%, had a function at the top of the stack that the
  linker shows as `<deduplicated_symbol>`: small functions with identical
  code, merged. Their callers are all over style resolution, layout,
  rendering and the DOM: accessors, reference counting, destructors,
  hash-table lookups. No caller has more than a few percent.

So the cost is that `-Oz` leaves small functions as calls. Its inlining
threshold is 5, where `-Os` has 50 and `-O2` 225. With WebCore still at
`-Oz` and the threshold at 50 (`-mllvm -inline-threshold=50`, for the
compiler and for the link-time optimizer), the DOM test went from 201 ms
to 137, for 2.2 MiB. Upstream's build takes 82; all of WebCore at `-Os`
would get there for 11.7 MiB (build 4 against build 6).

Not tried: a threshold between 50 and 225, and the same treatment for
the WebKit framework.

### 8 to 9: FFmpeg for applications, 5.4 MiB

Not slimming: the opposite, for the sake of one interface on both
systems. On Linux the engine plays media with FFmpeg and lends it to
applications for 1.8 MiB more (`docs/VENDOR.md`). On macOS the engine
plays media with the system's decoders, so the same FFmpeg, built the
same way and with the same 938 functions exposed, is all extra: WebKit's
binary went from 14.6 to 20.0 MiB. Nothing else changed, and the
benchmark is the same.

It is linked into WebKit.framework by the profile alone, with no patch:
the entry points whole (`-force_load`), FFmpeg's archives with their own
names hidden (`-load_hidden`). The package with it (frameworks, helper
processes, resources, without headers) is 92.2 MiB, and its archive
28.2 MiB.

## Why macOS is larger than Linux

The Linux engine is 76.7 MiB with `-Oz` throughout.

- About 24 MiB of the Linux engine is what macOS takes from the system or
  does not have: ICU's data (7.6), the media libraries (about 9),
  relocation tables (7.4).
- macOS has three libraries Linux is without, 10.1 MiB in build 8:
  libwebrtc (WebRTC is off on Linux, and off by default in WPE WebKit),
  WebKitLegacy (the old single-process API of Mac applications, which the
  GLib ports never had) and WebGPU (WebKit's implementation is written
  for Metal; the Linux port has no backend for it).
- macOS has the top JIT tiers on, and `-Os` for the JavaScript engine
  (6.5 MiB).
- FFmpeg is all extra on macOS (5.4 MiB), where on Linux the engine needs
  most of it anyway.
- macOS has code inside the main binaries that Linux has not: below.

## What features weigh

Inside WebCore, WebKit and JavaScriptCore, by the names of the symbols in
build 3 (`-Os`; about a quarter less with `-Oz`). Each symbol's size is
the distance to the next one, and a symbol belongs to the first feature
whose pattern its demangled name matches. Lower bounds: shared code, and
text, are not attributed.

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

For comparison, by the same count: SVG 2.3, the DFG JIT 1.9,
accessibility 1.1, service workers 1.1, WebGL 1.2, IndexedDB 0.7.

Web extensions, gamepad and speech synthesis count as nothing in that
build, which is the check that the switches took.

## Build system, learned the hard way

- **Xcode reads a file given with `-xcconfig` once.** A later change to
  the profile built nothing. `scripts/build-macos.sh` reads the profile
  itself and hands each setting over on the command line.
- **Xcode's compilation cache filled the disk.** It keeps every
  compilation it ever did, as much again for each change of settings. It
  is off (`COMPILATION_CACHE_ENABLE_CACHING=NO`), and the cache link-time
  optimization keeps is deleted when the settings change.

## Not decided

- WebRTC and media capture: about 8 MiB in build 8, the largest feature
  left. The same library may carry WebM, VP9 and Opus on macOS, so
  `scripts/media.sh` has to run there first.
- What the Linux engine never had: WebGPU, the legacy API, WebAuthn,
  Apple Pay, the PDF viewer, `<model>`.
