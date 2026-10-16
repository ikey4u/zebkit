# Progress

Goal: a self-contained, portable, trimmed WebKit engine for Linux, Windows
and macOS that plays media, runs on old systems and works on the web's
most-visited sites, kept as small as those requirements allow. It is meant
to be embedded in agent applications, which drive it over the Chrome
DevTools Protocol.

Requirements, set by the project owner:

- On Linux, runs on glibc 2.17 and newer, with as little as possible from
  the system.
- Media playback works.
- ICU data covers at least English, Chinese, Japanese and Korean.
- No size cut that costs a lot of speed for a little size. The JIT stays.
- No removing web platform features that common sites rely on.
- Correctness is checked against the top 100 sites (`docs/COMPAT.md`).
- The engine speaks the Chrome DevTools Protocol, so that agent tooling
  (Puppeteer, Playwright, raw CDP clients) can drive it (`docs/CDP.md`).
- Size: about 61 MiB was accepted before media playback and glibc 2.17 were
  required. Those two put it at 80.3 MiB on Linux; `docs/LINUX.md` has
  what that buys and what could come back.

Last updated: 2026-10-12

## Platforms

Each platform has a document of its own, with its state, sizes, speed,
what was decided for it and what is left.

| | Linux | macOS | Windows |
|---|---|---|---|
| Document | [`docs/LINUX.md`](LINUX.md) | [`docs/MACOS.md`](MACOS.md) | [`docs/WINDOWS.md`](WINDOWS.md) |
| State | Released, `v0.0.1-alpha.2` | Released, `v0.0.1-alpha.2`; `Zebkit.framework` is since then, not released yet | Started: the engine builds with the CDP endpoint and passes its tests under Wine; not slimmed, nothing released |
| Engine | WPE WebKit 2.54.1 | WebKit's Mac port, the same release | WebKit's Windows port, the same release, cross-built on Linux |
| Target | x86_64, glibc 2.17 or newer | Apple Silicon, macOS 27 | x86_64, Windows 10 1809 or newer; run under Wine only so far |
| Form | `libzebkit.so.1` and two helper programs | `Zebkit.framework`, with the engine's frameworks and helper processes inside it | |
| `slim` package | 83.1 MiB | 92.3 MiB | `slim` has the functions, not yet the size: 160.3 MiB (`baseline` 165.8) |
| Release archive | 24.7 MiB | 27.9 MiB | |
| JIT | Baseline and DFG | All tiers | All tiers |
| Networking, media, fonts | Built from source, in the package | The system's | Networking built from source; media and fonts the system's |
| Smoke test | Pass | Pass | |
| CDP: raw checks, Puppeteer, Playwright | 43 of 43, 17 of 17, 17 of 17 | 42 of 43 (the one is the test machine's network), 17 of 17, 17 of 17 | 43 of 43, 17 of 17, 17 of 17, under Wine |
| Media test | Pass | Pass | |
| Top-100 sites against stock WebKit | 84 pass, 0 fail, 16 blocked | 89 pass, 0 fail, 11 blocked | |
| FFmpeg for applications (`docs/VENDOR.md`) | Yes | Yes, the same | |
| Rust crates, examples | Yes | Yes, the same | |
| Debug build (`mise run d`) | Yes | No profile for it | |
| Signed for distribution | Not applicable | No; ad hoc only | |

What is shared by all platforms is below: the CDP endpoint, the build
pipeline, the Rust crates, releases, decisions and the roadmap. Other
documents: `docs/CDP.md` (the endpoint), `docs/COMPAT.md` (the site
comparison), `docs/DEPS.md` (libraries built from source), `docs/VENDOR.md`
(libraries inside the engine for applications), `docs/reports/` (generated
size and comparison reports, macOS's in `docs/reports/macos/`).

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

The bridge is one piece of code for every platform (`CDPView.cpp` and what
it shares), with a layer for each engine API under it: GLib on Linux,
Cocoa on macOS. The results below are Linux's; macOS passes the same
suites but for one raw check that depends on the test machine's network
(`docs/MACOS.md`).

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


Two records of how the endpoint got here are in `docs/CDP.md`: the
comparison with Chrome's protocol definition and with Chrome itself, and
the review for production use, each with what it found.

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
- **macOS is WebKit's own Mac port, not WPE**, with networking, media and
  fonts from the system; `-Oz` with the JavaScript engine at `-Os`, and
  all JIT tiers. `docs/MACOS.md` has these and the rest.

## Open questions for the project owner

1. **Size.** 83.1 MiB is what media, glibc 2.17, WebGL and CDP cost with
   everything else as it was on Linux. Is that acceptable, or should one of
   the items under "What could come back" in `docs/LINUX.md` be pursued?
2. **Codec licensing.** H.264 and AAC decoders are patented in some
   countries; shipping them may need a licence.
3. **Speed.** The toolchain change cost roughly 15% to 40% on the
   C++-bound benchmarks. Worth investigating now, or later?
4. **macOS.** WebRTC and the features only the Mac port has, Intel, the
   oldest macOS to support, signing: listed in `docs/MACOS.md`.
5. **Windows.** Everything after the baseline build; `docs/WINDOWS.md` has the list and the questions.

## What exists

The pipeline is one set of scripts for every platform; where a script is
Linux's only, `docs/MACOS.md` says so.


- **One-command builds:** `mise.toml` with the tasks `r` (release, into
  `dist/release`), `d` (with debug information, into `dist/debug`, Linux
  only so far) and `mise example:linux:basic`, `mise example:macos:basic`
  and `mise example:ffmpeg` (build and run the programs in `examples/`);
  they call `scripts/dist.sh` and `scripts/example.sh`. mise also brings
  the tools the tasks need and a system may lack (`gh`, `xz`).
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
- **Patches (18):** six build or bug fixes (`0xxx`), two size patches
  (`1xxx`), ten port and feature patches (`2xxx`). `0005` stops WebKit from
  leaving a web process behind for every page that starts at `about:blank`;
  `0006` fixes a crash at exit in any application that has shown a window on
  Wayland (EGL was shut down after the display connection it used was
  closed); `2002` is no longer used by `slim`; `2005` hooks `src/cdp` into
  the build and into `WebKitWebView`; `2006` gives WebKit's inspector the
  isolated worlds CDP needs; `2007` names the shipped files after zebkit
  (`libzebkit.so.1`, `zebkit-web`, `zebkit-network`, `libzebkit-bundle.so`),
  the API and the symbols keeping WebKit's names; `2008` exports the entry
  points for libraries inside the engine; `2014` adds child toplevels
  (`wpe_toplevel_new_child`, `_set_child_geometry`, `_set_child_visible`):
  a view drawn inside another view's window at a rectangle, as a Wayland
  subsurface or an X11 child window, keyboard focus following the last
  click, for applications that embed a browser in their window
  (the application's, checked on X11 under Xvfb with pixels and xdotool input,
  and on Wayland under labwc). The X11 backend now redraws on Expose.
  `2015` takes files dropped from other applications: a `file-drag`
  signal on `WPEView` (enter, motion, leave, drop with the URIs), from the
  Wayland data device and from XDND (version 5) on X11; only
  `text/uri-list`, only as a copy, and refused by a view nothing listens
  to. On Wayland a drag no longer replaces the clipboard's offer. Checked
  with test drag sources (Xlib; a Wayland client driving a virtual
  pointer under labwc): the application imports what is dropped, MiniBrowser
  refuses, pasting from another application still works after a drag.
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
13. ~~macOS port.~~ `docs/MACOS.md`: WebKit's own Mac port, built from the
    same revision, slimmed, packaged and released, with the CDP endpoint,
    FFmpeg for applications, the examples, the Rust crates and the tests
    as on Linux. Left there: feature decisions, signing, a debug build.
14. **Windows port.** `docs/WINDOWS.md`. Started: the toolchain, the build, the feature set of the other platforms but for Web Audio and Media Source Extensions, and the CDP endpoint, tested under Wine.
15. **Embedding API** that hides GLib (the CDP calls in
    `src/cdp/zebkit-cdp.h` are its first piece).
16. **CI:** build, smoke, media, CDP, site test, benchmark and size check.
