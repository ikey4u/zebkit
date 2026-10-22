# examples

Small programs on zebkit. Each builds against a package directory,
`out/blink/slim/package` after `scripts/package.sh slim` (`docs/BUILD.md`);
below, `$PKG` is that directory.

| Example | What it shows |
|---|---|
| `window.c` | A small browser window: a page with a back button, a forward button and an address box above it, the bar a script in the page that calls the program through `zebkit.app`. One file for Linux and macOS |
| `ui/todo.mjs` | An application in JavaScript with native widgets: `zebkit.ui`, Chromium's views driven by signals, no web page (`docs/API.md`, "Native interface") |
| `ffmpeg.cpp` | A library inside the engine: an MP3 decoded to WAV with the engine's FFmpeg, through `zebkit::vendor::ffmpeg`, with no FFmpeg of the program's own |
| `../rust/zebkit/examples/window.rs` | A window with a web page, in Rust |
| `../rust/zebkit/examples/codecs.rs` | What the engine's FFmpeg can decode, in Rust |
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

# ffmpeg.cpp
c++ -std=c++17 examples/ffmpeg.cpp -I$PKG/include -L$PKG -l:libzebkit.so.1 -Wl,-rpath,$PKG -o ffmpeg-example
./ffmpeg-example tests/media/sample.mp3 out.wav

# the Rust examples
cd rust && ZEBKIT_DIR=$PKG cargo run --example window -- https://example.com/
```

Where the machine cannot give Chromium's sandbox (containers, many build
machines), add `--no-sandbox` to the engine's arguments. On macOS the
package holds `Zebkit.framework`: link with
`-F$PKG -framework Zebkit -Wl,-rpath,$PKG` instead.

`mise example:linux:basic` builds and runs `window.c` (`scripts/example.sh`),
`mise example:ffmpeg` the FFmpeg example, `mise example:rust:window` and
`example:rust:codecs` the Rust ones.
