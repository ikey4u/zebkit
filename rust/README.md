# zebkit for Rust

Three crates:

| Crate | What it is |
|---|---|
| `zebkit` | The Rust interface: web views, the main loop, the Chrome DevTools Protocol endpoint, and the FFmpeg inside the engine. No `unsafe` is needed to use it. An application depends on this one. |
| `zebkit-build` | For the application's build script: one call puts the engine beside the executable. |
| `zebkit-rs` | The raw bindings: `extern "C"` declarations for the engine (`libzebkit.so.1` on Linux, `WebKit.framework` on macOS), everything `unsafe`. It also finds the engine, downloading it if need be. Re-exported as `zebkit::sys` for what `zebkit` does not wrap yet. |

The interface of `zebkit` is the same on Linux and on macOS: a program
written for one builds and runs on the other.

## Using it

Nobody has to build the engine. The first build downloads it.

```toml
[dependencies]
zebkit = "0.0.1-alpha.2"

[build-dependencies]
zebkit-build = "0.0.1-alpha.2"
```

```rust
// build.rs
fn main() {
    zebkit_build::bundle();
}
```

```rust
// src/main.rs
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

```sh
cargo run
```

What happens:

- **The engine is downloaded once**, about 25 MiB, from the GitHub release
  of the same version as the crate, into `~/.cache/zebkit`. Its SHA-256 is
  fixed in the crate (`zebkit-rs/engines.txt`); a file that does not match
  is refused. The download uses the system's `curl`, `sha256sum` (`shasum`
  on macOS), `tar` and `xz`, so the crates have no dependencies of their
  own.
- **`zebkit_build::bundle()` puts the engine beside the executable**, in
  `target/<profile>/zebkit/`, and makes the executable look for it there.
  That is why `cargo run` works with no environment set up.
- **To ship the application**, copy the executable and that `zebkit`
  directory together. Nothing else is needed on the user's machine but
  glibc 2.17 or newer; on macOS, macOS 27 on Apple Silicon.

There are two archives: one covers every Linux distribution on x86_64,
the other macOS 27 on Apple Silicon. For other platforms there is none;
the build says so and how to go on.

| Environment variable | Effect |
|---|---|
| `ZEBKIT_DIR` | Use the engine in this directory and download nothing: for an engine built by hand, and for building without network access |
| `ZEBKIT_CACHE_DIR` | Where downloads are kept, instead of `~/.cache/zebkit` |
| `ZEBKIT_DOWNLOAD_URL` | Where the archives are, for a mirror: the directory that holds `zebkit-<version>-<target>.tar.xz` |

## Working on the crates in this repository

Inside the repository the crates use `dist/release`, which `mise run r`
makes, and download nothing.

```sh
mise run r                                   # once: the engine, into dist/release
cd rust
cargo test                                   # a page, the CDP endpoint, FFmpeg, on the real engine
cargo run --example window -- https://example.com/
cargo run --example codecs                   # what the engine's FFmpeg has
```

The same on a Mac. `mise example:rust:window` and
`mise example:rust:codecs` are the last two.

## On macOS

The engine's API there is Objective-C (`WKWebView` in an `NSWindow`, in
an application whose loop AppKit runs), which Rust cannot declare.
`zebkit-rs/src/cocoa/shim.m` puts the part of it that `zebkit` uses behind
C functions; the build script compiles it with the system's compiler, and
`zebkit::sys::cocoa` declares them. What differs for a program:

- The window of a view is the engine's on both: shown when the machine
  has a display, and not otherwise (on macOS, `zebkit-view.h`).
- `MainLoop` is the application's loop, of which a process has one: every
  `MainLoop` is a handle to the same loop.
- `WebView::as_ptr()` is a `WKWebView`, for code that speaks Objective-C.
- The executable finds the engine through a path the linker writes into
  it (`-dyld_env`), where Linux has a run path. `zebkit_build::bundle()`
  arranges it on both.

## Releasing

The crates and the engine have one version, in `VERSION` and
`rust/Cargo.toml`.

```sh
mise run r                     # on each system: the engine, and dist/zebkit-<version>-<target>.tar.xz; its checksum goes into rust/zebkit-rs/engines.txt
                               # then, on one machine, with every archive in dist/ and every checksum in engines.txt:
git commit -am "Release <version>"
mise run r:github              # the GitHub release v<version>, with the archives
cd rust
cargo publish -p zebkit-rs && cargo publish -p zebkit-build && cargo publish -p zebkit
```

`mise` installs the GitHub CLI the second step uses. It has to be logged
in once, with `mise exec -- gh auth login`, or given a token in `GH_TOKEN`.

The archive has to be on GitHub, and readable by anyone, before the crates
are published: a crate whose archive cannot be downloaded does not build.
The release has to be made from the commit whose `engines.txt` names the
archive's checksum, which the script checks.

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
where declarations are added as needed). The engine's own API (WPE
WebKit's C API on Linux, WebKit's Objective-C API on macOS) is far larger
than what is declared in `zebkit-rs`; the declarations are written by
hand, not generated.

## Threads

The engine belongs to one thread, the first to create a `WebView` or a
`MainLoop`, and calls every closure on it. The types are neither `Send` nor
`Sync`, so a program that tries otherwise does not compile.
