# Progress

Goal: a self-contained, portable, trimmed WebKit engine for Linux, Windows
and macOS that plays media, runs on old systems and works on the web's
most-visited sites, kept as small as those requirements allow. It is meant
to be embedded in agent applications, which drive it over the Chrome
DevTools Protocol.

Requirements, set by the project owner:

- Runs on glibc 2.17 and newer, with as little as possible from the system.
- Media playback works.
- ICU data covers at least English, Chinese, Japanese and Korean.
- No size cut that costs a lot of speed for a little size. The JIT stays.
- No removing web platform features that common sites rely on.
- Correctness is checked against the top 100 sites (`docs/COMPAT.md`).
- The engine speaks the Chrome DevTools Protocol, so that agent tooling
  (Puppeteer, Playwright, raw CDP clients) can drive it (`docs/CDP.md`).
- Size: about 61 MiB was accepted before media playback and glibc 2.17 were
  required. Those two put it at 80.3 MiB; see "Size" for what that buys and
  what could come back.

Last updated: 2026-10-11

## Current state

| Area | Status |
|---|---|
| Linux `slim` build | Builds; smoke test (HTTP, HTTPS, WebGL, JIT) and media test pass |
| Size | **83.1 MiB** in 11 files, plus headers |
| Form | A shared library, `libzebkit.so.1`, with two helper programs it starts itself (`zebkit-web`, `zebkit-network`). An application links against it and creates views with WPE WebKit's C API; `examples/window.c` is a complete one |
| Chrome DevTools Protocol | Built in; Puppeteer and Playwright connect and drive pages. Checked against Chrome's protocol definition and against Chrome itself. A subset: see "Chrome DevTools Protocol" |
| glibc floor | **2.17**: no shipped binary imports a newer symbol |
| Media | H.264, VP8, VP9, AAC, MP3, Opus, Vorbis, FLAC; MP4 and WebM; Media Source; Web Audio |
| Top-100 sites | **84 pass, 0 fail, 16 blocked** (round 4) |
| Relocatable | Yes: copied elsewhere, it loads an HTTPS page with nothing from the build tree |
| System requirements | **glibc only.** Wayland, X11, GPU buffer and sound libraries are used when present |
| Display | Chosen at run time: Wayland, then X11, then offscreen; all three tested |
| macOS | In progress (`docs/MACOS.md`): WebKit's Mac port builds from the same release with nine patches, and passes the smoke test and, for the CDP endpoint, the raw checks, Puppeteer and Playwright. The `slim` profile is 89.4 MiB of binaries (upstream settings: 125.1), with FFmpeg for applications as on Linux. `dist/release` is a relocatable directory of frameworks, 92.2 MiB, 28.2 as an archive; the examples, the Rust crates and the media test work there as on Linux, and the top-100 comparison is 89 pass, 0 fail, 11 blocked. Feature decisions are open; not signed for distribution |
| Windows, embedding API, CI | Not started |

## Size

Linux x86_64. The measured thing is the directory `scripts/package.sh`
assembles: the stripped engine and every library a base system does not
provide.

| Profile | Engine | Bundled libraries | Total |
|---|---:|---:|---:|
| `baseline` (upstream defaults, host libraries) | 119.6 | 93.3 | 213.0 |
| `slim` | 76.7 | 6.4 | **83.1** |

Compressed, the 80.3 MiB build before the display work was 23.0 MiB with
xz, 25.1 with zstd and 32.0 with gzip.

### How it got from 61.0 to 83.1

| Change | MiB | Measured or estimated |
|---|---:|---|
| Media: WebKit's media code, GStreamer (core and 21 plugins), FFmpeg decoders | about +9 | Estimated from engine sizes before and after |
| glibc 2.17: no packed relocations (they need glibc 2.36) | +7.4 | Size of the relocation section they would remove |
| Different toolchain: no LLVM function merging, clang 21 and libc++ instead of clang 19 and libstdc++ | about -1 net | Estimated; the pieces were not separated |
| WebGL (ANGLE) | +3.7 | Measured: 76.6 to 80.3 |
| X11 backend, static libxkbcommon, on-demand loading of system libraries | +0.3 | Measured: 80.3 to 80.6 |
| CDP endpoint (`src/cdp`), and the inspector agents it drives, which a size patch had removed | +0.7 | Measured: 80.6 to 81.3 |
| All of FFmpeg's API kept and exported for applications (`docs/VENDOR.md`) | +0.3 | Measured: 81.3 to 81.6 |
| FFmpeg parts for applications only: file reading and writing, scaling, its own encoders | +1.5 | Measured: 81.6 to 83.1 |

### Where the engine's size is (at 73.9 MiB, before the display work)

| Section | MiB |
|---|---:|
| Code | 49.5 |
| Read-only data (ICU data is 7.6 of it) | 12.4 |
| Dynamic relocations | 7.4 |
| Vtables and other relocated data | 3.4 |

A per-component breakdown needs a linker map, which the cross toolchain's
linker driver does not produce. `docs/reports/slim-map.md` is from the last build
before the toolchain change and no longer current.

### What could come back

- **Packed relocations, 7.4 MiB.** Only if the glibc floor moves to 2.36.
- **Linker map and function merging, about 1.2 MiB.** Needs lld driven
  directly instead of through the toolchain's driver.
- **ICU extras, about 1.2 MiB.** Khmer, Burmese and Lao dictionaries and the
  less common Chinese collation orders.

## Correctness

`docs/COMPAT.md` has the method and the record. Each of the 100 sites is
loaded in `slim` and in stock WebKit; `slim` passes when it loads, paints,
builds a comparable DOM and throws no script error stock WebKit does not.

| Round | Pass | Fail | Blocked | What it found |
|---|---:|---:|---:|---|
| 1 | 35 | 62 | 3 | A linker option merged JavaScriptCore type descriptors; the JIT built a `Map` for `new Set` |
| 2 | 54 | 34 | 12 | `HTMLVideoElement`, `Audio` and friends did not exist |
| 3 | 86 | 1 | 13 | One site needed Media Source Extensions |
| 4 | 84 | 0 | 16 | A crash in software video upload (ikea.com); WebGL missing (baidu.com). Both fixed. |

- **Blocked (16):** stock WebKit did not get the real site from this machine
  either (Cloudflare challenges, bot walls, one unreachable server, one page
  that never yields). These are untested, not passed.
- **Media:** `scripts/media.sh` plays four generated samples, including one
  with non-square pixels, and checks they reach the end at the right size.
- **Guards in the smoke test:** a JIT-hot type check, a check that no two
  `ClassInfo` objects share an address, and a WebGL draw-and-read-back.

Not covered: interaction (nothing is clicked or logged in), pixel-level
layout equality, and playback on the sites themselves.

`slim` still lacks 27 script-visible interfaces stock WebKit has: capture
and recording (15), WebCodecs (7), Gamepad (4) and XSLT (1).

## Chrome DevTools Protocol

WebKit has no CDP; it has its own inspector protocol. `src/cdp` is a bridge
compiled into the engine library: it serves CDP's HTTP and WebSocket
endpoint, answers navigation, input, targets, cookies and dialogs from the
WebKit API, and translates the rest to and from WebKit's protocol.
`docs/CDP.md` has the design, the list of what is covered and what is not.

Started with `ZEBKIT_CDP_PORT=9222` in the environment of any embedding
application, or with `zebkit_cdp_listen()`. It listens on 127.0.0.1
and refuses requests that come from web pages. It has no password; with
discovery switched off (`ZEBKIT_CDP_DISCOVERY=0`) only a client that was
given the browser's address, which contains a random id, can connect.

| Test (`scripts/cdp.sh slim --clients`) | Result on `slim` |
|---|---|
| Raw protocol, `tests/cdp/protocol.py` | 43 of 43 checks |
| The same checks against Chrome 155, `scripts/cdp-reference.sh` | 41 of 41; the 2 that are about zebkit's own choices are left out |
| Messages that depart from Chrome's protocol definition | none, in either browser |
| Endpoint without discovery, `tests/cdp/private.py` | passes |
| Puppeteer 24.10 (`puppeteer.connect`), `tests/cdp/puppeteer.js` | 17 of 17 steps |
| Playwright 1.52 (`connectOverCDP`), `tests/cdp/playwright.js` | 17 of 17 steps |

The client tests do what an agent does: open a page, navigate, evaluate
script, click, type, work inside a frame, take screenshots, go back and
forward, move between sites, answer dialogs, follow a popup, upload a file,
use cookies and an isolated browser context.

Also tried by hand, not part of the scripts: both clients loading
example.com, Wikipedia, Hacker News, GitHub, Google, Bing, DuckDuckGo,
Amazon, YouTube and Reddit one after another in the same page (each a
change of site, and of process), a typed Wikipedia search with Puppeteer
and a link click on Hacker News with Playwright. All loaded; Google, Amazon
and Reddit answered some of the repeated visits with their bot-check pages.

What it is not:

- **A subset of CDP.** Not implemented: request interception (`Fetch`),
  the accessibility tree and DOM snapshots, touch input, device and network
  emulation beyond viewport size, PDF output, tracing, the script debugger,
  download events. Unimplemented commands return CDP's "wasn't found"
  error.
- **Not Chrome.** The user agent is WebKit's, sites serve what they serve
  Safari, and error text and stack traces are JavaScriptCore's.
- **Tested with one version of each client**, on local pages plus the sites
  above. Other clients (browser-use, Stagehand, chrome-devtools-mcp and the
  like) have not been run; those that read pages through the accessibility
  tree or `DOMSnapshot` will not work until those are added.

While a client is attached to a page, the page's back/forward cache is
off, because WebKit's inspector does not report the script contexts of a
page restored from it.

### Review for production use (2026-10-10)

A read of all of `src/cdp` and tests aimed at failures, not at the usual
path, found the following. All are fixed and each has a check in
`tests/cdp/protocol.py` unless noted.

| Found | Effect before the fix |
|---|---|
| Every page opened at `about:blank` left a web process running after it closed. An upstream bug in how WebKit hands out its spare process, not in the bridge (`patches/0005`) | 300 pages opened and closed left 303 processes of 100 MiB and more. Now 4, and the application's memory stays flat. Measured by hand; no scripted check |
| Answers to mouse and key commands could overtake each other | Puppeteer believed a button was still down; about one run in six of the client suite failed |
| After the page's process died, nothing was ever answered again, `Page.navigate` included | A client hung until its own timeouts, and could not recover the page |
| `Page.stopLoading` left the `Page.navigate` it stopped unanswered | The client's navigation hung |
| A dialog a client was shown and did not answer stayed open after the client left | The page's script was blocked for good |
| `GET /json/new` and requests carrying a web page's `Origin` were served; any id was accepted on the browser socket | A web page open anywhere on the machine could open pages in the browser |
| The back/forward cache was switched off and on per page on a settings object views share | The second attached page could get the cache back while attached; history navigation then lost its script contexts. No scripted check |
| `DOM.setFileInputFiles` clicked whatever element it was given | A wrong id pressed a button |
| Certificate checks a client turned off, and the view size it set, stayed that way after it left | The application's own pages kept running without certificate checks |
| Bookkeeping per document (request ids, document addresses, requests in flight) was dropped only when the page changed process | Slow growth on a page used for days; a request cut off by a navigation could keep "network idle" from ever being reported |
| Integer ids from clients and pages were used as map keys of the same width as the map's two reserved values | Not shown to fail in this build; keys are now wider than any id |
| Popups became hidden views of the endpoint whether or not a client was connected | Views nobody would ever close |
| Full-page screenshots had no size limit; incoming messages had none either | Memory use set by the page or the client |

Still true after the review, and the reasons it is not yet what a product
should promise without saying so:

- It has run for minutes at a time, not days. The longest run is 300 page
  lifecycles in 85 seconds.
- One version each of two client libraries. No agent framework has been
  run against it.
- No password on the port. Discovery can be switched off; a transport that
  is not a TCP port (a pipe, or calls inside the process) does not exist.
- Some setup commands are answered with success and do nothing
  (`docs/CDP.md` lists them), among them granting permissions and
  redirecting downloads.
- Verified on this one machine and its glibc only.

## Portability

| Question | Answer |
|---|---|
| Which glibc? | 2.17 or newer. Built with a pinned cross toolchain (`toolchain.lock`) that targets glibc 2.17 whatever the build host runs. |
| What else must the system have? | Nothing. The package's only hard dependencies are glibc's own libraries. |
| What does it use if present? | `libwayland-client`, `libX11`, `libgbm`, `libdrm`, `libasound`, and the EGL/GLES driver. Each is loaded on first use (`deps/syslibs`); without it the feature that needs it is unavailable and the engine carries on. |
| C++ runtime, keyboard handling? | libc++ and libxkbcommon are linked in. |
| Does it run from any directory? | Yes; helpers, the injected bundle and the TLS plugin are found next to the engine. |
| Fonts, CA certificates, keyboard layouts? | Read from the usual system locations. |

### Display

The engine picks a display backend when it starts, in this order:

1. **Wayland**, if `libwayland-client` (1.20 or newer) is installed and a
   compositor answers.
2. **X11**, if `libX11` is installed and `DISPLAY` leads to a server. This
   backend is ours (`patches/2004`): one window per toplevel, frames copied
   with `XPutImage`, core-protocol mouse, wheel and keyboard input.
3. **Offscreen** otherwise. The page loads and renders with no window; the
   embedding application can read frames back.

`WPE_PLATFORM=wayland|x11|headless` forces one. `scripts/display.sh` checks
all of this with a private X server and a private headless compositor,
19 checks: Wayland is preferred when both are up; X11 draws the right pixels
and delivers clicks, motion, wheel and typed text; with libwayland-client
"not installed" it falls back to X11; with no optional library at all it
still starts and renders offscreen.

Limits of the X11 backend: 24- or 32-bit TrueColor visuals only, no input
methods, no clipboard, no custom cursors, no HiDPI scaling, and frames are
copied through the X protocol rather than shared, so large windows cost
CPU.

### Not verified

The build machine cannot run containers, so:

- **It has not been started on an actual glibc 2.17 system.** The symbol
  versions say it should load; nothing has run it there.
- **Kernel features.** A glibc 2.17-era kernel (3.10) may lack system calls
  WebKit uses, such as `memfd_create`.
- **Old graphics drivers.** Rendering needs EGL; how old a driver works is
  untested. Without `libgbm` the engine falls back to copying frames.
- **"Library not installed"** was tested by making the loader refuse a
  library (`ZEBKIT_WITHOUT_LIBRARIES`), not on a machine that lacks it.

## Speed

`scripts/bench.sh`, milliseconds, best of three, lower is better.

| Build | loop | objects | strings | json | regex | sort | dom |
|---|---:|---:|---:|---:|---:|---:|---:|
| `baseline` (`-O2`, all JIT tiers) | 38 | 48 | 17 | 220 | 210 | 62 | 415 |
| `slim` (`-Oz`, baseline + DFG JIT) | 179 | 72 | 27 | 406 | 342 | 102 | 767 |

`slim` got slower with the toolchain change: before it, the same benchmark
gave json 287, regex 248, sort 66, dom 670. The cause is not isolated; the
candidates are clang 21's `-Oz`, libc++ with its hardening checks, and the
loss of function merging.

Rejected for speed: interpreter-only JavaScript (4 MiB, 2x to 10x slower)
and the machine outliner (1.8 MiB, about 2x slower JSON and DOM).

## Decisions

- **glibc 2.17 floor, media playback required, about 61 MiB accepted before
  those two.**
- **The JIT stays; no web platform features are removed for size.**
- **No linker identical-code folding.** Under ThinLTO it merges data that
  WebKit compares by address.
- **ICU data covers English, Chinese, Japanese and Korean.**
- **WebGL is on:** baidu.com's scripts throw without it.
- **Media is GStreamer plus FFmpeg, linked statically**, because a
  glibc 2.17-era system cannot be assumed to have usable versions.
- **Display is chosen at run time: Wayland, then X11, then offscreen.**
  Nothing but glibc is a hard requirement.
- **CDP is built into the engine** as a bridge onto WebKit's inspector, so
  the inspector backend stays in and the size patch that removed its agents
  (`1002`) is gone. The endpoint is off unless the application or the
  environment turns it on.

## Open questions for the project owner

1. **Size.** 83.1 MiB is what media, glibc 2.17, WebGL and CDP cost with
   everything else as it was. Is that acceptable, or should one of the items
   under "What could come back" be pursued?
2. **Codec licensing.** H.264 and AAC decoders are patented in some
   countries; shipping them may need a licence.
3. **Speed.** The toolchain change cost roughly 15% to 40% on the
   C++-bound benchmarks. Worth investigating now, or later?

## Trade-offs in `slim`

| Choice | Where | Consequence |
|---|---|---|
| No capture, recording, WebRTC, WebCodecs, encrypted media, speech | `profiles/slim.cmake` | No camera/microphone, no DRM video |
| GStreamer without its GL path | `profiles/slim.cmake` | Video frames are uploaded from memory; no zero-copy |
| Audio output through ALSA only | `deps/gstreamer` | Works through PulseAudio/PipeWire's ALSA plugin where present; silent where `libasound` is missing |
| X11 backend is a simple one | `patches/2004` | See its limits under Portability |
| No WebXR, GPU process | `profiles/slim.cmake` | Feature absent |
| No FTL JIT or WebAssembly JIT tiers | `profiles/slim.cmake` | Slower hot loops; wasm runs interpreted |
| `-Oz` instead of `-O2` | `profiles/slim.cmake` | Up to 2x slower on C++-bound work |
| No Web Inspector front end, remote inspector server, WebDriver, JIT disassembler | `profiles/slim.cmake` | Automation and debugging go through CDP (`docs/CDP.md`) only |
| No PDF.js, XSLT, MHTML, gamepad, spellcheck, content extensions, accessibility, AVIF, JPEG XL, colour management | `profiles/slim.cmake` | Feature absent |
| No unwind tables, no assertion strings | `profiles/slim.cmake`, `profiles/include/size-prelude.h` | A crash gives no native stack trace or message; `tools/crashsite` recovers an approximate one |
| simdutf without its AVX-512 path | `profiles/slim.cmake` | AVX2 and SSE paths are used instead |
| ICU locale data for English and CJK only | `deps/icu/filter.json` | `Intl` formats other locales with root-locale rules |
| GnuTLS without p11-kit, IDN, GMP | `deps/gnutls`, `deps/nettle` | No smart-card trust; slower RSA handshakes |
| libsoup without Kerberos or NTLM | `deps/libsoup` | No Negotiate/NTLM HTTP authentication |
| GLib without SELinux, libmount, xattr | `deps/glib` | No mount monitoring through GIO |
| libgcrypt with only the WebCrypto algorithms | `deps/libgcrypt` | A new WebCrypto algorithm needs the list extended |
| FFmpeg with eight decoders | `deps/ffmpeg` | Other codecs (HEVC, AV1, ...) do not play |

### Held to Chrome's definition and Chrome's behaviour (2026-10-10)

Until this point the bridge was written from knowledge of the protocol and
checked only against itself and two client libraries. Chrome's
implementation cannot be compiled against WebKit (it is part of Blink and
V8), so what was taken from it is what can be:

- **Its protocol definition**, vendored unchanged in
  `deps/devtools-protocol` (revision r1716261, 1.6 MiB of JSON, not
  shipped). The endpoint's parameter checks are generated from it, and the
  tests compare every message with it.
- **Its behaviour.** `scripts/cdp-reference.sh` runs the raw checks against
  Chrome. On the first run Chrome failed 15 of the 34 checks, and passed
  the definition check with no departures where zebkit had 10. Some checks
  were wrong; most pointed at zebkit. Side-by-side runs of about 150
  commands and their events found the rest.
- **One more engine patch**, where WebKit lacked what CDP needs
  (`patches/2006`, the same additions Playwright's WebKit has).

What that found, all fixed, each with a check that Chrome passes too:

| Where zebkit differed from Chrome | Before |
|---|---|
| Isolated worlds | Emulated in the page's own world: the page could see and overwrite what a client put there, and a script registered for a world ran in the page. Now WebKit user worlds, separate as in Chrome, with nodes carried between worlds by `DOM.resolveNode` |
| Wrong or missing parameters | Accepted and guessed at (`Runtime.evaluate` without an expression returned `undefined`). Now `-32602 Invalid parameters`, checked against the definition |
| Messages that are not commands (bad JSON, no id, no method, unknown session) | Dropped without an answer, so a client waited for ever. Now answered with Chrome's codes |
| A document changing its own address (fragment, history API) | Not reported at all, so a client's idea of the page's address went stale on single-page sites. Now `Page.navigatedWithinDocument` |
| State set while a new page was still being set up | Lost: a popup did not get the scripts and worlds a client registered in that moment. Found by Playwright's popup test once worlds were real |
| A client that attaches to a loaded page | Was not told the page had loaded. Lifecycle events are now replayed as in Chrome |
| Events with mandatory fields missing or fields Chrome does not have | Dialog events without `frameId`, `navigatedWithinDocument` without its type, object previews without `overflow`, cookies and nodes with fields of WebKit's own |
| How objects are described | Nodes as `<div id="x">` for Chrome's `div#x`; typed arrays, maps, sets, promises, classes, symbols and `-0` with other types and descriptions than Chrome's; the prototype listed as a property |
| About thirty error answers | Other codes and words than Chrome's, or success where Chrome fails (an invalid URL, a malformed object id, focus on what cannot be focused, a cookie with no address, an image format that does not exist, a target that is not there) |
| Navigation and contexts | A `commit` lifecycle event Chrome does not send; contexts reported destroyed one by one where Chrome clears them; `DOM.getDocument` one level deeper or shallower than Chrome's |
| Failed requests | WebKit's sentence for the error and type `Other`; now Chrome's error names (`net::ERR_CONNECTION_REFUSED`) and the request's type |

What zebkit still does differently is listed in `docs/CDP.md` under
"Differences from Chrome".

Limits of this work: the comparison covers the commands the tests and the
side-by-side runs exercise, not all 143 that are implemented; Chrome's
answers were taken from one version (155); and what Chrome does that no
check asks about is still unknown.

## What exists

- **One-command builds:** `mise.toml` with the tasks `r` (release, into
  `dist/release`), `d` (with debug information, into `dist/debug`) and
  `mise example:linux:basic` and `mise example:ffmpeg` (build and run the
  programs in `examples/`); they call `scripts/dist.sh` and
  `scripts/example.sh`.
- **Pipeline:** `bootstrap-linux.sh`, `fetch-toolchain.sh`,
  `fetch-upstream.sh`, `fetch-cdp-clients.sh`, `build-deps.sh`, `build.sh`, `smoke.sh`, `media.sh`,
  `display.sh`, `cdp.sh`, `bench.sh`, `package.sh`, `measure.sh`, `compat.sh`,
  `compat-report.py`,
  `symbolize.py`, `codesize.py`, `mapsize.py`.
- **Profiles:** `baseline` and `slim`, plus `profiles/include/size-prelude.h`.
  `slim-dev` is `slim` without link-time optimization, for working on the
  engine: a change relinks in seconds instead of five minutes.
- **CDP endpoint:** `src/cdp` (first-party source, compiled into the engine),
  `tools/cdphost` (a minimal embedding application), `tests/cdp`, and
  `deps/devtools-protocol`, Chrome's protocol definition, which the
  endpoint's parameter table is generated from (`gen-cdp-protocol.py`) and
  the tests check against. `cdp-reference.sh` runs the checks against
  Chrome.
- **Toolchain:** Zig 0.16.0 (clang 21, lld, libc++), pinned by SHA-256.
- **Dependencies:** 29 recipes in `deps/`, sources pinned by SHA-256.
  `deps/syslibs` builds lazy-loading stand-ins for optional system libraries
  with `tools/syslib-stubs`.
- **Patches (16):** six build or bug fixes (`0xxx`), two size patches
  (`1xxx`), eight port and feature patches (`2xxx`). `0005` stops WebKit from
  leaving a web process behind for every page that starts at `about:blank`;
  `0006` fixes a crash at exit in any application that has shown a window on
  Wayland (EGL was shut down after the display connection it used was
  closed); `2002` is no longer used by `slim`; `2005` hooks `src/cdp` into
  the build and into `WebKitWebView`; `2006` gives WebKit's inspector the
  isolated worlds CDP needs; `2007` names the shipped files after zebkit
  (`libzebkit.so.1`, `zebkit-web`, `zebkit-network`, `libzebkit-bundle.so`),
  the API and the symbols keeping WebKit's names; `2008` exports the entry
  points for libraries inside the engine.
- **Libraries inside the engine, for applications:** FFmpeg as
  `zebkit::vendor::ffmpeg` (`docs/VENDOR.md`), generated by
  `scripts/gen-vendor.py`, checked by `scripts/vendor.sh`. 938 functions,
  under link names of zebkit's own so that another FFmpeg in the same
  process cannot collide. Beyond what the engine needs it has file reading
  and writing, scaling and FFmpeg's own encoders. Costs 1.8 MiB in all.
- **Rust:** `rust/zebkit-rs` (raw bindings; its build script downloads the
  engine of the same version from the GitHub release and checks it against
  a SHA-256 fixed in the crate), `rust/zebkit-build` (puts the engine beside
  an application's executable) and `rust/zebkit` (the safe
  interface: pages, windows, main loop, CDP endpoint, the engine's FFmpeg).
  `cargo test` loads a page headlessly, checks the CDP endpoint lists it and
  looks at FFmpeg's decoders, on the real engine. A first cut: script
  evaluation, settings, cookies and FFmpeg decoding are not wrapped yet.
- **Releases:** `scripts/release.sh` makes the archive of `dist/release`
  that the crates download (25 MiB on Linux, 28 MiB on macOS,
  reproducible) and, with `--publish`, the GitHub release, which holds
  the archives of both systems. `v0.0.1-alpha.2` is the current one. The
  repository is private, so the crates cannot download from it yet.
- **Examples:** `examples/window.c`, a small browser window, with
  `examples/window.m` as the same program for macOS, and
  `examples/ffmpeg.cpp`, which decodes with the engine's FFmpeg, one
  file for both systems. The Rust crate has the same two.
- **Tests:** smoke, media, display, CDP and benchmark pages; `tools/pageprobe`;
  `tools/crashsite`; the site list in `assets/WEBSITE-TOP-100.json`.

`out/slim-nojit` (3.7 GiB) and
`out/deps/prefix-host-glibc` are leftovers and can be deleted. `out/slim-dev`
is the development build and can be rebuilt in about 20 minutes.

## Roadmap

1. ~~Linux baseline and slim build.~~ Done.
2. ~~ICU and dependencies from source.~~ Done.
3. ~~Size investigation and speed measurement.~~ Done.
4. ~~Relocatable package.~~ Done.
5. ~~glibc 2.17 build.~~ Done at the symbol level; not run on such a system.
6. ~~Media playback.~~ Done.
7. ~~Top-100 site test.~~ 84 pass, 0 fail; 16 need another network.
8. ~~Run-time display choice with no hard system dependencies.~~ Done.
9. ~~Chrome DevTools Protocol endpoint.~~ Done for navigation, script,
   input, DOM, network observation, cookies, screenshots, targets.
10. **CDP gaps, in the order agents are likely to hit them:** the
    accessibility tree and `DOMSnapshot`; request interception (`Fetch`);
    touch input; device, locale and timezone emulation; downloads and
    permissions, which are acknowledged today and not applied. Run the agent
    frameworks themselves against it, and run it for days. Extend the
    comparison with Chrome to every implemented command.
    A transport without a TCP port, for applications that drive the engine
    from inside their own process.
11. **Verify on old distributions** in containers or VMs.
12. **Resolve the open questions.**
13. **macOS port.** `docs/MACOS.md`: WebKit's own Mac port, built from the
    same revision. It builds, is slimmed and packaged, and the CDP
    endpoint, FFmpeg for applications, the examples and the Rust crates
    work there. Left: feature decisions, the media test, publishing the
    archive, signing.
14. **Windows port.**
15. **Embedding API** that hides GLib (the CDP calls in
    `src/cdp/zebkit-cdp.h` are its first piece).
16. **CI:** build, smoke, media, CDP, site test, benchmark and size check.

## Host notes (this build machine)

- `sudo` here is `fakeroot` and parts of the filesystem are root-owned, so
  `bootstrap-linux.sh` needs
  `APT_OPTS="-o Dir::Cache::archives=$PWD/.cache/apt -o APT::Sandbox::User=$USER -o Dpkg::Options::=--path-exclude=/usr/share/sgml/* -o Dpkg::Options::=--path-exclude=/usr/share/doc/*"`.
- `/var/lib/dpkg/status` was root-owned and blocked `dpkg`; it was replaced
  with an identical user-owned copy. The original is kept at
  `/var/lib/dpkg/status.zebkit-orig`.
- Four packages are unpacked but not configured because their post-install
  scripts need root: `libdbus-1-dev`, `sgml-base`, `libatspi2.0-dev`,
  `libatk-bridge2.0-dev`. `bootstrap-linux.sh` exits non-zero because of
  them even when everything it was asked for is installed.
- Compilers crash at random here, on a different file each time, with both
  the host's clang 19 and the toolchain's clang 21. `build.sh` retries a
  failed pass up to three times.
- The checkout is reachable as `the checkout` and through a symlink. The
  scripts normalize this; a bare `cmake` run from the symlinked path breaks
  the build tree's configuration.
- No containers or user namespaces, so nothing can be run under another
  distribution's userland here.
- Stock WebKit on this host has no H.264 decoder installed, so the
  `baseline` profile fails the H.264 samples in `scripts/media.sh`.
- A full build of one profile takes about 40 minutes.
