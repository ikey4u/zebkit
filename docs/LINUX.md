# Linux

zebkit on Linux: what it is, how large and how fast, what was checked and
what was not. `docs/PROGRESS.md` has the project as a whole; this is the
part that is Linux's alone.

The engine is WPE WebKit 2.54.1, built for x86_64 with a pinned cross
toolchain, with every library it needs but glibc built from source
(`docs/DEPS.md`) and linked in or shipped beside it.

## State

| Area | Status |
|---|---|
| `slim` build | Builds; smoke test (HTTP, HTTPS, WebGL, JIT) and media test pass |
| Size | **83.1 MiB** in 11 files, plus headers |
| Form | A shared library, `libzebkit.so.1`, with two helper programs it starts itself (`zebkit-web`, `zebkit-network`). An application links against it and creates views with WPE WebKit's C API; `examples/window.c` is a complete one |
| Chrome DevTools Protocol | Built in; Puppeteer and Playwright connect and drive pages. Checked against Chrome's protocol definition and against Chrome itself. A subset: see `docs/PROGRESS.md` |
| glibc floor | **2.17**: no shipped binary imports a newer symbol |
| Media | H.264, VP8, VP9, AAC, MP3, Opus, Vorbis, FLAC; MP4 and WebM; Media Source; Web Audio |
| Top-100 sites | **84 pass, 0 fail, 16 blocked** (round 4) |
| Relocatable | Yes: copied elsewhere, it loads an HTTPS page with nothing from the build tree |
| System requirements | **glibc only.** Wayland, X11, GPU buffer and sound libraries are used when present |
| Display | Chosen at run time: Wayland, then X11, then offscreen; all three tested |

## Size

The measured thing is the directory `scripts/package.sh`
assembles: the stripped engine and every library a base system does not
provide.

| Profile | Engine | Bundled libraries | Total |
|---|---:|---:|---:|
| `baseline` (upstream defaults, host libraries) | 119.6 | 93.3 | 213.0 |
| `slim` | 76.7 | 6.4 | **83.1** |

Compressed, the 80.3 MiB build before the display work was 23.0 MiB with
xz, 25.1 with zstd and 32.0 with gzip.

### How it got from 61.0 to 83.1

| Change | MiB | Measured or estimated |
|---|---:|---|
| Media: WebKit's media code, GStreamer (core and 21 plugins), FFmpeg decoders | about +9 | Estimated from engine sizes before and after |
| glibc 2.17: no packed relocations (they need glibc 2.36) | +7.4 | Size of the relocation section they would remove |
| Different toolchain: no LLVM function merging, clang 21 and libc++ instead of clang 19 and libstdc++ | about -1 net | Estimated; the pieces were not separated |
| WebGL (ANGLE) | +3.7 | Measured: 76.6 to 80.3 |
| X11 backend, static libxkbcommon, on-demand loading of system libraries | +0.3 | Measured: 80.3 to 80.6 |
| CDP endpoint (`src/cdp`), and the inspector agents it drives, which a size patch had removed | +0.7 | Measured: 80.6 to 81.3 |
| All of FFmpeg's API kept and exported for applications (`docs/VENDOR.md`) | +0.3 | Measured: 81.3 to 81.6 |
| FFmpeg parts for applications only: file reading and writing, scaling, its own encoders | +1.5 | Measured: 81.6 to 83.1 |

### Where the engine's size is (at 73.9 MiB, before the display work)

| Section | MiB |
|---|---:|
| Code | 49.5 |
| Read-only data (ICU data is 7.6 of it) | 12.4 |
| Dynamic relocations | 7.4 |
| Vtables and other relocated data | 3.4 |

A per-component breakdown needs a linker map, which the cross toolchain's
linker driver does not produce. `docs/reports/slim-map.md` is from the last build
before the toolchain change and no longer current.

### What could come back

- **Packed relocations, 7.4 MiB.** Only if the glibc floor moves to 2.36.
- **Linker map and function merging, about 1.2 MiB.** Needs lld driven
  directly instead of through the toolchain's driver.
- **ICU extras, about 1.2 MiB.** Khmer, Burmese and Lao dictionaries and the
  less common Chinese collation orders.

## Correctness

`docs/COMPAT.md` has the method and the record. Each of the 100 sites is
loaded in `slim` and in stock WebKit; `slim` passes when it loads, paints,
builds a comparable DOM and throws no script error stock WebKit does not.

| Round | Pass | Fail | Blocked | What it found |
|---|---:|---:|---:|---|
| 1 | 35 | 62 | 3 | A linker option merged JavaScriptCore type descriptors; the JIT built a `Map` for `new Set` |
| 2 | 54 | 34 | 12 | `HTMLVideoElement`, `Audio` and friends did not exist |
| 3 | 86 | 1 | 13 | One site needed Media Source Extensions |
| 4 | 84 | 0 | 16 | A crash in software video upload (ikea.com); WebGL missing (baidu.com). Both fixed. |

- **Blocked (16):** stock WebKit did not get the real site from this machine
  either (Cloudflare challenges, bot walls, one unreachable server, one page
  that never yields). These are untested, not passed.
- **Media:** `scripts/media.sh` plays four generated samples, including one
  with non-square pixels, and checks they reach the end at the right size.
- **Guards in the smoke test:** a JIT-hot type check, a check that no two
  `ClassInfo` objects share an address, and a WebGL draw-and-read-back.

Not covered: interaction (nothing is clicked or logged in), pixel-level
layout equality, and playback on the sites themselves.

`slim` still lacks 27 script-visible interfaces stock WebKit has: capture
and recording (15), WebCodecs (7), Gamepad (4) and XSLT (1).

## Portability

| Question | Answer |
|---|---|
| Which glibc? | 2.17 or newer. Built with a pinned cross toolchain (`toolchain.lock`) that targets glibc 2.17 whatever the build host runs. |
| What else must the system have? | Nothing. The package's only hard dependencies are glibc's own libraries. |
| What does it use if present? | `libwayland-client`, `libX11`, `libgbm`, `libdrm`, `libasound`, and the EGL/GLES driver. Each is loaded on first use (`deps/syslibs`); without it the feature that needs it is unavailable and the engine carries on. |
| C++ runtime, keyboard handling? | libc++ and libxkbcommon are linked in. |
| Does it run from any directory? | Yes; helpers, the injected bundle and the TLS plugin are found next to the engine. |
| Fonts, CA certificates, keyboard layouts? | Read from the usual system locations. |

### Display

The engine picks a display backend when it starts, in this order:

1. **Wayland**, if `libwayland-client` (1.20 or newer) is installed and a
   compositor answers.
2. **X11**, if `libX11` is installed and `DISPLAY` leads to a server. This
   backend is ours (`patches/2004`): one window per toplevel, frames copied
   with `XPutImage`, core-protocol mouse, wheel and keyboard input.
3. **Offscreen** otherwise. The page loads and renders with no window; the
   embedding application can read frames back.

`WPE_PLATFORM=wayland|x11|headless` forces one. `scripts/display.sh` checks
all of this with a private X server and a private headless compositor,
19 checks: Wayland is preferred when both are up; X11 draws the right pixels
and delivers clicks, motion, wheel and typed text; with libwayland-client
"not installed" it falls back to X11; with no optional library at all it
still starts and renders offscreen.

Limits of the X11 backend: 24- or 32-bit TrueColor visuals only, no input
methods, no clipboard, no custom cursors, no HiDPI scaling, and frames are
copied through the X protocol rather than shared, so large windows cost
CPU.

### Not verified

The build machine cannot run containers, so:

- **It has not been started on an actual glibc 2.17 system.** The symbol
  versions say it should load; nothing has run it there.
- **Kernel features.** A glibc 2.17-era kernel (3.10) may lack system calls
  WebKit uses, such as `memfd_create`.
- **Old graphics drivers.** Rendering needs EGL; how old a driver works is
  untested. Without `libgbm` the engine falls back to copying frames.
- **"Library not installed"** was tested by making the loader refuse a
  library (`ZEBKIT_WITHOUT_LIBRARIES`), not on a machine that lacks it.

## Speed

`scripts/bench.sh`, milliseconds, best of three, lower is better.

| Build | loop | objects | strings | json | regex | sort | dom |
|---|---:|---:|---:|---:|---:|---:|---:|
| `baseline` (`-O2`, all JIT tiers) | 38 | 48 | 17 | 220 | 210 | 62 | 415 |
| `slim` (`-Oz`, baseline + DFG JIT) | 179 | 72 | 27 | 406 | 342 | 102 | 767 |

`slim` got slower with the toolchain change: before it, the same benchmark
gave json 287, regex 248, sort 66, dom 670. The cause is not isolated; the
candidates are clang 21's `-Oz`, libc++ with its hardening checks, and the
loss of function merging.

Rejected for speed: interpreter-only JavaScript (4 MiB, 2x to 10x slower)
and the machine outliner (1.8 MiB, about 2x slower JSON and DOM).

## Trade-offs in `slim`

| Choice | Where | Consequence |
|---|---|---|
| No capture, recording, WebRTC, WebCodecs, encrypted media, speech | `profiles/slim.cmake` | No camera/microphone, no DRM video |
| GStreamer without its GL path | `profiles/slim.cmake` | Video frames are uploaded from memory; no zero-copy |
| Audio output through ALSA only | `deps/gstreamer` | Works through PulseAudio/PipeWire's ALSA plugin where present; silent where `libasound` is missing |
| X11 backend is a simple one | `patches/2004` | See its limits under Portability |
| No WebXR, GPU process | `profiles/slim.cmake` | Feature absent |
| No FTL JIT or WebAssembly JIT tiers | `profiles/slim.cmake` | Slower hot loops; wasm runs interpreted |
| `-Oz` instead of `-O2` | `profiles/slim.cmake` | Up to 2x slower on C++-bound work |
| No Web Inspector front end, remote inspector server, WebDriver, JIT disassembler | `profiles/slim.cmake` | Automation and debugging go through CDP (`docs/CDP.md`) only |
| No PDF.js, XSLT, MHTML, gamepad, spellcheck, content extensions, accessibility, AVIF, JPEG XL, colour management | `profiles/slim.cmake` | Feature absent |
| No unwind tables, no assertion strings | `profiles/slim.cmake`, `profiles/include/size-prelude.h` | A crash gives no native stack trace or message; `tools/crashsite` recovers an approximate one |
| simdutf without its AVX-512 path | `profiles/slim.cmake` | AVX2 and SSE paths are used instead |
| ICU locale data for English and CJK only | `deps/icu/filter.json` | `Intl` formats other locales with root-locale rules |
| GnuTLS without p11-kit, IDN, GMP | `deps/gnutls`, `deps/nettle` | No smart-card trust; slower RSA handshakes |
| libsoup without Kerberos or NTLM | `deps/libsoup` | No Negotiate/NTLM HTTP authentication |
| GLib without SELinux, libmount, xattr | `deps/glib` | No mount monitoring through GIO |
| libgcrypt with only the WebCrypto algorithms | `deps/libgcrypt` | A new WebCrypto algorithm needs the list extended |
| FFmpeg with eight decoders | `deps/ffmpeg` | Other codecs (HEVC, AV1, ...) do not play |

## Host notes (this build machine)

- `sudo` here is `fakeroot` and parts of the filesystem are root-owned, so
  `bootstrap-linux.sh` needs
  `APT_OPTS="-o Dir::Cache::archives=$PWD/.cache/apt -o APT::Sandbox::User=$USER -o Dpkg::Options::=--path-exclude=/usr/share/sgml/* -o Dpkg::Options::=--path-exclude=/usr/share/doc/*"`.
- `/var/lib/dpkg/status` was root-owned and blocked `dpkg`; it was replaced
  with an identical user-owned copy. The original is kept at
  `/var/lib/dpkg/status.zebkit-orig`.
- Four packages are unpacked but not configured because their post-install
  scripts need root: `libdbus-1-dev`, `sgml-base`, `libatspi2.0-dev`,
  `libatk-bridge2.0-dev`. `bootstrap-linux.sh` exits non-zero because of
  them even when everything it was asked for is installed.
- Compilers crash at random here, on a different file each time, with both
  the host's clang 19 and the toolchain's clang 21. `build.sh` retries a
  failed pass up to three times.
- The checkout is reachable as `the checkout` and through a symlink. The
  scripts normalize this; a bare `cmake` run from the symlinked path breaks
  the build tree's configuration.
- No containers or user namespaces, so nothing can be run under another
  distribution's userland here.
- Stock WebKit on this host has no H.264 decoder installed, so the
  `baseline` profile fails the H.264 samples in `scripts/media.sh`.
- A full build of one profile takes about 40 minutes.
- `out/slim-nojit` (3.7 GiB) and `out/deps/prefix-host-glibc` are leftovers
  and can be deleted. `out/slim-dev` is the development build and can be
  rebuilt in about 20 minutes.
