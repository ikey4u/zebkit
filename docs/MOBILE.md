# Android and iOS

On Android and iOS zebkit is the same library, built without its engine:
`libzebkit.so` for an APK, `Zebkit.framework` for an iOS app. Pages are
drawn by the system's web view (Android's System WebView, which is
Chromium; WKWebView, which is WebKit), and zebkit's viewer gives them the
same global `zebkit` they have in zebkit's own views on Linux, macOS and
Windows: [docs/VIEWER.md](VIEWER.md).

```
                         libzebkit
  ┌──────────────────────────────────────────────────────────────┐
  │ what needs no engine (:engine_free)                          │
  │   <zebkit/pdf.h> PDFium   <zebkit/canvas.h> Skia             │
  │   <zebkit/text.h> HarfBuzz   <zebkit/media_tools.h> FFmpeg    │
  │   <zebkit/services.h> without HTTP   <zebkit/vendor/*.h>      │
  │   <zebkit/viewer.h> the page's zebkit in a system web view    │
  ├──────────────────────────────────────────────────────────────┤
  │ the engine: Chromium's content layer, Blink, V8               │
  │   views, windows, HTTP, hooks, automation, zebkit.ui          │
  │   Linux, macOS, Windows only (zebkit_build_engine)            │
  └──────────────────────────────────────────────────────────────┘
```

`zebkit_build_engine` is off by default on Android and iOS: the content
layer, Blink and V8 are not built there at all.

## Why without the engine

- **iOS**: guideline 2.5.6 of the App Store Review Guidelines: "Apps that
  browse the web must use the appropriate WebKit framework and WebKit
  JavaScript", with an entitlement for other engines in the EU and Japan
  only. PDFium, Skia, HarfBuzz, FFmpeg and ICU are libraries like any other,
  and PDFium is built without V8 and XFA (`pdf_enable_v8 = false`): a PDF's
  scripts are not run.
- **Android** allows an engine of the application's own, but zebkit has no
  port of Chromium's Android side (its Java, surfaces, input, lifecycle),
  and the engine would be tens of megabytes per processor. Android's System
  WebView is Chromium already.

## What there is without the engine

| | Android, iOS | Linux, macOS, Windows |
|---|---|---|
| `<zebkit/pdf.h>`, `<zebkit/canvas.h>`, `<zebkit/text.h>` | yes | yes |
| `<zebkit/media_tools.h>` | not yet (zebkit's FFmpeg configuration has no Android or iOS one) | yes |
| `<zebkit/services.h>`: compression, digests, AES-GCM, URLs, images, ICU | yes | yes |
| `zebkit_fetch()`, `zebkit_url_registrable_domain()` | no: the system's HTTP (the viewer's zebkit.fetch uses it) | yes |
| `<zebkit/vendor/*.h>` | all but LevelDB, and FFmpeg | all |
| `<zebkit/viewer.h>` | yes | yes |
| Views, windows, hooks, automation, `zebkit.ui`, zebkit-js | no: the system's web view | yes |

LevelDB is left out without the engine because Chromium's keeps its files
through Chromium's storage services, which bring Mojo and `//net`.

Nothing needs `zebkit_main()`. ICU's data is an `icudtl.dat` the application
carries: `zebkit_icu_init()` finds it in the bundle's resources on iOS;
`zebkit_icu_init_from_file(path)` takes it from anywhere (on Android, from
the APK's assets copied to a file, which `ZebkitViewer.initIcu()` does:
Chromium's own lookup there asks Java, which libzebkit does not have).

## From a release

Each release on GitHub has an archive for each:

- **`zebkit-<version>-android.tar.xz`**: `zebkit.aar` (`dev.zebkit.ZebkitViewer`,
  libzebkit for arm64 and x86-64, ICU's data), and `jni/<abi>/libzebkit.so`
  with `include/` for native code. An application adds the AAR:
  `implementation(files("libs/zebkit.aar"))`.
- **`zebkit-<version>-ios.tar.xz`**: a Swift package: `Package.swift`,
  `Sources/`, `Tests/` and `Zebkit.xcframework` (devices and the simulator).
  An application adds the unpacked directory as a local package (Xcode:
  File › Add Package Dependencies › Add Local), product `ZebkitViewer`.
- The Rust crate `zebkit-rs` downloads them for `aarch64-linux-android`,
  `x86_64-linux-android`, `aarch64-apple-ios` and `aarch64-apple-ios-sim`, as
  it does the desktop's.

## Building

| Profile | Output | Built on |
|---|---|---|
| `slim-android` | `libzebkit.so` (arm64; `target_cpu = "x64"` for the emulator), `zebkit.aar`, `zebkit_mobile_test` | Linux, with Android's NDK and SDK |
| `slim-ios` | `Zebkit.framework` for arm64 devices, and on a Mac `Zebkit.xcframework` | a Mac with Xcode: Chromium builds iOS only on macOS |
| `slim-ios-simulator` | `Zebkit.framework` for the simulator on Apple silicon: the xcframework's other half | a Mac with Xcode |
| `mobile-linux` | `libzebkit.so` without the engine for Linux, `zebkit_mobile_test` | Linux: to test that configuration here, not to ship |

```sh
ZEBKIT_TARGET_OS=android scripts/fetch-upstream.sh   # Android's NDK and SDK too
scripts/build.sh slim-android zebkit/engine zebkit/engine:zebkit_mobile_test
scripts/package-mobile.sh slim-android               # out/blink/slim-android/package
scripts/mobile-test.sh slim-android                  # on the device adb sees
```

`ZEBKIT_TARGET_OS` adds systems to the checkout's `target_os`; on a Mac,
`ZEBKIT_TARGET_OS=ios`, then:

```sh
scripts/build.sh slim-ios-simulator zebkit/engine
scripts/build.sh slim-ios zebkit/engine
scripts/package-mobile.sh slim-ios-simulator
scripts/package-mobile.sh slim-ios          # Zebkit.xcframework, also into bindings/swift
cd bindings/swift && xcodebuild test -scheme Zebkit -destination 'platform=iOS Simulator,name=iPhone 17'
```

The Swift package's tests run the viewer in WKWebView on the simulator.
The iOS profiles set `use_blink = true`: Chromium's own iOS build has no
Blink, and some libraries zebkit has (libjpeg-turbo) are built only with
it; nothing of Blink is built (GN finds no path from `//zebkit/engine` to
it). patches/1088 gives PDFium its Apple platform code on iOS, which
Chromium's build adds on macOS only. These profiles do not use PartitionAlloc as `malloc`
(`use_partition_alloc_as_malloc = false`): the library lives in an
application, whose allocator it keeps. They turn FreeType on, which
Chromium builds on desktop only and PDFium draws its text with.

A package has the library, `include/zebkit/` (the headers it has, and the
vendored libraries'), `icudtl.dat` and the licences. On Android,
`zebkit.aar` is all an application needs: `dev.zebkit.ZebkitViewer`,
`libzebkit.so` and `assets/zebkit/icudtl.dat`. On iOS, `Zebkit.framework`
has `Headers/` and a module map (`import Zebkit`); `bindings/swift` is the
Swift package with `ZebkitViewer`, over `Zebkit.xcframework`.

## Bindings

- **Swift** (`bindings/swift`): `ZebkitViewer`, and libzebkit's C API as the
  module `Zebkit`.
- **Java** (`bindings/android`): `dev.zebkit.ZebkitViewer`, and
  `dev.zebkit.Native` for what it is built on.
- **Rust**: `zebkit-rs` links `libzebkit.so` on Android and
  `Zebkit.framework` on iOS, from `ZEBKIT_DIR`, the repository's
  `out/blink/slim-android/package` and `slim-ios/package`, or else the
  release's Android or iOS archive, downloaded and checked against
  `engines.txt` as the desktop's are. The engine's functions are declared all
  the same: a program that calls one does not link.
