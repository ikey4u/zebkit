# examples

Small applications that embed zebkit.

| Example | Linux | macOS | What it shows |
|---|---|---|---|
| A small browser window | `window.c` | `window.m` | A web page with a back button, a forward button and an address box above it. Shows creating a view, sizing and titling its window, navigating, and how a host and a script in the page talk to each other |
| A library inside the engine | `ffmpeg.cpp` | `ffmpeg.cpp` | Decodes an MP3 file to WAV with the engine's FFmpeg, through `zebkit::vendor::ffmpeg`, with no FFmpeg of the application's own (`docs/VENDOR.md`) |

`window.c` and `window.m` are one program written twice, since the
engine's API is WPE WebKit's C API on Linux and `WKWebView` on macOS. They
take the same argument, open a window of the same size and behave the
same. The bar is the same script for both, in `window-bar.h`.
`ffmpeg.cpp` is one file for both.

Run them with `mise example:linux:basic` or `mise example:macos:basic`,
and `mise example:ffmpeg`, or with `scripts/example.sh`.

`tools/cdphost/cdphost.c`, and `cdphost.m` on macOS, is another host: the
same, plus the Chrome DevTools Protocol endpoint, and an option for no
window at all.

The Rust crate has the same two as `rust/zebkit/examples/window.rs` and
`codecs.rs`, each one file for both systems (`rust/README.md`).

## Building and running one on Linux

zebkit's API is WPE WebKit's C API (`WebKitWebView` and friends, on GLib).
The headers and pkg-config files are in a profile's stage directory, which
`scripts/build.sh` and any test script fill:

```sh
stage=out/slim/stage
deps=out/deps/prefix
export PKG_CONFIG_PATH=$stage/lib/pkgconfig:$deps/lib/pkgconfig
cc examples/window.c -o window $(pkg-config --cflags --libs wpe-webkit-2.0)
```

To run it from the build tree:

```sh
LD_LIBRARY_PATH=$stage/lib:$deps/lib GIO_MODULE_DIR=$deps/lib/gio/modules ./window https://example.com/
```

To ship it, put the executable next to the contents of the package
directory (`scripts/package.sh slim`, then `out/slim/package`) and link it
with `-Wl,-rpath,'$ORIGIN'`, so that it finds `libzebkit.so.1` beside
itself. The library finds its helper programs and its TLS module in the
directory it was loaded from.

The window appears on Wayland if the session has it, else on X11. With
neither, the view still exists and loads pages, without a window.

## Building and running one on macOS

zebkit is `Zebkit.framework`, and its API is `<Zebkit/Zebkit.h>`:
WebKit's `WKWebView` and the rest, with `zebkit-view.h` for a view whose
window the engine makes, as it does on Linux: on the screen when the
machine has a display, and nowhere otherwise. The package directory
(`scripts/package.sh slim`, then `out/slim/package`; or `dist/release`)
holds the framework:

```sh
package=$PWD/dist/release
clang -fobjc-arc examples/window.m -o window -F$package -framework Zebkit -framework Cocoa \
    -Wl,-rpath,$package -Wl,-dyld_env,DYLD_FRAMEWORK_PATH=$package/Zebkit.framework/Versions/A/Frameworks
./window https://example.com/
```

`-rpath` is what it is on Linux: where the program finds
`Zebkit.framework`. The last option is macOS's own. The engine inside
`Zebkit.framework` is made of frameworks with WebKit's names, and macOS
has a WebKit too, which the system's own libraries use; told this
directory, the loader takes the frameworks there in the system's place
for the whole process. A program linked without it does not start. To
ship the program, put the package directory beside it and name both
relative to the executable: `-Wl,-rpath,@executable_path/zebkit` and
`-Wl,-dyld_env,DYLD_FRAMEWORK_PATH=@executable_path/zebkit/Zebkit.framework/Versions/A/Frameworks`.
The engine's helper processes find the frameworks in the directory they
are in themselves.
