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
| zebkit `slim`, 2026-10-27 | **158.7 MiB** | **54.3 MiB** | **72.3 MiB** |
| Electron 44.9.0, trimmed (the baseline) | 255.7 MiB | 85.3 MiB | 112.6 MiB |
| zebkit of Electron | 62% | 63% | 64% |

Own embedder (2026-11-01): the content shell's code replaced by the engine's, in place
of its browser classes. Package 168,729,926 to 168,590,133 bytes, the library 146,930,312
to 146,791,048: 0.08% less. Almost nothing, as what the shell linked was mostly needed;
Chrome's origin trial policy in place of the shell's costs a page (4 KiB) of it.

The engine library is 138.9 MiB of it; ICU's data 6.5 MiB, SwiftShader
with its Vulkan loader 3.6 MiB, the headers an application builds against
7.6 MiB, V8's snapshots, the resource pack and ANGLE the rest
(`docs/reports/slim.md`).

There was a limit of 150 MiB (owner, 2026-10-19), under which the engine
was 147.1 MiB with PDFium and FFmpeg's additions. On 2026-10-21 the owner
put speed first ("push the size limit to get the speed"): the size-built
engine laid text out twice as slowly as Chrome's own build. What is checked
now is speed against Chrome's build of the same version, and size against
Electron.

## PDF in pages: what it cost

Measured 2026-11-01, Linux `slim`, against the package before it (which also lacks the same build's
Playwright and CDP fixes, so this is an upper bound): library 146,512,520 to 146,872,968 bytes, +0.25%;
package +0.21%; library in xz +0.17 MB, +0.33%. No page without a PDF does anything more; no speed
was measured, none is expected. Chromium's plugin is that cheap because PDFium was in the engine already,
and `enable_pdf_ink2` and `enable_pdf_save_to_drive` are off. A 756-page document scrolled from end
to end kept its PDF renderer at 183 MiB.

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

Text layout was what the size build lost. Laying out a chapter of a book,
as a reader does, when it was decided (2026-10-21):

| Build | ms | Package, MiB |
|---|---:|---:|
| Electron 44.9.0 (Chrome's own build of Chromium 152) | 6.8 | |
| `slim`, size-built | 13.2 | 147.1 |
| with Chromium's PGO profile | 11.1 | |
| and Blink core, Blink's platform and HarfBuzz at `-O2` | 9.2 | |
| **and link-time optimization (`zebkit_lto_optimize`): adopted** | **8.7** | **157.3** |
| and without the size-trained inliner at the link | 8.2 | 171.4, not taken |

Now, 0.0.1-alpha.11 (2026-10-27): `scripts/layout-bench.sh` lays out a
chapter of about 9,000 words in columns a page wide, again after each
change of its width by a pixel, as a reader does when its window is
resized; the median of 50 layouts, the median of 5 runs, in each engine.

| Text layout, `scripts/layout-bench.sh` | ms |
|---|---:|
| Electron 44.9.0 | **9.2** |
| zebkit `slim`, 2026-10-27 | 10.9, 18% slower |

(The page is not hyphenated: Electron ships no hyphenation dictionaries,
and the engines would not lay out the same lines. The absolute times
drift with the machine's state from one session to the next; the ratio
holds, 18 to 19%, and so does the CPU per layout below.)

`zebkit_max_speed_patterns` in `profiles/slim.gn` is the list of
directories at `-O2` (a third tier in `patches/1007`). `slim-macos` is
built without PGO until Chromium's profile for macOS is fetched.

### Where the time went: the build, not the engine

The gap was taken apart on 2026-10-27, measuring the CPU time of the
renderer's main thread per layout of the same chapter (its lines, widths
and glyph advances were checked to be identical in every engine), the
median of five runs, each engine in turn, on an idle machine. What the
engines do turned out to be the same; how fast their code runs did not:

| CPU per layout, ms | | of Electron |
|---|---:|---:|
| Electron 44.9.0 (Chromium 152, Chrome's build flags) | **3.50** | 100% |
| Chrome 155.0.8059.84, Google's own build | 3.78 | 108% |
| zebkit `slim`, 0.0.1-alpha.11 | 4.42 | 126% |
| `slim` with vtables as pointers, initial-exec thread-locals and V8 at `-O3` | 4.43 | 127% |
| `slim-speed` (`-O2` throughout, no size-trained inliner) | 4.07 | 116% |
| `slim-speed` with those three | 3.91 to 3.96 | 112% |
| Chromium's content shell, official build, as Chromium links it (no zebkit change, `--lto-O0`) | 4.43 | 127% |

Ruled out, each by measurement: garbage collection (traced over CDP,
Electron collects more, not less, and is faster); fonts, line breaks and
justification (identical); the renderer's priority, its backgrounding and
visibility; a mismatch of Chrome's PGO profile (none, with the compiler's
warnings on); Blink's newly stable features; and the engine's own
patches, none of which touch layout or the heaps. The same factor shows
on the animated page: every thread of every process costs about 15%
more in `slim` than in Electron, and 5% less once built for speed.

What the library gives up for being one costs nothing measurable:
vtables as pointers (`use_relative_vtables_abi = false`), thread-local
variables of V8 and Blink's heap read in one instruction instead of a
call (`zebkit_tls_initial_exec`, `patches/1040`, kept as an option) and
V8 at `-O3` were built into `slim` together, for 5 MiB, and changed
neither layout (4.43 ms) nor the animated page; `slim` stays as it was.
What moves it is the compiler's optimization level: `slim-speed`, `-O2`
for all of the engine and no size-trained inliner at the link, is 7 to
11% faster at layout, costs 5% less on the animated page than Electron
instead of 15% more, and is 30 MiB larger (188 MiB; 60.8 MiB as
`.tar.xz` against Electron's 85.3) and 20 to 50 MB more memory a page
(a static article 195 MB against `slim`'s 175, Electron's 266). The two
things left that Chrome's build has could not be measured: PartitionAlloc
as malloc cannot be built into a library without taking the
application's malloc over (the shim crashed the host program at start),
and the packed relocations cost only at load. `slim` stays the release
(owner, 2026-10-27): a build that costs more in size than it gains in
speed is not an improvement, and `slim-speed` costs 19% in size for 7 to
11% in speed.

What remains is not zebkit's: Google's own build of Chromium 155 takes 8%
more CPU per layout than Electron's Chromium 152, and `slim-speed` is
within 4% of Google's build. Beating Electron on this test means
beating Chrome 155 at Chromium 155, which is a change of Chromium, not of
the build.

### Web pages

What pages cost to load and to keep showing, against Electron 44.9.0 on
the same display, with the same user agent, measured by
`scripts/web-bench.sh --sites` (`docs/reports/web-bench.md` has every page).
Fifteen pages: three local ones and twelve popular sites (Wikipedia,
Google, YouTube, GitHub, Amazon, Reddit, The New York Times, BBC News, The
Verge, Stack Overflow, Apple, MDN). Each is loaded for 10 s and shown for
20 s more; the CPU and the memory (PSS) of every process are counted, and
the median of 3 runs is taken, both engines in a 1280 by 800 window (before 2026-10-27 zebkit's content shell window stayed at its 800 by 600 default, which flattered it). 2026-10-27, zebkit `slim`:

Each page is opened alone, in an engine started for it, so the totals are
sums of separate runs, not one engine with many pages. Reddit showed
zebkit a reCAPTCHA in place of the page (Google's, which timed out) and
Electron a challenge it passed; with different pages to compare, it is
left out of the totals, which are over the other 14:

| 14 pages, one at a time | zebkit `slim` | Electron 44.9.0 | zebkit |
|---|---:|---:|---:|
| CPU, loading and at rest, all pages | **64 s** | 76 s | 16% less |
| CPU, a page on average | **4.6 s** | 5.5 s | |
| Memory, a page on average | **416 MB** | 498 MB | 16% less |
| Memory, the smallest page (a static article) | **172 MB** | 262 MB | 34% less |

Of a simple page's memory, about two thirds is the engine's own code: each
process maps the same library and PSS divides those pages among them, so
it is counted once and a second page does not add it again. For the
static article, 171 MB in all when it was broken down (0.0.1-alpha.6): the GPU process 61 MB (rendering in
software, SwiftShader, on a display without a GPU), the browser 40, the
page's renderer 37, the network and storage services 25, the two zygotes
10; of it 58 MB is memory the processes allocated and 113 MB their share
of the code. Busy sites cost more in their renderers' heaps: script,
images and advertising, one renderer for each site's frames (The New
York Times: 14).

zebkit costs less memory on all 14 pages and less CPU on 12; on BBC News
(6.1 s against 5.9) and Stack Overflow (9.0 against 9.0) the CPU is
level. BBC News varies with the advertising it is served: in three more
runs the same day it took 6 renderer processes and 497 MB twice, and 25
and 728 MB once. Two defaults account for much of the lead: third-party
cookies are blocked, which keeps advertising frames, and with site
isolation their processes, from syncing (The Verge: 9 renderers against
17), and no spare renderer is kept waiting.

Compiling for speed was measured on the same pages (0.0.1-alpha.6,
2026-10-24, over 15 with Reddit): `slim-speed`
(`profiles/slim-speed.gn`, `-O2` and Chromium's full PGO, `-Oz` only for
cold code) took 69 s of CPU against `slim`'s 73, with 2% more memory and
a package 27 MB larger (186 MB). Since `slim` already costs less than
Electron, `slim` stays the release build (owner, 2026-10-24).

`scripts/web-bench.sh --check` runs the local pages against the limits in
`tests/bench/web/limits.json`, so that a change that makes pages cost
more fails.

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
