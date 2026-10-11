# zebkit

A trimmed WebKit build that ships as a small, self-contained, relocatable
engine with media playback, for Linux first and Windows and macOS later.
The Linux package needs nothing from the system but glibc 2.17 or newer,
and uses Wayland, X11, a GPU and sound when they are there. It is driven
over the Chrome DevTools Protocol. See [docs/PROGRESS.md](docs/PROGRESS.md)
for where it stands.

zebkit is not a fork with its own copy of the WebKit source. It is a pinned
upstream release plus what turns it into zebkit:

| Directory | What it holds |
|---|---|
| `upstream.lock` | The upstream release we build, with its checksum |
| `toolchain.lock` | The cross toolchain and the oldest glibc we target |
| `profiles/` | Build flavours as CMake initial-cache files (`baseline`, `slim`, `slim-dev`) |
| `patches/` | Source changes on top of upstream, applied in `series` order |
| `deps/` | Recipes for third-party libraries built from source for size; `devtools-protocol/`, Chrome's definition of the protocol, copied unchanged, which the CDP endpoint is generated from and tested against |
| `src/` | Our own engine code: `cdp/`, the Chrome DevTools Protocol endpoint |
| `scripts/` | Fetch, build, test and measure; every step is one script |
| `rust/` | zebkit for Rust: `zebkit-rs`, the raw bindings to the engine library, and `zebkit`, the Rust interface built on them |
| `examples/` | Applications that embed the engine: `window.c` opens a window with a web page in it |
| `tests/` | Smoke, media, display and benchmark pages; `cdp/`, the protocol and client tests |
| `tools/` | Small programs the scripts build: `pageprobe` loads a URL headlessly and reports on it, `cdphost` is a minimal embedding application, `crashsite` locates a crash, `syslib-stubs` makes the stand-ins for optional system libraries |
| `assets/` | The top-100 site list used for compatibility testing |
| `docs/` | `PROGRESS.md`: what is done, what is not, the roadmap. `CDP.md`: driving the engine over the Chrome DevTools Protocol. `DEPS.md`: every library in the engine and beside it, with versions. `VENDOR.md`: using those libraries from an application, FFmpeg first. `COMPAT.md`: how site compatibility is tested, and the record. `reports/`: size reports per profile, committed so regressions show in diffs |

`upstream/` (extracted source), `out/` (build trees) and `.cache/` (downloads,
ccache) are generated and not tracked.

## Quick start (Debian/Ubuntu)

With [mise](https://mise.jdx.dev), after `scripts/bootstrap-linux.sh` has
installed the host tools once:

```sh
mise run r                # release build for this machine  -> dist/release
mise run d                # the same with debug information -> dist/debug
mise example:linux:basic  # build and run examples/window.c, a small browser window
mise example:ffmpeg       # build and run examples/ffmpeg.cpp, on the FFmpeg inside the engine
```

`mise d` works as a short form. `mise r` alone does not: it is mise's own
short form of `mise run`, and asks which task to run; `mise r r` is the
release build. The first build takes hours; a run with nothing changed
takes seconds. The tasks call `scripts/dist.sh` and `scripts/example.sh`, which
work without mise too.

Step by step:

```sh
scripts/bootstrap-linux.sh      # host tools and library headers (uses sudo)
scripts/fetch-toolchain.sh      # pinned cross toolchain into out/toolchain/
scripts/fetch-upstream.sh       # download, verify, extract, apply patches
scripts/build-deps.sh           # build the libraries in deps/ into out/deps/prefix
scripts/build.sh slim           # configure + build into out/slim/
scripts/smoke.sh slim           # load a test page headlessly over HTTP and HTTPS
scripts/media.sh slim           # play H.264, VP9 and MP3 samples
scripts/display.sh slim         # Wayland, X11 and offscreen display paths
scripts/cdp.sh slim --clients   # Chrome DevTools Protocol: raw, Puppeteer, Playwright
scripts/measure.sh slim         # assemble out/slim/package/, write docs/reports/slim.md
scripts/compat.sh slim          # load the top 100 sites (see docs/COMPAT.md)
```

`baseline` builds upstream defaults with the host's compiler and libraries;
it is the "before" number for size and the reference for site compatibility.
`slim` is the product: built with the cross toolchain, linking `deps/`.
`out/slim/package/` is the directory to ship. `slim-dev` is `slim` without
link-time optimization, for quick rebuilds while changing the engine. Add a
profile by dropping a new `.cmake` file into `profiles/`.

## Status

See [docs/PROGRESS.md](docs/PROGRESS.md) for what is done, what is not, and
the roadmap.
