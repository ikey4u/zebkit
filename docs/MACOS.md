# macOS port: plan and state

zebkit on macOS does not exist yet. This is the plan, what has been
prepared for it on Linux, and where to start on a Mac. Whoever picks the
work up, on whatever machine, should need nothing but this file and the
repository.

## Decisions taken

- **The engine is WebKit's own Mac port**, built by us from the same WebKit
  revision as the Linux engine, as our own `WebKit.framework`. Not WPE
  ported to macOS: WPE needs GLib, libsoup, GStreamer and a window backend
  that nobody maintains there. Not the WebKit that ships with macOS: the
  CDP endpoint is compiled into the engine and uses what that framework
  does not expose.
- **Networking, media, fonts, text and graphics are the system's** on
  macOS (CFNetwork, AVFoundation, CoreText, CoreGraphics, Metal). The
  libraries in `deps/` are for Linux. The engine should come out much
  smaller than the Linux one.
- **The same checks decide when it is done**: `tests/cdp/protocol.py`
  against the engine and against Chrome, the Puppeteer and Playwright
  suites, smoke and media tests.

## Not decided

| Question | Why it matters |
|---|---|
| Apple Silicon only, or Intel too | Build time doubles, and the release archive is per architecture or universal |
| Oldest macOS to support | Sets the deployment target, and with it which system APIs the engine may use |
| FFmpeg for applications on macOS | The Mac engine does not contain FFmpeg. `zebkit::vendor::ffmpeg` is either absent on macOS or FFmpeg is linked in for applications only, at a cost in size |
| The C API | On macOS the engine's public API is Objective-C (`WKWebView`), not the GLib C API. The Rust crate can hide the difference; C applications would see two APIs unless a common C layer is written |

## What is needed

- A Mac. Building the Mac port needs Xcode and the macOS SDK, and cannot be
  done from Linux. Apple Silicon, a current macOS and Xcode, about 100 GB
  of free disk.
- For releases, later: an Apple Developer ID, to sign and notarize. The JIT
  needs the `com.apple.security.cs.allow-jit` entitlement in a hardened
  runtime. Not needed to develop.

## What is prepared

- **The CDP endpoint is split into portable code and a platform layer.**
  `src/cdp/CDPPlatform.h` is the line and lists what a platform supplies.
  Portable, 4300 lines: `CDPServer.cpp`, `CDPPage.cpp`, `CDPDomains.cpp`.
  Linux, 1300 lines: `CDPViewGLib.cpp` (the view, input, dialogs, cookies,
  view creation) and `CDPTransportGLib.cpp` (listener, HTTP, WebSockets,
  the C API). `scripts/cdp.sh` fails if a portable file uses a GLib,
  libsoup, WPE or WebKit-GLib API.
- Nothing else. The build scripts, profiles, patches, packaging and the
  Rust crates are Linux-only as they stand.

## The work, in order

1. **Source.** The pinned tarball (`upstream.lock`) is the WPE release and
   has no Mac port in it: no `Source/WebKit/UIProcess/Cocoa`, no Xcode
   projects. Get the WebKit git repository at the commit the 2.54.1 release
   was made from (the releases are tagged, `wpewebkit-2.54.1`; check), and
   teach `fetch-upstream.sh` a second source.
   *Unknown:* the 2.54 branch is maintained for the GLib ports only. The
   Mac port should build at the point the branch was cut, but nobody tests
   that. If it does not, the choice is between fixing it and moving both
   platforms to a revision where both build.
2. **Baseline.** Build the unmodified Mac port and load a page with
   WebKit's own MiniBrowser. Record its size. This is `baseline` for macOS.
3. **Apply the patches that are about the engine and not about Linux**
   (`0001`, `0002`, `0005`, `1001`, `1003`, `2006`). The rest are for the
   GLib ports.
4. **Slim.** Carry over what applies from `profiles/slim.cmake` (feature
   switches, `-Oz`, LTO, dead-code removal) to the Mac build, which is
   driven by Xcode configuration files, not CMake. Measure.
5. **The CDP platform layer.** Write `CDPViewCocoa.mm` and
   `CDPTransportCocoa.mm` against `CDPPlatform.h`:
   - the types (`PlatformView` is a `WKWebView`, and so on);
   - the listener and sockets (Network.framework has both HTTP-level
     sockets and WebSocket framing);
   - navigation, reload, size, focus through `WKWebView` and its delegates;
   - mouse and key events as `NSEvent`s delivered to the view;
   - dialogs through `WKUIDelegate`, file choosers likewise;
   - cookies through `WKHTTPCookieStore`;
   - view creation, with a `WKWebsiteDataStore` per browser context.
   When writing it, move what turns out to be common out of
   `CDPViewGLib.cpp` into the portable files: the bookkeeping of a
   navigation, the order of input answers, the key tables.
   Hook the sources into the framework's build and call `addView` and
   `removeView` from `WKWebView`'s creation and teardown, as `patches/2005`
   does for `WebKitWebView`.
6. **Tests.** `scripts/cdp.sh` and the others assume `cdphost`, a GLib
   program. Write the Cocoa equivalent (a window, a `WKWebView`, the
   endpoint), then run everything.
7. **Package.** A relocatable framework with its helper processes (XPC
   services on macOS) that works from inside an application bundle.
   `scripts/dist.sh`, `release.sh` and the mise tasks for macOS.
8. **Rust.** `zebkit-rs` gets bindings for the Mac engine, `zebkit` an
   implementation of `WebView`, `Window` and `MainLoop` on AppKit behind
   the same interface, `zebkit-build` the copying of a framework. Release
   archives for `aarch64-apple-darwin`.
9. **Signing and notarization.**

## Starting on the Mac

```sh
git clone git@github.com:ikey4u/zebkit.git && cd zebkit
cat docs/MACOS.md docs/PROGRESS.md      # this file, and the state of everything
```

Then step 1 above. Nothing in `scripts/` runs on macOS yet; the first
thing each of them does is refuse.
