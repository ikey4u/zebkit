# examples

Small applications that embed zebkit.

| File | What it shows |
|---|---|
| `window.c` | A small browser window: a web page with a back button, a forward button and an address box above it. Shows creating a view, sizing and titling its window, navigating, and how a host and a script in the page talk to each other |

| `ffmpeg.cpp` | Using a library that is inside the engine: decodes an MP3 file to WAV with the engine's FFmpeg, through `zebkit::vendor::ffmpeg`, with no FFmpeg of the application's own (`docs/VENDOR.md`) |

Run them with `mise example:linux:basic` and `mise example:ffmpeg`, or
`scripts/example.sh`.

`tools/cdphost/cdphost.c` is another host: the same, plus the Chrome
DevTools Protocol endpoint, and an option for no window at all.

## Building and running one

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
