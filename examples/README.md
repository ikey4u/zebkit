# examples

Small programs on zebkit. Each builds against a package directory,
`out/blink/slim/package` after `scripts/package.sh slim` (`docs/BUILD.md`);
below, `$PKG` is that directory.

| Example | What it shows |
|---|---|
| `window.c` | A small browser window: a page with a back button, a forward button and an address box above it, the bar a script in the page that calls the program through `zebkit.app`. One file for Linux and macOS |
| `ui/todo.mjs` | An application in JavaScript with native widgets: `zebkit.ui`, Chromium's views driven by signals, no web page (`docs/API.md`, "Native interface") |
| `pdf-plugin/` | A plugin of the PDF library's hooks, in C (`plugin.c`) and in JavaScript (`plugin.mjs`): a kind of annotation with its own appearance, an undo history from the operations hooks are told of, text for pages without any |
| `gpui-browser/` | A tabbed browser in GPUI-KIT (Rust): GPUI's window, tabs, toolbar and dialogs; the engine in GPUI's loop, its pages native children of GPUI's window; the engine's questions (dialogs, permissions, downloads, sign-in) answered with GPUI-KIT dialogs |
| `native-host/` | Toolkit-owned GTK, Qt, AppKit and winit windows with a web pane and native PDF pane; event-driven wakeup/pump integration, keyboard/IME and popup layers |
| `offscreen-host/` | A page drawn off-screen in one process (`render.c`) and shown in another that links nothing of zebkit's (`show.c`): frames as IOSurfaces or shared memory passed over a socket, input sent back, the reference protocol in `protocol.h` (`docs/API.md`, "Off-screen views") |
| `ffmpeg.cpp` | A library inside the engine: an MP3 decoded to WAV with the engine's FFmpeg, through `zebkit::vendor::ffmpeg`, with no FFmpeg of the program's own |
| `../bindings/rust/zebkit/examples/window.rs` | A window with a web page, in Rust |
| `../bindings/rust/zebkit/examples/codecs.rs` | What the engine's FFmpeg can decode, in Rust |
| `electron-hello/` | Electron's hello world, which `scripts/electron-baseline.sh` measures zebkit against |

For the C API, the programs in `tests/engine/` are examples too: `api.c`
starts the engine, loads a page, runs script in it and serves CDP;
`bridge.c` gives a page `zebkit.*`; `scheme.c` answers a URL scheme of the
program's own; `pdf.c` opens, renders, searches and edits a PDF.

## Running them

```sh
# window.c
cc -std=c11 -I$PKG/include examples/window.c -L$PKG -l:libzebkit.so.1 -Wl,-rpath,$PKG -o window
./window https://example.com/

# ui/todo.mjs, with zebkit-js built against the package
cc -std=c11 -I$PKG/include tools/zebkit-js/zebkit-js.c -L$PKG -l:libzebkit.so.1 -Wl,-rpath,$PKG -o zebkit-js
./zebkit-js examples/ui/todo.mjs
./zebkit-js --headless examples/ui/todo.mjs --capture todo.png    # no display: a picture of it

# offscreen-host: the renderer against the package, the host with nothing of it
cc -std=c11 -I$PKG/include examples/offscreen-host/render.c -L$PKG -l:libzebkit.so.1 -Wl,-rpath,$PKG -pthread -o render
cc -std=c11 examples/offscreen-host/show.c -o show
./show --renderer=./render --url=https://example.com/ --size=800x600@1 --shots=shots -- --no-sandbox

# ffmpeg.cpp
c++ -std=c++17 examples/ffmpeg.cpp -I$PKG/include -L$PKG -l:libzebkit.so.1 -Wl,-rpath,$PKG -o ffmpeg-example
./ffmpeg-example examples/sample.mp3 out.wav

# the Rust examples
cd bindings/rust && ZEBKIT_DIR=$PKG cargo run --example window -- https://example.com/
```

Where the machine cannot give Chromium's sandbox (containers, many build
machines), add `--no-sandbox` to the engine's arguments. On macOS the
package holds `Zebkit.framework`: link with
`-F$PKG -framework Zebkit -Wl,-rpath,$PKG` instead.

`mise example:linux:basic` builds and runs `window.c` (`scripts/example.sh`),
`mise example:ffmpeg` the FFmpeg example, `mise example:rust:window` and
`example:rust:codecs` the Rust ones.

On macOS, `scripts/example.sh macos-basic` builds the C embedding example
with the slim framework. To select a prebuilt engine package:

```sh
ZEBKIT_DIR=/path/to/macos/package scripts/example.sh macos-basic https://example.com/
```

This compiles `window.c` and creates `dist/example/Zebkit Embedded Browser.app`, copies the
framework inside it, and signs the helper, framework and application locally.
The Chromium sandbox stays enabled. `EXAMPLE_BUILD_ONLY=1` builds without
launching; the app can then be opened from Finder. Without `ZEBKIT_DIR`, the
script prefers `dist/release` (`dist/debug` with `EXAMPLE_DIST=debug`), then
`out/blink/slim-macos-native/package`, then the cross-built
`out/blink/slim-macos/package`.

This builds the application against that package. To rebuild the engine
on this Mac, run `scripts/dist.sh release` first (`docs/MACOS.md`). A
released package may predate fixes in this checkout's `src/engine` and
`patches`.
