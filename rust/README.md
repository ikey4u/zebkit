# zebkit for Rust

Two crates:

| Crate | What it is |
|---|---|
| `zebkit-rs` | The raw bindings: `extern "C"` declarations for the engine library (`libzebkit.so.1`), nothing else. Everything in it is `unsafe`. |
| `zebkit` | The Rust interface, built on `zebkit-rs`: web views, the main loop, the Chrome DevTools Protocol endpoint, and the FFmpeg inside the engine. No `unsafe` is needed to use it. |

An application depends on `zebkit`. `zebkit-rs` is there for what `zebkit`
does not wrap yet, re-exported as `zebkit::sys`.

```rust
use zebkit::{MainLoop, WebView};

fn main() {
    let main_loop = MainLoop::new();
    let view = WebView::new();
    if let Some(window) = view.window() {
        window.resize(1024, 768);
        let quit = main_loop.clone();
        view.on_close(move || quit.quit());
    }
    view.load_uri("https://example.com/");
    main_loop.run();
}
```

## Building

The crates link against a built zebkit: a directory with `libzebkit.so.1`
and the libraries shipped beside it. They look in `ZEBKIT_DIR`, and
otherwise in `dist/release` of this repository, which `mise run r` makes.

```sh
mise run r                                   # once: the engine, into dist/release
cd rust
cargo test                                   # a page, the CDP endpoint, FFmpeg, on the real engine
cargo run --example window -- https://example.com/
cargo run --example codecs                   # what the engine's FFmpeg decodes
```

The examples and the test find the engine by themselves. An application of
your own has to be told where it is when it runs: ship the package
directory beside the executable and link with a run path, in `build.rs`

```rust
println!("cargo:rustc-link-arg=-Wl,-rpath,$ORIGIN");
```

or run it with `LD_LIBRARY_PATH` set to the package directory.

## What is covered

| Area | In `zebkit` |
|---|---|
| Pages | `WebView`: load an address or HTML, address and title, reload, stop, back and forward, with callbacks for title, address, load progress and the window being closed; `WebView::headless` for a page that is never shown |
| Windows | `Window`: size and title |
| Main loop | `MainLoop`, `timeout` |
| CDP | `cdp::listen`, `port`, `url`, `set_discovery_enabled`, `stop` |
| FFmpeg | `vendor::ffmpeg`: version, and the decoders and encoders that are built in |

Not covered yet: running script in a page and getting its result, settings,
cookies, user scripts and message handlers, downloads, and decoding with
FFmpeg from safe Rust (the C API is reachable through `zebkit::sys::ffmpeg`,
where declarations are added as needed). The engine's C API is WPE
WebKit's and is far larger than what is declared in `zebkit-rs`; the
declarations are written by hand, not generated.

## Threads

The engine belongs to one thread, the first to create a `WebView` or a
`MainLoop`, and calls every closure on it. The types are neither `Send` nor
`Sync`, so a program that tries otherwise does not compile.
