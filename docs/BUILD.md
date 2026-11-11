# Building zebkit

The engine is Chromium's tree at the release `upstream.lock` pins, zebkit's
patches on it, zebkit's own code linked into it, and GN arguments from a
profile. Each step is one script; each does nothing when it is up to date.

## What it takes

- Linux x86-64 (Debian or Ubuntu), about 120 GB of disk for the tree and a
  build, and as much memory as there is: a link with ThinLTO wants tens of
  gigabytes. The reference machine has 24 cores and 94 GB; a full build
  takes a few hours there, a change in zebkit's own code minutes.
- Chromium's own tools do the rest: depot_tools (pinned in
  `upstream.lock`, never updated by itself), and the compiler, sysroot and
  Rust toolchain Chromium's `DEPS` pins. Nothing else is installed on the
  machine; caches go under `.cache/`.

## Linux

```sh
scripts/fetch-upstream.sh       # Chromium into upstream/chromium/src, patches applied
scripts/build.sh slim           # libzebkit.so.1 and zebkit-helper
scripts/package.sh slim         # out/blink/slim/package: what an application ships
scripts/engine-test.sh slim     # every layer of the API, from programs built against the package
```

- **`fetch-upstream.sh`** checks out the release without history (about
  30 GB), runs `gclient sync` when the pinned version changes, applies
  `patches/series` with `patch -p1`, and links `src/engine` into the tree as
  `//zebkit/engine`. A change of patches is applied file by file, without
  syncing again.
- **`build.sh <profile> [--configure-only] [targets...]`** runs `gn gen`
  with `profiles/_common.gn`, the profile it is based on (a line
  `# zebkit-base: <profile>`) and the profile, into
  `upstream/chromium/src/out/<profile>`, and builds with autoninja. The
  default targets are the engine and its helper.
- **`package.sh <profile>`** assembles `out/blink/<profile>/package`: the
  library, the helper, ICU's data, V8's snapshots, the resource pack
  trimmed to what the engine uses (`scripts/trim-pak.py`), SwiftShader
  with its Vulkan loader, and `include/`, every binary stripped
  (`PACKAGE_STRIP=0` keeps the symbols).

A package needs nothing on the target machine but what
`docs/reports/slim.md` lists: glibc 2.17 or newer and the libraries every
desktop has (X11, GLib, NSS, ALSA, D-Bus). `scripts/package.sh` refuses a
package that needs a newer glibc, and `scripts/glibc-test.sh` runs it on
CentOS 7's.

## macOS

On an Apple silicon Mac with full Xcode installed:

```sh
scripts/dist.sh release          # native engine and package into dist/release
scripts/example.sh macos-basic    # C embedding demo with Zebkit.framework
```

`slim-macos-native` uses the Mac's Xcode and disables the Linux-only ML
inliner and ThinLTO. `scripts/dist.sh debug` uses `slim-macos-native-dev`.

Release archives can still be cross-built on Linux; only Apple's SDK has
to come from a Mac. `docs/MACOS.md` has the steps:

```sh
scripts/macos-toolchain.sh MacOSX.sdk.tar.xz version.plist    # once
scripts/build.sh slim-macos
scripts/package.sh slim-macos                                 # out/blink/slim-macos/package/Zebkit.framework
```

`scripts/engine-test.sh slim-macos` runs on the Mac, against the package
copied there.

## Windows

Built on Linux, the same API: [docs/WINDOWS.md](WINDOWS.md).

## Profiles

| Profile | What it is |
|---|---|
| `slim` | The product: the engine as a library, without what an embedder does not need, optimized for speed where speed is spent (`docs/SIZE_AND_SPEED.md`) |
| `slim-macos` | `slim` for macOS on arm64; without PGO until Chromium's macOS profile is fetched |
| `slim-macos-native` | macOS on arm64 with installed Xcode, without ML inlining or ThinLTO |
| `slim-macos-native-dev` | The native macOS profile with debug information and DCHECKs |
| `slim-windows` | `slim` for Windows x64, built on Linux (`docs/WINDOWS.md`) |
| `slim-windows-sandbox` | `zebkit_sandbox.lib` for the application's executable on Windows, without link-time optimization |
| `slim-speed` | `slim` compiled for speed: `-O2` throughout, no size-trained inliner; 7 to 11% less CPU at layout and on animated pages, 30 MiB larger, 20 to 50 MB more memory a page (`docs/SIZE_AND_SPEED.md`) |
| `slim-dev` | `slim` with debug information and without the link-time optimization pass, for debuggers and profilers (`mise run d`) |
| `slim-chrome` | `slim` compiled as Google compiles Chrome, as far as a library allows; for measuring how much of the gap to Chrome's own build is how `slim` is compiled |

`profiles/_common.gn` supplies the defaults: an official release build,
not a component build, without symbols, with linker maps (for
`scripts/mapsize.py`), H.264 and AAC (`proprietary_codecs`), PDFium without
XFA forms or its own V8, and zebkit's configuration of FFmpeg. A new
profile is a new `.gn` file.

## Patches

`patches/series` is the list, in order. Each patch begins with what it does
and why. Numbering:

| Range | For |
|---|---|
| `1xxx` | Chromium for zebkit: size and speed (relocations, optimization levels by directory, V8), the engine in the build, PDFium additions, content's and Blink's hooks for an embedder, FFmpeg's configuration |
| `0013` | macOS: what upstream does not build without Dawn |

A patch is named `NNNN-short-description.patch`
(`1085-serial-ports-without-a-platform-bluetooth.patch`), begins with what it
does and why, and what it saves if it is for size, and is listed in
`patches/series` (`patches/README.md`).

Every patch has to be carried to each Chromium release, so a build switch
in a profile is preferred to a patch wherever one exists.

## Testing

| Script | What it checks |
|---|---|
| `scripts/engine-test.sh <profile>` | What the engine exports; the libraries (`tests/engine/vendor.c`, `vendor.cc`), services, PDF, the C API, a page's `zebkit.*`, off-screen views and their frames in another process (`tests/engine/offscreen.c`, `examples/offscreen-host`; `ENGINE_TEST_SHOTS=<dir>` keeps their pictures), dialogs, printing to PDF and drag and drop from the C API and over CDP (`tests/engine/page.c`, `cdp_page.py`), URL schemes, script outside a page, `zebkit.ui`, Python, and CDP read back over HTTP. Headless and without the sandbox, as build machines need |
| `scripts/glibc-test.sh <profile>` | The C tests (libraries, services, PDF, the C API, `zebkit.*`, dialogs, popups, media capture, URL schemes) on glibc 2.17 and CentOS 7's libraries, every process of the engine included, without a container: CentOS 7 unpacked once into `out/blink/glibc-2.17/` |
| `cd bindings/rust && cargo test` | The Rust crates against the real engine |
| `bindings/golang`: `go run ./internal/check --headless` and the other checks in `internal/` | The Go binding against the real engine (`bindings/golang/README.md`); `engine-test.sh` builds and runs them, on macOS and Windows from the bundle |
| `scripts/check-bindings.py` | That Rust, Python, Go and host JavaScript each reach every function the headers export, and that each package's `LICENSE` is a copy of `LICENSE` |
| `scripts/check-surfaces.py` | That `src/engine/surfaces.json`, the registry of what the engine shows the person or asks of the system, matches the code (`docs/SYSTEM_INTEGRATION.md`) |
| `scripts/check.sh` (`mise run check`) | What a release is published after: the static checks, `engine-test.sh --fetch`, the suite in strict mode, `glibc-test.sh`, `web-bench.sh --check`; a pass is recorded for these sources in `out/blink/check/` (`--profile`, default `slim`) |
| `scripts/check.sh --static` (`mise run check:static`) | What needs no build: `bash -n` of the scripts, the Python files' syntax, `check-bindings.py` |

`engine-test.sh` takes:

- `--only REGEX`: the tests whose names match, and only the programs they
  run built; one that matches no test fails at once.
- `--fetch`: what some tests take from the network, into `.cache`: the long
  real PDF, and the npm packages of the CDP clients (installed with
  `--ignore-scripts`: no browser), of the MCP client and Playwright's, at
  `upstream.lock`'s versions. Then it exits; the run itself uses no network.
- `--strict`, or `ENGINE_TEST_STRICT=1`: a skip is a failure, of a whole
  test (no tool, no download) or of a part of one (a `skip <part>: <why>` or
  `narrowed: ...` line in its log, which the suite collects), and so is a
  test that exits 0 after printing `FAIL` lines. `ENGINE_TEST_EXPECTED_SKIPS`
  names skips it accepts: extended regular expressions, one a line, on the
  entries of the list the suite prints at the end, `<test>: <why>` or
  `<test>: <part>: <why>`.
- `--build-only DIR` and `--prebuilt DIR/programs`: the bundle for a machine
  with no compiler, and its run there (`docs/MACOS.md`, `docs/WINDOWS.md`).

`scripts/check.sh` runs the suite strict with its own list of expected
skips, what a Linux build machine (this one, the container below) cannot
avoid, written out in the script: the device choosers with no USB, HID or
serial device; `cdp-clients` and `network-stages`' comparison with no
Chrome; the native hosts' PDF tests while the hosts are not built
(`examples/native-host/README.md`); and parts only a Mac or Windows run has
with a person's screen (their clipboard, the Local Network grant, a
window's focus). Any other skip fails it. Its record,
`out/blink/check/passed-<key>`, holds the sources' key (`lib.sh`'s
`source_tree_key`), the profile, whether the run was strict and the tested
package's stamp (`out/blink/<profile>/package.stamp`); `release.sh
--publish` publishes only when all of them match: a strict pass, of `slim`,
of the package as it is now. `ZEBKIT_RELEASE_UNCHECKED=1` publishes without
and says so.

Heavy steps (builds, syncs, the release's cargo builds) run one at a time at
the lowest priority, under the lock `ZEBKIT_HEAVY_LOCK` (default
`.cache/heavy.lock`; point checkouts that share a machine at one file), and
on Linux wait while the load is above `ZEBKIT_HEAVY_MAX_LOAD` (30);
`ZEBKIT_JOBS` (12) is how many jobs they run.

## Measuring

| Script | What it writes |
|---|---|
| `scripts/measure.sh <profile>` | `docs/reports/<profile>.md`: the package's size, uncompressed and compressed, against Electron; the oldest glibc it runs on; what it expects from the system |
| `scripts/mapsize.py <profile>` | `docs/reports/<profile>-map.md`: the engine's bytes by the source directory they came from, from the linker map |
| `scripts/electron-baseline.sh` | `docs/reports/electron.md`: Electron's hello world, packaged and counted the same way |
| `scripts/bench.sh <profile> [before=<dir>]` | JavaScript and DOM workloads, best of three, against a saved package |
| `scripts/layout-bench.sh` | Text layout in zebkit and the pinned Electron: a chapter of a book in columns, laid out again as its width changes (`tests/bench/web/local/layout.html`) |
| `scripts/ui-bench.sh <profile> [rows]` | The same interface as `zebkit.ui` widgets and as a page: layout time, update time, memory |
| `scripts/web-bench.sh [--sites] [--check]` | What pages cost in zebkit and in the pinned Electron: CPU loading and at rest, memory, processes; `docs/reports/web-bench.md`. `--check` fails when zebkit goes over `tests/bench/web/limits.json` (`mise run bench:web`) |

Size counts uncompressed and as a download (`.tar.xz` at `xz -6`, `.zip` at
deflate level 9), both computed by `compressed_size` in `scripts/lib.sh`.

## With mise

| Task | What it does |
|---|---|
| `mise run r` | `scripts/release.sh`: the release packages with the tools, Linux in `dist/release`, macOS in `dist/release-macos` and Windows in `dist/release-windows` (cross-built, `docs/MACOS.md`, `docs/WINDOWS.md`), their archives `dist/zebkit-<version>-<target>.tar.xz`, and the archives' SHA-256 in `bindings/rust/zebkit-rs/engines.txt` |
| `mise run r:github` | The same, then the GitHub release `v<version>` with the archives (`gh`, logged in; the tree committed and pushed; `mise run check` passed, strict, on the same sources and package) |
| `scripts/release.sh --publish-only` | Publishes what an earlier run built into `dist/`, building nothing: what the release lacks is uploaded, what it has must be the same bytes |
| `mise run check` | `scripts/check.sh`: the checks a release needs (above); records the pass `r:github` looks for |
| `mise run check:static` | `scripts/check.sh --static`: the checks that need no build |
| `mise run r:crate` | `scripts/publish-crates.sh`: the Rust crates at `VERSION` to crates.io (`CARGO_REGISTRY_TOKEN`; `--dry-run` to check only) |
| `mise run r:python` | `scripts/publish-python.sh`: the Python binding at `VERSION` to PyPI (`UV_PUBLISH_TOKEN` or `PYPI_TOKEN`; `--dry-run` to check only) |
| `mise run bench:web` | `scripts/web-bench.sh`: what web pages cost in zebkit against Electron; `--check` fails on a regression past `tests/bench/web/limits.json` |
| `mise run d` | `scripts/dist.sh debug`: the platform's development profile into `dist/debug`, with debug information |
| `mise example:linux:basic` | Builds and runs `examples/window.c` against `dist/release` (a URL after `--`) |
| `mise example:macos:basic` | Builds and runs the C embedding demo against the macOS framework |
| `mise example:ffmpeg` | Builds and runs `examples/ffmpeg.cpp` |
| `mise example:rust:window`, `example:rust:codecs` | The Rust crate's examples |

The examples add `--no-sandbox` where the system has no unprivileged
user namespaces for Chromium's sandbox. `mise d` works as a short form of
`mise run d`; `mise r` alone is mise's own short form of `mise run`, so the
release is `mise r r` or `mise run r`.

## Releases

A release is one archive per system, `x86_64-unknown-linux-gnu`,
`aarch64-apple-darwin` and `x86_64-pc-windows-msvc`, all built on Linux, whose SHA-256 goes
into `bindings/rust/zebkit-rs/engines.txt`, where the Rust crate checks what it
downloads; the crates and the engine share the version in `VERSION`. Build
(`mise run r`), check (`mise run check`), commit and push `engines.txt`, then
publish (`mise run r:github`, or `scripts/release.sh --publish-only` to
upload what was built without building again). A version whose tag
`v<version>` exists, here or on origin, and whose release has archives is
published: `release.sh` stops before building and asks for a new `VERSION`,
and never rewrites that version's lines of `engines.txt`; only
`--publish-only` goes on, to upload archives the release lacks with the bytes
it names. An interrupted upload can leave a draft release without a tag: the
next run says so, and the draft is deleted (`gh release delete v<version>`)
before running again.
Crates download only from a release anyone can read, so the repository
has to be public for that.

An archive, `zebkit-<version>-<target>.tar.xz`, unpacks to one directory
`zebkit-<version>-<target>/`: the package (the engine, its helper and data,
`include/`), and beside the engine the tools, built from that package by
`release.sh`:

| | Linux | macOS | Windows |
|---|---|---|---|
| the engine | `libzebkit.so.1`, `zebkit-helper`, data | `Zebkit.framework` (helper and data inside) | `zebkit.dll`, `zebkit.dll.lib`, `zebkit-helper.exe`, `zebkit_sandbox.lib`, data |
| the tools | `zebkit-run`, `zebkit-js`, `zebkit-mcp` | the same | the same, `.exe` |
| how a tool finds the engine | run path `$ORIGIN` | run path `@loader_path` (`@executable_path`) to the framework | beside it, as Windows finds DLLs |

The tools run from the unpacked directory as they are, and can be moved
only together with the engine. They are optimized and stripped;
`zebkit-mcp` is built `--release --locked`. On Linux they are linked against CentOS 7's glibc
2.17 (the root `scripts/glibc-test.sh --root` lays out), as Rust's std built
against a newer glibc asks for versions (`getrandom`, `GLIBC_2.25`) that
glibc 2.17's dynamic linker refuses even when weak; `release.sh` refuses a
tool that needs a newer glibc or names a path of the build machine. On
Windows they take part in Chromium's sandbox (`docs/WINDOWS.md`).
`release.sh` needs cargo, with Rust's `aarch64-apple-darwin` and
`x86_64-pc-windows-msvc` targets (`rustup target add`). The Rust crate's
`zebkit-build` leaves the tools out of what it copies beside an
application.

The tools add to an archive, unpacked / as `.tar.xz`, as last measured with
0.2.0's engine on 2026-11-07: Linux 1.1 / 0.4 MiB, macOS 0.9 / 0.35 MiB,
Windows 4.8 / 1.0 MiB (the C tools carry the sandbox's 1.2 MiB each there).

### In a container

`docker/Dockerfile` is a machine for the whole release: Debian 13 with
what Chromium's tools call from the system, what the tests drive the engine
with (Xvfb, labwc, a session bus, xdotool, `x11-utils`), Rust with the
macOS and Windows targets, and mise. Its entry, `docker/publish.sh`:

1. lays out both SDKs once, when `.cache/sdk` (or `ZEBKIT_SDK_DIR`) has
   neither: `scripts/fetch-upstream.sh`, Apple's SDK copied from the `/macos`
   mount, then `scripts/zsdk-create.sh all`, which also downloads MSVC and the
   Windows SDK from Microsoft (that accepts Microsoft's licence terms;
   `docs/WINDOWS.md`);
2. runs `mise run r` (all three systems built and packed, their checksums
   written into `engines.txt`), then `mise run check` (`scripts/check.sh`,
   strict);
3. with `--no-publish`, stops there. Otherwise it stops too while the tree has
   changes (`engines.txt` with the new checksums): commit and push them and
   run it again;
4. a run on a clean tree whose `dist/` archives are those `engines.txt` names
   and whose sources have a strict pass of the checks recorded builds
   nothing: it runs `scripts/release.sh --publish-only` (`GH_TOKEN` needed).
   When the sources changed after the build, it builds and checks again
   instead.

```sh
docker build -t zebkit-release --build-arg UID="$(id -u)" docker
run=(docker run --rm -it -v "$PWD:$PWD" -w "$PWD" -v zebkit-mise:/home/builder/.local/share/mise
     -v ~/MacOSX:/macos:ro)
"${run[@]}" -e GH_TOKEN zebkit-release   # build, check; stops for the commit
git commit -am "<version>: ..." && git push   # engines.txt has the new checksums
"${run[@]}" -e GH_TOKEN zebkit-release   # publishes what was built and checked
```

The repository is mounted at the same path as outside, so the Chromium
tree, the build and the caches stay in it between runs and a second build
compiles only what changed; the `zebkit-mise` volume keeps mise's tools. `~/MacOSX` holds `MacOSX.sdk.tar.xz` and
`version.plist` from a Mac (`scripts/macos-toolchain.sh`), needed on the
first run only. The image is not a glibc 2.17 system and need not be:
the engine is compiled against Chromium's own sysroot, `package.sh`
refuses one that needs a newer glibc than 2.17, and `glibc-test.sh` runs
it on CentOS 7's before anything is published.
