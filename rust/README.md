# zebkit for Rust

Three crates, over the engine's C API (`<zebkit/zebkit.h>`,
`<zebkit/services.h>`; `docs/API.md`):

| Crate | What it is |
|---|---|
| `zebkit` | The Rust interface: the engine and its loop, views, the page's calls to the application, script outside any page, the Chrome DevTools Protocol endpoint, the engine's services, and the FFmpeg inside the engine. No `unsafe` is needed to use it. An application depends on this one. |
| `zebkit-build` | For the application's build script: one call puts the engine beside the executable. |
| `zebkit-rs` | The raw bindings: `extern "C"` declarations for the engine (`libzebkit.so.1` on Linux, `Zebkit.framework` on macOS), everything `unsafe`. It also finds the engine. Re-exported as `zebkit::sys` for what `zebkit` does not wrap yet. |

The C API is the same on Linux and on macOS, and so are the crates: a
program written for one builds and runs on the other.

## Using it

```toml
[dependencies]
zebkit = "0.0.1-alpha.6"

[build-dependencies]
zebkit-build = "0.0.1-alpha.6"
```

```rust
// build.rs
fn main() {
    zebkit_build::bundle();
}
```

```rust
// src/main.rs
use zebkit::{Allow, View};

fn main() {
    std::process::exit(zebkit::run(|| {
        let view = View::new(Some("https://app.example/"), 1024, 768);
        // The page may call zebkit.app.greet(name); nothing else of zebkit.*.
        view.grant("https://app.example", Allow::APP, None);
        view.bind("greet", |call| {
            let answer = format!("\"hello, {}\"", call.args_json());
            call.resolve(&answer);
        });
        view.on_close(zebkit::quit);
    }));
}
```

```sh
ZEBKIT_DIR=/path/to/package cargo run
```

- **The engine is a package directory**: `scripts/package.sh` in the
  repository makes it (`out/blink/slim/package`, `out/blink/slim-macos/package`).
  Inside the repository the crates find it there; elsewhere `ZEBKIT_DIR`
  names it. Released engines are downloaded once, checked against the
  SHA-256 in `zebkit-rs/engines.txt`; the latest engine is
  0.0.1-alpha.6, for Linux and macOS.
- **`zebkit_build::bundle()` puts the engine beside the executable**, in
  `target/<profile>/zebkit/`, and makes the executable look for it there.
- **To ship the application**, copy the executable and that `zebkit`
  directory together.
- **Chromium's switches** in the program's arguments work as for Chrome:
  `--remote-debugging-port=9222`, `--headless`, `--user-data-dir`,
  `--no-sandbox`.

| Environment variable | Effect |
|---|---|
| `ZEBKIT_DIR` | Use the engine in this directory and download nothing |
| `ZEBKIT_CACHE_DIR` | Where downloads are kept, instead of `~/.cache/zebkit` |
| `ZEBKIT_DOWNLOAD_URL` | Where the archives are, for a mirror: the directory that holds `zebkit-<version>-<target>.tar.xz` |

## Working on the crates in this repository

```sh
scripts/build.sh slim && scripts/package.sh slim     # once: the engine
cd rust
cargo test                                           # services, a page and its calls, a script, CDP, on the real engine
cargo run --example window -- https://example.com/
cargo run --example codecs                           # what the engine's FFmpeg has
```

On a Mac the profile is `slim-macos`.

## What is covered

| Area | In `zebkit` |
|---|---|
| The engine | `run`, `run_with_args`, `quit`, `post` and `post_after` (from any thread), `after`, `is_headless`, `version`, `engine_version` |
| Views | `View`: load, reload, back, forward, resize, close, URL and title, callbacks for loads, the title and closing; `evaluate` for the page's script |
| The page and the application | `View::grant` with `Allow`, `View::bind` and `Call` |
| Script outside any page | `Script`: `run_module`, `evaluate`, `on_idle` |
| CDP | `cdp::listen`, `port`, `url`, `stop` |
| Services | `services`: compression (gzip, zlib, raw deflate, Brotli, Zstandard), digests, HMAC, random bytes, AES-GCM, URL parts, registrable domains, image decoding and encoding, `fetch` through the engine's network stack |
| PDF | `pdf::Document`: open, render (whole or tiles), text and its layout, search, outline, links, annotations, attachments, metadata, save |
| URL schemes | `scheme::register`, `scheme::handle`, `Request` with `respond` and `respond_file` (ranges) |
| FFmpeg | `vendor::ffmpeg`: version and decoders |

The other libraries inside the engine (`<zebkit/vendor/*.h>`) are not
declared here yet.

## Threads

The engine has one thread, the one in `zebkit::run`, and calls every
closure on it. Views and scripts are neither `Send` nor `Sync`, so a
program that tries otherwise does not compile; `zebkit::post` runs a
closure there from any thread. What only the engine's thread may do
(`View::new`, `Script::new`, `quit`, `after`, `cdp::listen`, `fetch`)
panics elsewhere.

## Releasing

The crates and the engine have one version, in `VERSION` and
`rust/Cargo.toml`. A release is an archive of the engine's package for each
system, whose SHA-256 goes into `zebkit-rs/engines.txt`, published on
GitHub before the crates (`mise run r`, then `mise run r:github`;
`docs/BUILD.md`, "Releases"). While the repository is private, point
`ZEBKIT_DIR` at a package you built.
