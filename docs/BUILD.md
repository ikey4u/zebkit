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
`docs/reports/slim.md` lists: glibc 2.25 or newer and the libraries every
desktop has (X11, GLib, NSS, ALSA, D-Bus).

## macOS

Built on the same Linux machine, as Chromium's own builders compile for
macOS on Linux; only Apple's SDK has to come from a Mac.
`docs/MACOS.md` has the steps:

```sh
scripts/macos-toolchain.sh MacOSX.sdk.tar.xz version.plist    # once
scripts/build.sh slim-macos
scripts/package.sh slim-macos                                 # out/blink/slim-macos/package/Zebkit.framework
```

`scripts/engine-test.sh slim-macos` runs on the Mac, against the package
copied there.

## Windows

Not built yet on Blink. The target is the same profile and the same API
(`docs/ROADMAP.md`).

## Profiles

| Profile | What it is |
|---|---|
| `slim` | The product: the engine as a library, without what an embedder does not need, optimized for speed where speed is spent (`docs/SIZE_AND_SPEED.md`) |
| `slim-macos` | `slim` for macOS on arm64; without PGO until Chromium's macOS profile is fetched |
| `slim-dev` | `slim` with debug information and without the link-time optimization pass, for debuggers and profilers (`mise run d`) |
| `baseline` | Chromium's content shell with its release defaults, the "before" of every measurement |

`profiles/_common.gn` is what every profile has: an official release
build, not a component build, without symbols, with linker maps (for
`scripts/mapsize.py`), H.264 and AAC (`proprietary_codecs`), PDFium without
XFA forms or its own V8, and zebkit's configuration of FFmpeg. A new
profile is a new `.gn` file.

## Patches

`patches/series` is the list, in order. Each patch begins with what it does
and why. Numbering:

| Range | For |
|---|---|
| `1xxx` | Chromium for zebkit: size and speed (relocations, optimization levels by directory, V8), the engine in the build, PDFium additions, the shell's hooks for an embedder, FFmpeg's configuration |
| `0013`, `0014` | macOS: what upstream does not build without Dawn and without test support |

Every patch has to be carried to each Chromium release, so a build switch
in a profile is preferred to a patch wherever one exists.

## Testing

| Script | What it checks |
|---|---|
| `scripts/engine-test.sh <profile>` | What the engine exports; the libraries (`tests/engine/vendor.c`, `vendor.cc`), services, PDF, the C API, a page's `zebkit.*`, URL schemes, script outside a page, `zebkit.ui`, Python, and CDP read back over HTTP. Headless and without the sandbox, as build machines need |
| `cd rust && cargo test` | The Rust crates against the real engine |

## Measuring

| Script | What it writes |
|---|---|
| `scripts/measure.sh <profile>` | `docs/reports/<profile>.md`: the package's size, uncompressed and compressed, against Electron; the oldest glibc it runs on; what it expects from the system |
| `scripts/mapsize.py <profile>` | `docs/reports/<profile>-map.md`: the engine's bytes by the source directory they came from, from the linker map |
| `scripts/electron-baseline.sh` | `docs/reports/electron.md`: Electron's hello world, packaged and counted the same way |
| `scripts/bench.sh <profile> [before=<dir>]` | JavaScript and DOM workloads, best of three, against a saved package |
| `scripts/ui-bench.sh <profile> [rows]` | The same interface as `zebkit.ui` widgets and as a page: layout time, update time, memory |

Size counts uncompressed and as a download (`.tar.xz` at `xz -6`, `.zip` at
deflate level 9), both computed by `compressed_size` in `scripts/lib.sh`.

## With mise

| Task | What it does |
|---|---|
| `mise run r` | `scripts/release.sh`: the release package in `dist/release`, its archive `dist/zebkit-<version>-<target>.tar.xz`, and the archive's SHA-256 in `rust/zebkit-rs/engines.txt` |
| `mise run r:github` | The same, then the GitHub release `v<version>` with the archive (`gh`, logged in; the tree committed) |
| `mise run d` | `scripts/dist.sh debug`: the `slim-dev` profile into `dist/debug`, with debug information |
| `mise example:linux:basic` | Builds and runs `examples/window.c` against `dist/release` (a URL after `--`) |
| `mise example:macos:basic` | The same on a Mac, against the macOS package built on Linux |
| `mise example:ffmpeg` | Builds and runs `examples/ffmpeg.cpp` |
| `mise example:rust:window`, `example:rust:codecs` | The Rust crate's examples |

The examples add `--no-sandbox` where the system has no unprivileged
user namespaces for Chromium's sandbox. `mise d` works as a short form of
`mise run d`; `mise r` alone is mise's own short form of `mise run`, so the
release is `mise r r` or `mise run r`.

## Releases

A release is one archive per system, built on Linux, whose SHA-256 goes
into `rust/zebkit-rs/engines.txt`, where the Rust crate checks what it
downloads; the crates and the engine share the version in `VERSION`. Build
(`mise run r`), commit `engines.txt`, then publish (`mise run r:github`).
Crates download only from a release anyone can read, so the repository
has to be public for that.
