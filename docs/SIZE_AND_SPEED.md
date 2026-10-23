# Size and speed

What zebkit weighs, where the bytes go, what was taken out and what was
kept, and how the compiler is set between size and speed. Numbers are for
Linux x86-64 and Chromium 155.0.8059.84, from `scripts/measure.sh` (the
package), `scripts/mapsize.py` (the engine's linker map) and
`scripts/bench.sh`, unless said otherwise. Where the usual advice for
slimming Chromium disagrees with what was measured, the measurement wins.

## Now

| | Uncompressed | `.tar.xz` | `.zip` |
|---|---:|---:|---:|
| zebkit `slim`, 2026-10-22 | **157.3 MiB** | **53.9 MiB** | **71.8 MiB** |
| Electron 44.9.0, trimmed (the baseline) | 255.7 MiB | 85.3 MiB | 112.6 MiB |
| zebkit of Electron | 62% | 63% | 64% |

The engine library is 137.6 MiB of it; ICU's data 6.5 MiB, SwiftShader
with its Vulkan loader 3.6 MiB, the headers an application builds against
7.6 MiB, V8's snapshots, the resource pack and ANGLE the rest
(`docs/reports/slim.md`).

There was a limit of 150 MiB (owner, 2026-10-19), under which the engine
was 147.1 MiB with PDFium and FFmpeg's additions. On 2026-10-21 the owner
put speed first ("push the size limit to get the speed"): the size-built
engine laid text out twice as slowly as Chrome's own build. What is checked
now is speed against Chrome's build of the same version, and size against
Electron.

## The baseline: Electron

`scripts/electron-baseline.sh` packages a hello-world application
(`examples/electron-hello`: one window, one page) as electron-packager
does, runs it, and counts it as `scripts/measure.sh` counts zebkit.

| Electron, Linux x86-64, MiB | Uncompressed | `.tar.xz` | `.zip` |
|---|---:|---:|---:|
| 44.9.0 as shipped | 283.3 | 87.1 | 117.2 |
| 44.9.0 trimmed: one locale, no licence file | **255.7** | **85.3** | **112.6** |
| 45.0.0-beta.5 as shipped | 273.9 | 88.9 | 117.8 |
| 45.0.0-beta.5 trimmed | 261.0 | 87.3 | 115.0 |

Of 44.9.0's 255.7 MiB, the `electron` binary (Chromium, Node.js and
Electron) is 218.9, the resource packs with the DevTools front end 13.9,
ICU's data for every language 10.4, SwiftShader and the Vulkan loader 6.8.
The trimmed numbers are the baseline: what an Electron application can ship
without building Electron.

## What Electron is made of, and what zebkit takes

```
Electron glue       app, BrowserWindow, ipcMain, menus, dialogs,        not taken
                    auto-update, asar, preload scripts
Node.js             libuv, Node's modules, N-API                         not taken
Chromium's content  the multi-process browser, without //chrome          taken
layer
```

| Part | What it is | In zebkit |
|---|---|---|
| `content/` | Processes, navigation, the sandbox, Mojo IPC, the DevTools backend | Kept: the API zebkit embeds through |
| Blink | DOM, CSS, layout, paint, the Web APIs and their V8 bindings | Kept, less the modules the requirements let go |
| V8 | JavaScript and WebAssembly, every JIT tier (Sparkplug, Maglev, Turbofan) | Kept |
| `cc/`, `components/viz`, Skia, ANGLE, SwiftShader | Compositing, the GPU process, 2D graphics, OpenGL ES, a GPU in software | Kept |
| `net/`, `services/network`, BoringSSL | HTTP/1 to 3, QUIC, cache, cookies, TLS | Kept |
| `media/`, FFmpeg, libvpx, dav1d | `<video>`, `<audio>`, MSE, EME with Clear Key | Kept, decoders only; FFmpeg with what applications need too |
| PDFium | Chrome's PDF library | Added: a content embedder has none (`docs/API.md`, PDF) |
| Dawn (WebGPU), WebXR, WebNN back ends, device APIs, CUPS, Kerberos, GTK and Qt shims | | Removed |
| WebRTC | | Still linked: Blink's platform library depends on it |

Blink cannot be embedded alone: it runs inside `content/`'s renderer
process, draws through `cc`, loads through the network service and runs on
V8. The Content API is the smallest unit; Electron, CEF, the content shell
and `headless_shell` embed the same.

## How the size came down

| Build, MiB | Package | `.tar.xz` | Engine |
|---|---:|---:|---:|
| First slim build, 2026-10-17 | 214.8 | 67.2 | 187.0 |
| `-Os`, relative vtables, features out | 173.0 | 51.5 | 157.8 |
| No CFI, packed relocations, more features out, ICU filtered | 151.9 | 47.7 | 139.5 |
| No test support, the size-trained inliner, `-Oz` off the hot path | 141.0 | 44.5 | 129.1 |
| No on-device model service, Blink's hot path with LLVM's own inliner | 139.1 | 43.8 | 127.1 |
| V8's compilers for size, the rest of V8 at `-O2` | 134.8 | 43.2 | 122.8 |
| The engine as a library with the API, its headers in the package | 142.5 | 44.6 | 123.6 |
| PDFium in the engine, FFmpeg for applications | 147.1 | | |
| **For speed: PGO, Blink's layout code at `-O2`, link-time optimization** | **157.3** | **53.9** | **137.6** |

What each change is (`profiles/slim.gn`, `patches/`):

- **`-Os`** in place of `-O2` for the code that is not on the hot path.
- **Relative vtables**: half the size, no relocation at load (14.5 to 6.3
  MiB of relocations).
- **Packed relative relocations**, applied by the engine itself so that
  they work on glibc before 2.36 (`patches/1005`): 6.3 MiB to under 0.1.
- **The resource pack** without the DevTools front end and the internal
  pages (`scripts/trim-pak.py`): 6.9 to 0.5 MiB.
- **ICU's data** for English, Chinese, Japanese and Korean
  (`scripts/build-icu-data.sh`): 9.2 to 6.5 MiB. The 2 to 4 MiB often
  quoted holds only without CJK, whose dictionaries and collation data are
  most of what stays.
- **No CFI checks**, which are hardening and not a feature.
- **Features out by switch**: Dawn, WebNN back ends, JPEG XL, AV1 encoding,
  CUPS, Kerberos, GTK, Qt, BlueZ, Widevine, WebXR, media remoting, Cast
  streaming, report delivery, VA-API, the OpenH264 encoder, PipeWire
  capture.
- **No web-test harness and no test support** in the shell
  (`patches/1006`, `1009`), which also runs the engine with Chromium's
  default features instead of its testing field trials.
- **No on-device model service** in the utility process (`patches/1010`),
  which took Dawn (3.0 MiB) and the SPIR-V tools with it.
- **The size-trained inliner** that Chromium uses for 32-bit Android, for
  Linux (`patches/1008`).
- **Optimization by directory** (`patches/1007`): `-Oz` for
  `content/browser`, `net`, the services, device access, the shell's window
  toolkit, tracing and storage; `-Os` for the rest; and directories a
  profile names kept at their own level.
- **V8's optimization chosen by the profile** (`patches/1011`): its
  optimizing compilers at `-Os` with the inliner (9.1 to 5.4 MiB), the rest
  at `-O2` as on Android.

## Speed

`scripts/bench.sh` times JavaScript and DOM workloads (loop, objects,
strings, JSON, regular expressions, sorting, DOM, parsing, GC, JIT); none
of the size changes above moved them beyond the spread of repeated runs.
All of V8 at `-Os` would have saved 3.6 MiB more, with JSON 45% slower,
regular expressions 55%, parsing 70% and JIT compilation 30%, and was not
taken. Maglev stays: 2.4 MiB for a JIT tier.

Text layout was what the size build lost. Laying out a chapter of an EPUB,
as a reader does:

| Build | ms | Package, MiB |
|---|---:|---:|
| Electron 44.9.0 (Chrome's own build of Chromium 152) | 6.8 | |
| Electron 36, an older release, for reference | 9.7 | |
| `slim`, size-built | 13.2 | 147.1 |
| with Chromium's PGO profile | 11.1 | |
| and Blink core, Blink's platform and HarfBuzz at `-O2` | 9.2 | |
| **and link-time optimization (`zebkit_lto_optimize`): adopted** | **8.7** | **157.3** |
| and without the size-trained inliner at the link | 8.2 | 171.4, not taken |

`zebkit_max_speed_patterns` in `profiles/slim.gn` is the list of
directories at `-O2` (a third tier in `patches/1007`). `slim-macos` is
built without PGO until Chromium's profile for macOS is fetched.

## Advice checked against the measurements

- **Locale packs.** A content embedder has no `locales/*.pak`. Not a lever.
- **Unwind tables.** Official builds leave them out on Linux already.
- **SwiftShader.** Kept on Linux: agents run on servers without a GPU, where
  it is the only way to composite and run WebGL. Left out on macOS, where
  every machine has a Metal GPU.
- **Open codecs only.** Ruled out: H.264 and AAC are required.
- **Compressing the executable.** Ruled out: every renderer process would
  unpack it again.
- **"Android ships Chromium in 50 to 70 MB."** True only of 32-bit ARM.
  Chromium's own builds of one commit for every platform, measured by
  loaded sections:

  | Build | CPU | MiB |
  |---|---|---:|
  | Content shell, Linux | x86-64 | 211.3 |
  | Content shell, Android | x86-64 | 177.9 |
  | Content shell, Android | arm64 | 161.4 |
  | Content shell, Android | 32-bit ARM | 91.7 |
  | WebView, Android | arm64 | 166.0 |

  On the same CPU Android is 16% smaller, and most of that is packed
  relocations and relative vtables, which `slim` has. WebView is the content
  shell's size. 32-bit ARM's Thumb-2 instructions make the small figures;
  x86-64 has no such encoding. Porting the Android engine would be a new
  embedder for little gain, and was not done.

## What would make it smaller, and what it costs

None of these is taken; they are listed for the case that size matters
again.

| Lever | MiB | Cost |
|---|---:|---|
| WebRTC out (Blink's platform depends on it: a patch over hundreds of files) | about 5 | Carried at every Chromium release |
| Device APIs, TFLite and XNNPACK out | about 4 | The same |
| Leftovers in the linker map: ANGLE's OpenGL back end, Skia's Graphite, the Lottie player, the IAMF decoder, kept type names | about 3 | |
| Back to the size build | 10 | Text layout 50% slower |
| No WebGL and GPU stack, compositing in software | 9 to 12 | No WebGL; heavy animation and canvas slower |
| V8 without Turbofan | about 9 | Compute-heavy script slower |
| No WebAssembly | 2 to 3 | Sites that use it break |
