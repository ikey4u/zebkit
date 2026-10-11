# macOS port: plan and state

zebkit on macOS is being built. This is the plan, what is done, and where
to start on a Mac. Whoever picks the work up, on whatever machine, should
need nothing but this file and the repository.

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
- **The same checks decide when it is done**: `tests/cdp/protocol.py`
  against the engine and against Chrome, the Puppeteer and Playwright
  suites, smoke and media tests.

## Not decided

| Question | Why it matters |
|---|---|
| Apple Silicon only, or Intel too | Build time doubles, and the release archive is per architecture or universal |
| Oldest macOS to support | Sets the deployment target, and with it which system APIs the engine may use. As built now it is macOS 27, the version of the SDK, which is what upstream builds. An older target means patching the code that assumes otherwise; how much there is has not been looked at |
| FFmpeg for applications on macOS | The Mac engine does not contain FFmpeg. `zebkit::vendor::ffmpeg` is either absent on macOS or FFmpeg is linked in for applications only, at a cost in size |
| The C API | On macOS the engine's public API is Objective-C (`WKWebView`), not the GLib C API. The Rust crate can hide the difference; C applications would see two APIs unless a common C layer is written |

## What is needed

- A Mac with Apple Silicon. Building the Mac port cannot be done from Linux.
- **Xcode 27** (Swift 6.3 or later, the macOS 27 SDK). Xcode 26.2 does not
  do: this revision of WebKit uses a Swift compiler flag that came with
  Swift 6.3, in two modules that cannot be switched off.
- **The Metal Toolchain**, a separate download, once per Xcode:
  `xcodebuild -downloadComponent MetalToolchain`. WebKit's graphics layer
  compiles its shaders with it. `scripts/build.sh` says so if it is missing.
- About 60 GB of disk for one profile's build tree.
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

Known gaps on macOS:

- WebP screenshots (no encoder; libwebp would have to be built in).
- Right-click: WebKit shows the system's context menu, which waits for the
  user. Not tested over CDP yet.
- Error names depend on the system language (see `docs/CDP.md`).
- Middle-button events are made with AppKit's general mouse-event call and
  have not been checked to arrive as button 1.

## The work that is left, in order

1. **CDP, the rest.** The gaps above, and a run on a Mac without a
   name-answering network tool.
2. **Slim.** A `slim` profile: what applies from `profiles/slim.cmake`
   (feature switches, `-Oz`, LTO, dead-code removal), and leaving out what
   an embedded engine does not need (WebKitLegacy, the inspector's
   interface, and whatever of WebRTC and WebGPU the standing rules allow).
   Measure.
3. **Media test** (`scripts/media.sh`), which on macOS exercises the
   system's decoders.
4. **Package.** A relocatable framework with its helper processes (XPC
   services on macOS) that works from inside an application bundle.
   `scripts/dist.sh`, `release.sh` and the mise tasks for macOS.
5. **Rust.** `zebkit-rs` gets bindings for the Mac engine, `zebkit` an
   implementation of `WebView`, `Window` and `MainLoop` on AppKit behind
   the same interface, `zebkit-build` the copying of a framework. Release
   archives for `aarch64-apple-darwin`.
6. **Signing and notarization.**

## Starting on a Mac

```sh
git clone git@github.com:ikey4u/zebkit.git && cd zebkit
scripts/fetch-upstream.sh
scripts/build.sh baseline
scripts/smoke.sh baseline
```

The scripts that are not ported yet (dependencies, packaging, release) are
for Linux only.
