# zebkit 0.6.0 (draft)

**A draft.** What `main` has that the 0.5.0 release does not
(`git log v0.5.0..main`), written as the notes of the next release, 0.6.0.
Until that release nothing here is in a release archive, on crates.io, on
PyPI or tagged for Go. At the release this file becomes
`docs/releases/0.6.0.md`, and the parts marked **To fill at release** are
filled from that release's own build and runs.

zebkit is on five systems: Android and iOS get libzebkit without its engine,
with a viewer that gives pages in the system's web view the same global
`zebkit` they have in zebkit's views; macOS on Intel has an archive of its
own; and an application can load a bundled JavaScript library and call it as
it calls any library.

| Archive | System | Unpacked | `.tar.xz` |
|---|---|---:|---:|
| `zebkit-0.6.0-x86_64-unknown-linux-gnu.tar.xz` | Linux x86-64, glibc 2.17 or newer | **To fill at release** | |
| `zebkit-0.6.0-aarch64-apple-darwin.tar.xz` | macOS, Apple silicon | **To fill at release** | |
| `zebkit-0.6.0-x86_64-apple-darwin.tar.xz` | macOS, Intel | **To fill at release** | |
| `zebkit-0.6.0-x86_64-pc-windows-msvc.tar.xz` | Windows 10 or newer, x64 | **To fill at release** | |
| `zebkit-0.6.0-android.tar.xz` | Android 10 (API 29) or newer: arm64 and x86-64 (the emulator), without the engine | **To fill at release** | |
| `zebkit-0.6.0-ios.tar.xz` | iOS 15 or newer: devices and the simulator (arm64), without the engine | **To fill at release** | |

The Rust crates `zebkit`, `zebkit-rs` and `zebkit-build` 0.6.0 download these
archives, each checked against its SHA-256 in `bindings/rust/zebkit-rs/engines.txt`,
Android's and iOS's included. The Python binding `zebkit` 0.6.0 is the
binding alone; its engine is this release's archive. The Go module is tagged
`bindings/golang/v0.6.0`.

## What changed since 0.5.0

**Android and iOS** (`docs/MOBILE.md`). The same libzebkit, built with
`zebkit_build_engine = false`: no content layer, Blink or V8 (iOS's App
Store asks apps that browse the web to use WebKit; Android's System WebView
is Chromium already). It has PDF on PDFium, canvas, text, the services
without HTTP and the C libraries inside, and the viewer. Profiles
`slim-android`, `slim-android-x64`, `slim-ios`, `slim-ios-simulator`;
`scripts/package-mobile.sh` makes `zebkit.aar` (both Android ABIs,
`dev.zebkit.ZebkitViewer`, ICU's data) and `Zebkit.xcframework` with the Swift
package (`bindings/swift`). Android is built on Linux; iOS on a Mac
(`scripts/release-ios.sh`). `zebkit_icu_init_from_file()` loads ICU's data
where it is not beside the library.

**One page, every system: the viewer** (`docs/VIEWER.md`,
`<zebkit/viewer.h>`). A page in WKWebView or Android's WebView has the
global `zebkit` of zebkit's views: `zebkit.app.<name>()` for the
application's functions, `zebkit.pdf`, compression, crypto, images,
`url.parse`, `fs` and `fetch` (the system's HTTP), with the same grants per
origin. The engine's bridge and the viewer answer pages with the same code
(`src/engine/page_api.h`). Swift: `ZebkitViewer` (tests in WKWebView on the
simulator); Java: `dev.zebkit.ZebkitViewer` (over JNI); Rust:
`zebkit::viewer`.

**macOS on Intel.** `zebkit-0.6.0-x86_64-apple-darwin`, cross-built on Linux
as Apple silicon's is, against the same SDK (`scripts/zsdk-create.sh
macos-intel`), with zebkit's FFmpeg configuration for mac/x64.

**JavaScript libraries as libraries of the application**
(`docs/JS_LIBRARIES.md`, `<zebkit/library.h>`). An application loads a
bundled library (a folder with `index.mjs`) and calls its exported functions
from C, Rust, Python, Go or host JavaScript. Two backends: `isolate`, a V8
isolate of its own, and `page`, a hidden page for libraries that need a DOM
(Mermaid is the example). A code cache, idle shrinking, `instances`, the
application's functions (`host.call()`); `tools/js-bundle.mjs` bundles npm
packages, CommonJS ones too.

**Smaller.** The Android and iOS packages have no tools (zebkit-run,
zebkit-js, zebkit-mcp are the desktop's). LevelDB is left out without the
engine. PDFium's Apple platform code is built for iOS too (`patches/1088`).
