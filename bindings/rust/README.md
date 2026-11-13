# zebkit for Rust

Three crates, over the engine's C API (`<zebkit/zebkit.h>`,
`<zebkit/services.h>`; `docs/API.md`):

| Crate | What it is |
|---|---|
| `zebkit` | The Rust interface: the engine and its loop, views, the page's calls to the application, script outside any page, the Chrome DevTools Protocol endpoint, the engine's services, and the FFmpeg inside the engine. No `unsafe` is needed to use it. An application depends on this one. |
| `zebkit-build` | For the application's build script: one call puts the engine beside the executable. |
| `zebkit-rs` | The raw bindings: `extern "C"` declarations for the engine (`libzebkit.so.1` on Linux, `Zebkit.framework` on macOS, `zebkit.dll` on Windows), everything `unsafe`. It also finds the engine. Re-exported as `zebkit::sys` for what `zebkit` does not wrap yet. |

The C API is the same on Linux, macOS and Windows, and so are the crates:
a program written for one builds and runs on the others.

## Using it

```toml
[dependencies]
zebkit = "0.4.0"

[build-dependencies]
zebkit-build = "0.4.0"
```

```rust
// build.rs
fn main() {
    zebkit_build::bundle();
}
```

```rust
// src/main.rs
use zebkit::json::Value;
use zebkit::{Allow, View};

fn main() {
    std::process::exit(zebkit::run(|| {
        let view = View::new(Some("https://app.example/"), 1024, 768);
        // The page may call zebkit.app.greet(name); nothing else of zebkit.*.
        view.grant("https://app.example", Allow::APP, None);
        view.bind("greet", |call| {
            // The arguments as JSON values (args_json() has them as the
            // page sent them); the answer is a JSON value too.
            let name = call.args().first().cloned().unwrap_or_default();
            call.resolve_value(&Value::object([("hello", name)]));
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
  0.4.0, for Linux, macOS and Windows.
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
| `ZEBKIT_CACHE_DIR` | Where downloads are kept, instead of `~/.cache/zebkit` (`%LOCALAPPDATA%\zebkit` on Windows) |
| `ZEBKIT_DOWNLOAD_URL` | Where the archives are, for a mirror: the directory that holds `zebkit-<version>-<target>.tar.xz` |

## Working on the crates in this repository

```sh
scripts/build.sh slim && scripts/package.sh slim     # once: the engine
cd bindings/rust
cargo test                                           # services, a page and its calls, a script, CDP, on the real engine
cargo run --example window -- https://example.com/
cargo run --example codecs                           # what the engine's FFmpeg has
```

On an Apple silicon Mac, `scripts/dist.sh release` builds the native
`slim-macos-native` profile into `dist/release`. The bindings prefer it,
then `out/blink/slim-macos-native/package`, then the cross-built
`out/blink/slim-macos/package`. `ZEBKIT_DIR` overrides this selection.
When rebuilding a host application that sets `ZEBKIT_DIR` itself, point
it at the new `dist/release` package explicitly so it uses the new engine.

When a process also starts the engine, use ICU-backed URL services from
the ready closure passed to `run` or `run_with_args`. Chromium registers
its schemes during startup. Standalone service programs can use these
helpers immediately without starting the engine.

## What is covered

| Area | In `zebkit` |
|---|---|
| The engine | `run`, `run_with_args`, `init`, `pump`, `shutdown`, `set_wakeup`, `quit`, `post` and `post_after` (from any thread), `after`, `is_headless`, `version`, `engine_version`, engine settings, cookie storage |
| Views | `View`: `new`, `try_new`, `new_with_options` (`<zebkit/window.h>`), `load_url`, `reload`, `go_back`, `go_forward`, `resize`, `close`, URL and title, callbacks for loads, the title and closing, the window's state and actions, attaching to a parent (`attach`, `try_attach`), settings; `evaluate` for the page's script, `capture`, `print_to_pdf` |
| The page and the application | `View::grant` with `Allow`, `View::bind` and `Call` (answered later, from any thread; one dropped unanswered rejects the page's promise) |
| What pages ask | permissions, navigations, certificates, HTTP authentication, new windows and popups, downloads, client certificates, file choosers, camera and microphone, the eye dropper, device choosers, sharing, the network filter, keys, the context menu, fullscreen, console messages, how decided downloads ended (`on_download_result`), the page's process gone (`on_process_gone`) (`View::on_*`, with `clear_*_handler`) |
| Off-screen views | `offscreen`: `View::new_offscreen` (the engine's error when it makes none), frames (`Frame`, `FrameBuffer`), mouse, wheel, keys, input methods, the caret, the cursor, popups, passkeys |
| Dialogs, drags, find | `page`: JavaScript dialogs, beforeunload, drag and drop, both ways, find in page (`View::find`, `stop_finding`, `on_find`, `find_result`, `FindResult`) |
| Contexts | `Context`: cookies, storage and cache of their own, for its views |
| Automation | `automation`: Playwright's API on a view's page, as Python's `zebkit.automation` has it: `Page` (its events with `on`), `Frame`, `Locator`, `FrameLocator`, `ElementHandle`, `JSHandle`, `Route`, `Request`, `Response`, `Dialog`, `FileChooser`, `Download`, `Worker`, `ConsoleMessage`, `Keyboard`, `Mouse`, `Touchscreen`, `Clock`, `Tracing`, `APIRequestContext`, `BrowserContext` (`new_context`), `expect`; any command with `call`; `add_init_script`'s `Disposable`; its endpoint (`listen`) |
| JSON | `json::Value`: what the engine gives and takes as JSON, read and written without a dependency |
| Script outside any page | `Script`: `run_module`, `evaluate`, `on_idle` |
| CDP | `cdp::listen`, `listen_async`, `port`, `url`, `stop` |
| Media | `media`: what a page plays, listed, captured to files (`MediaCapture`) and tapped (`MediaTap`); `media_tools`: `probe`, `remux`, `extract_audio`, `extract_video` on the engine's FFmpeg |
| Bytes | What the engine makes (pixels, files, compressed or decrypted data) comes as `Bytes`, its own buffer without a copy, freed when dropped; what a callback is given (a capture, a PDF, a drag's or cursor's picture, a fetched body) is borrowed for the call |
| Services | `services`: compression (gzip, zlib, raw deflate, Brotli, Zstandard), digests, HMAC, random bytes, AES-GCM, URL parts, registrable domains, image decoding and encoding, `fetch` through the engine's network stack |
| Canvas and text | `canvas`: drawing on the engine's Skia, text shaped on its HarfBuzz |
| PDF | `pdf::Document`: open, render (whole or tiles), text and its layout, search, outline, links, annotations, forms, pages, attachments, signatures, metadata, save; hooks |
| URL schemes | `scheme::register`, `scheme::handle`, `Request` with `respond` and `respond_file` (ranges) |
| The system | `system`: screens, power, sleep, notifications, the system's handlers (`open_external`, `show_in_folder` and their `_async` forms), its appearance; `Tray`; `app`: the menu, the Dock's menu, the badge, progress, attention, files to open, reopen, quit |
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

`View::new` panics, too, when the engine makes no view (its window could
not be made); `View::try_new` returns that as an error. A closure the
engine calls that panics ends the loop, and `run` (or `pump`) resumes the
panic once the engine has returned.

## Releasing

The crates and the engine have one version, in `VERSION` and
`bindings/rust/Cargo.toml`. A release is an archive of the engine's package for each
system, whose SHA-256 goes into `zebkit-rs/engines.txt`, published on
GitHub before the crates (`mise run r`, then `mise run r:github:publish`;
`docs/BUILD.md`, "Releases"). Where the release archives cannot be
reached, point `ZEBKIT_DIR` at a package you built.
