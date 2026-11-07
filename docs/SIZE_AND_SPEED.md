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
| zebkit `slim`, 2026-11-06 | **171.2 MiB** | **59.4 MiB** | **79.5 MiB** |
| Electron 45.2.1, trimmed (the baseline) | 260.5 MiB | 87.1 MiB | 114.8 MiB |
| zebkit of Electron | 66% | 68% | 69% |

As an application ships it (`scripts/electron-compare.sh`'s size series:
the package without the headers, with the thin host). 2026-11-02: 160.8,
54.9 and 73.1 MiB; since then CFI on virtual calls (+1.8%), on indirect
calls (+1.15%) and the size model's cutoff (+2.1%) for speed (below), and the engine's own additions.

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

## Against Electron, measured the same way

`scripts/electron-compare.sh`, 2026-11-06, zebkit `slim` (Chromium
155.0.8059.84; CFI on indirect calls and the size model's cutoff of that
day, built in an experiment directory with `slim`'s arguments) against
Electron 45.2.1 (Chromium 156.0.8078.75), the latest stable release on npm
when it was chosen, on Linux x86-64 (a 12-core Ryzen with SMT). Every
measure, the median of 5 runs (7 for startup and layout) with the lowest and
highest run, is in `docs/reports/electron-compare.md`; a result is within
noise when those ranges overlap. Losses are shown as wins are. Startup and
size were measured again on 2026-11-07, with the cold-start read-ahead
below, 9 runs each.

What was held the same: one harness (`tests/bench/equal/compare.py`) that
does not know which engine it runs; one page set, served by the harness;
two thin hosts that do the same thing (`tests/bench/equal/host.c`,
`tests/bench/equal/electron/main.js`): one window whose content is 1280 by
800 on the same Xvfb display, focused, no menu, the same user agent, the
page's calls to the host answered with what they sent; each runtime packaged
as it ships (Electron's prebuilt release with the application in
`resources/app`); the same switches (both `--no-sandbox`, see below); one
clock, the harness's, with the page's marks on it; every process counted,
those an engine disowns too (the harness is a subreaper): CPU as utime and
stime from `/proc`, memory as PSS plus swapped PSS; the engines taking turns,
each first in every other pair; each pair started on a quiet machine (load
under 4.5, no build or suite; none had to be discarded). A cold start is
made without root: before each one the harness waits for the disk to do
under 2 MB/s, then takes the engine's files out of the page cache with
`posix_fadvise(POSIX_FADV_DONTNEED)` (its package, and every system file it
mapped in an earlier run; pages another process maps stay) and checks with
`mincore()` that none of the main binary is left (none was). It is not a
reboot: system libraries other programs have mapped stay in the cache, for
both. The machine's disk is a spinning one shared with other work, which
is what makes cold starts slow here, and other work on it during a run
slows it severalfold (one Electron run of 9 on 2026-11-07, 16.9 s).

| | zebkit | Electron 45.2.1 | Less |
|---|---:|---:|---|
| Size as published, MiB (`.tar.xz`, `.zip`) | 179.6 (60.9, 81.5) | 273.4 (88.7, 117.6) | zebkit, 34% (31%, 31%) |
| Size as an application ships it | 171.2 (59.4, 79.5) | 260.5 (87.1, 114.8) | zebkit, 34% (32%, 31%) |
| Start, cold (files out of the cache), to first paint, ms | 2279 | 6613 | zebkit, 66% (see below) |
| Start, warm, to first paint, ms | 116 | 236 | zebkit, 51% |
| Start, warm, CPU until the first frame, s | 0.21 | 0.46 | zebkit, 54% |
| Memory, a blank page, MB | 179 | 250 | zebkit, 29% |
| Processes, a page | 7 | 6 | **Electron**: zebkit runs storage in a process of its own, as Chrome does (below) |
| Text layout, ms a layout | 8.1 | 7.6 | Electron, 6%, within noise |
| Text layout, renderer's CPU, ms a layout | 10.9 | 10.3 | Electron, 6%, within noise |
| JavaScript and DOM, ms, all ten | 1139 | 1193 | zebkit, 5%, within noise |
| DOM (20,000 elements made and laid out), ms | 280 | 278 | level (Electron 1%, within noise) |
| A call from the page to the host and back, 64 bytes / 64 KiB, ms | 0.1 / 0.4 | 0.1 / 0.4 | level |
| The same with 1 MiB / 16 MiB, ms | 3.3 / 58.1 | 4.3 / 66.3 | zebkit, 23% / 12% |
| Long document (200,000 words), to its first frame, ms | 171 | 170 | level (within noise) |
| Long document, longest task, ms | 189 | 184 | Electron, 3%, within noise |
| Long document, scrolling frames p50 / p95 / max | 16.7 / 17.3 / 17.7 | 16.7 / 17.4 / 18.1 | level |
| Long document, CPU of all processes, s / memory, MB | 3.46 / 260 | 4.33 / 333 | zebkit, 20% / 22% |
| A 756-page PDF in each one's PDF viewer: CPU loading, s / memory, MB / renderers | 0.75 / 261 / 1 | 1.6 / 399 / 3 | zebkit, 53% / 35% |
| Five local pages and the PDF: CPU loading, s / at rest, % of a core / memory, MB | 3.0 / 5.6 / 1156 | 4.8 / 5.6 / 1572 | zebkit / level / zebkit |
| Ten sites: CPU loading, s / at rest, % of a core / memory, MB | 21.7 / 150 / 4756 | 25.2 / 161 / 5060 | zebkit, 14% / 6% / 6% |

- **Wins**: size; cold and warm startup; memory on every local page and
  on 8 of the 10 sites; CPU while loading on every local page and 8 of
  the sites (the other two within noise); the PDF viewer, which in Electron is an
  extension with two renderers more; calls between the page and the host,
  level up to 64 KiB and faster from 1 MiB; JavaScript.
- **Level since 2026-11-06**: the DOM workload (377 against 282 ms on
  2026-11-02, 280 against 278 now), the long document's first frame and
  longest task (were 16 to 19% slower), text layout within noise (was 9%
  slower, 12% in CPU). What closed it is below: CFI on indirect calls and
  the size model kept off more of the code the profile has.
- **Losses**: one process more a page; the memory of two sites, YouTube
  (684 against 583 MB) and The Verge (801 against 686, within noise),
  where the advertising and the video decide. The cold start, lost on
  2026-11-06 (7.4 against 6.5 s), is now zebkit's (below).
- **At rest on the sites** (`docs/reports/electron-compare.md`): the ten
  summed, 150 against 161% of a core; zebkit less on 6. Reddit and Stack
  Overflow showed a different page in some run and are left out.

What was done about the losses on 2026-11-02 and after, each measured with
the same harness before and after (the commits have the runs):

- **The cold start** (2026-11-07). Measured with the harness's own cold
  start, three ways in turn on the same quiet machine, 7 to 9 runs each,
  to first paint (lowest to highest): 0.1.0's package (2026-11-02) 4.33 s
  (4.27-4.44), today's `slim` 5.43 (5.22-5.46), Electron 6.58
  (6.45-6.96); in a second set 4.63 (4.43-4.84), 5.28 (5.05-5.51) and
  6.50 (6.31-6.72). So it had regressed, 14 to 25%, and the 7.4 s of
  2026-11-06 was that plus other work on the disk. Warm, nothing had
  changed (120 against 121 ms). A cold start here is time spent waiting
  for the disk: CPU 0.3 s in both. The browser and its first processes
  fault the library in (major faults 1015 against 955, 108 against 100 MB
  read), and what was in the cache after it, page by page, shows why:
  26,400 pages of the library in 220 separate ranges against 24,400 in
  181. `.text` grew from 121.5 to 139.0 MiB (CFI, LLVM's inliner on hot
  code, all taken for speed); lld puts the code the PGO profile has first,
  and that first 40 MiB is read almost whole, but the rest of what startup
  runs is spread over the other 100 MiB, most of it code the profile
  calls cold (`.text.unlikely`), so each range is a seek. Not the loader:
  `LD_DEBUG=statistics` gives both about 2.3 million cycles and the same
  relocations; nor TLS, ICU's data (the same 6.8 MB) or the request hooks
  (warm unchanged). Reading the whole library front to back takes 0.9 s
  on this disk, where faulting in two thirds of it took 4 s, so the
  browser process now does that, on a thread of its own as startup goes
  on (`engine.cc`, `PrefetchEngineFiles`), with its data files, as Chrome
  pre-reads chrome.dll on Windows and its library on Android. Chromium's
  `base::PreReadFile` would not do: on Linux it is `posix_fadvise()`,
  which the kernel cuts to one readahead window. It reads only when the
  library is not in the cache: 64 of its pages are looked at, and a warm
  start reads nothing (`mincore()` over all of it had cost the warm start
  35 ms). Same build with and without it, 7 runs each in turn: cold to
  first paint 5.49 (5.19-5.55) to 2.42 s (2.27-2.54), warm 120.8
  (117-130) and 121.8 ms (116-130), for 4 KiB. A cold start reads 170 MB
  instead of 108 (the cache holds it, not the processes: PSS at the
  first frame 185 MB, a warm start's 182). Linux only for now; macOS and
  Windows start as before.

- **Calls between the page and the host** (were 4 to 8 times slower):
  `zebkit.*` no longer goes as a `fetch()` to `zebkit://call` with its JSON
  parsed and written again twice; each frame's own Mojo channel carries
  the JSON and the bytes, through shared memory past 64 KiB, and
  `zebkit.app`'s JSON is checked and handed on unparsed, both ways
  (`docs/ARCHITECTURE.md`). 64 B 0.4 to 0.1 ms, 64 KiB 1.8 to 0.4, 1 MiB
  32.1 to 3.4, 16 MiB 540 to 56. The C API gained
  `zebkit_call_resolve_json()` (JSON with its length), which the bench host
  uses to answer with what it got, as Electron's handler does.
- **The DOM and text layout**: the size-trained inliner now decides only
  for code the PGO profile calls cold (`patches/1075`); LLVM's own inliner
  does Blink's hot code. DOM 406 to 370 ms in the same-objects comparison
  (style 19%, bindings 21% faster), text layout 4%, for 9.7 MB (+5.8% of
  the package): the DOM gains more than the size costs. And the
  thread-locals of V8 and Blink's heap are read initial-exec, in one
  instruction (`zebkit_tls_initial_exec`, `patches/1040`; the library
  already needs static TLS): the DOM 2%, its style 4% and text 5%, text
  layout 3%, the long document's longest task 3%, for at most 0.3 MB.
- **What is left, split against the reference.** Google's own Chrome
  155.0.8059.84 for Linux (the Chromium zebkit is built from), run by the
  same harness in an app window, medians of 5:

  | | zebkit | Chrome 155 | Electron (156) |
  |---|---:|---:|---:|
  | DOM, fresh page, ms | 352 | 251 | 248 |
  | Text layout, ms a layout (CPU) | 8.6 (12.0) | 7.6 (10.3) | 7.8 (10.6) |
  | Long document, longest task, ms | 210 | 186 | 172 |

  Chrome 155 is as fast as Electron on the DOM and on text layout, so
  those gaps were zebkit's build, not the version. zebkit built with
  Chrome's own arguments as far as a library allows
  (`profiles/slim-chrome.gn`) runs the DOM page in 254 ms, Chrome's speed,
  for 26 MB more. Taken apart one change at a time: vtables as pointers,
  nothing; V8 at -O3 and base, ICU, Skia and the bindings at -O2, nothing;
  no `-Os` and no size model anywhere, 2%. The rest was **CFI**: Chrome's
  PGO profile is recorded from a CFI build, whose checks are branches in
  every function with a virtual call; without them the profile's hashes
  do not match and the hottest layout functions
  (`BlockLayoutAlgorithm::Layout`, `HandleInflow`) were compiled with no
  profile at all (in `block_layout_algorithm.cc`, 172 functions with a
  profile against 224; the warnings that would say so are off in
  Chromium's build). slim now has CFI on virtual calls (`is_cfi`, not on
  calls through function pointers, which reach the application's
  callbacks): DOM 346 to 280 ms, text layout 8.6 to 8.1 ms (CPU 12.1 to
  11.2), the long document's longest task 213 to 201 ms, for 3.1 MB (+1.8%
  of the package). What is left to Chrome (280 against 252 ms on the DOM,
  8.1 against 7.6 on text layout) is mostly size optimisation and the
  checks on indirect calls Chrome's profile also has.
- **The profile's match, 2026-11-06.** Which functions get Chrome's profile
  was counted per file (each compiled to IR with the profile-mismatch
  warnings on, against `slim-chrome`): CFI on indirect calls gave back 7
  to 9 functions a file in the DOM and layout code (`element.cc` 692 to
  700 of Chrome's 701), sized deallocation and zeroed locals (both off in
  Chromium's size configuration) the last one or two. The DOM page runs
  again and again at Chrome's speed once matched, but the first layout of
  a fresh page did not. What still differed was the link: the size model
  inlined wherever the profile called a caller cold, and with the profile
  now matched, more of the first run's code than before counts as cold.
  Measured with the same objects linked each way, medians of 11
  alternating runs on a quiet machine, against Google's Chrome 155:

  | Step, each against the build before it in the same runs | before | after | Chrome 155 |
  |---|---:|---:|---:|
  | **CFI on indirect calls** (+2,093,077 bytes, +1.15% of the package) | | | |
  | DOM, first layout of a fresh page, ms | 243 | 230 | |
  | DOM, the same again and again, ms a round | 177.7 | 173.2 | |
  | Long document, longest task, ms | 199 | 198 | |
  | Text layout, CPU ms a layout | 10.98 | 11.28 | |
  | **The size model only below 99.9%** (+3,846,144 bytes, +2.08%) | | | |
  | DOM, first layout of a fresh page, ms | 236 | 222 | 204 |
  | DOM, the same again and again, ms a round | 179.1 | 161.2 | 174.6 |
  | Long document, longest task, ms | 204 | 191 | 188 |
  | Text layout, CPU ms a layout | 11.13 | 10.93 | 10.40 |

  Both were taken: each gains more on the DOM (5% and 3%; 6% and 10%) than
  it costs in size (1.15%; 2.08%), and the second the long document 6%.
  Text layout moved within noise. Not taken: sized deallocation
  with zeroed locals (+1.2%, no difference beyond noise), the size model's
  cutoff at 99% (+1.2%, less gain), no model at the link at all (+4.2%,
  no more gain than 99.9%). CFI on indirect calls cannot check a call to
  a function of the application's, which is outside the library, so
  `patches/1078` leaves exactly those calls unchecked: zebkit's own C API,
  the exported libraries' allocators, streams, destroy, font and unicode
  functions, and whole libraries where an application's function can be
  called from anywhere (an error handler, an allocator). A first draft
  that left `base/functional`, HarfBuzz and ICU out whole cost Mojo's and
  HarfBuzz's shaping code their profile (functions with one in
  `interface_endpoint_client.cc` 76 to 70, `hb-ot-layout.cc` 147 to 136;
  an application's font functions still cost `hb-font.cc` its own: 43 to
  16); Chromium's own list already leaves
  ICU's common code and `base::BindOnce` of a function pointer unchecked.
  `tests/engine/vendor.c` calls the libraries with an application's
  allocators, destroy, font and unicode functions, which a missing entry
  would stop with a trap.
- **Moving to Chromium 156** would take Electron's version: about 14 ms
  of the long document's first layout, nothing on the DOM or text layout.
  It is a new pin, the 76 patches carried over and every platform built
  again (about seven hours of building here, a day or two of porting).
  It belongs with the next Chromium update rather than on its own.
- **`page.screenshot`** (native 30 to 41 ms since 2026-11-01): the move to
  `Page.captureScreenshot` waits for a fresh frame, and the PNG encoder,
  Skia's in Rust, was compiled at opt-level s: the encoding crossed a frame
  boundary every time. Those crates now build at opt-level 3, as in Chrome
  (`patches/1076`, +29 KB): native 31.5 ms, over CDP 66.7 to 50.2 (Chrome
  33.6). Over CDP zebkit's headless capture still takes one frame more than
  Chrome's, JPEG too.
- **One process more a page**: the storage service runs in a process of
  its own, as in Chrome (and content_shell), where Electron runs it in the
  browser process. That process costs 3.9 MB PSS on a blank page (0.8 MB
  of it private), about 2% of the 167 MB, and keeps what parses the pages'
  stored data (IndexedDB, local storage, caches) in a sandbox. Kept, as
  Chrome has it.

What could not be made the same:

- **Chromium versions**: Electron 45.2.1 has Chromium 156, zebkit 155. Both
  send zebkit's user agent (Chrome 155); the client hints still say 156 for
  Electron.
- **The sandbox**: off in both. Each needs its set-user-ID helper installed
  as root or unprivileged user namespaces, which this machine refuses; both
  ship it on.
- **Sites**: Reddit showed zebkit a "prove your humanity" page in every run
  and Electron the page; Stack Overflow showed each a challenge in one run.
  Both are left out of the sums, and listed.
- **Defaults that differ** were left as each ships them, as an application
  gets them: zebkit blocks third-party cookies and keeps no spare renderer;
  Electron runs Node.js in its browser process.
- **Not measured**: a real application on both (the roadmap's one
  application, two hosts, beyond these thin hosts); an EPUB; cold start
  on macOS.

On macOS (Apple silicon, a person's desktop, Electron 45.2.1 for arm64,
`docs/reports/electron-compare-macos.md`), with memory as the sum of
`phys_footprint`, which leaves shared code out: zebkit starts warm to its
first paint in 135 ms against 229, with 27% less CPU, and costs 5 to 18%
less memory on the local pages; it lays text out 7% slower, runs the
JavaScript and DOM page 15% slower (`slim-macos` is built without PGO), and
its calls to the host were 7 to 22 times slower, before the call channel of
2026-11-02. The Mac was not measured again after it: its console was
locked (`IOConsoleLocked`), so windows there draw nothing and pages in them
wait. Headless, the same build passed the whole engine suite there (136
tests, the call channel's among them), as the 0.1.0 build passed its own;
the windowed comparison waits for an unlocked desktop. On the Mac, Electron's
window was not given the focus and zebkit's was. The Mac's runs used
the package before the change to `goto` (`docs/AUTOMATION.md`), which
changes none of these.

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
| 45.2.1 as shipped (2026-11-02, the latest stable) | 273.4 | 88.7 | 117.6 |
| **45.2.1 trimmed: the baseline since 2026-11-02** | **260.5** | **87.1** | **114.8** |

Of 44.9.0's 255.7 MiB, the `electron` binary (Chromium, Node.js and
Electron) is 218.9, the resource packs with the DevTools front end 13.9,
ICU's data for every language 10.4, SwiftShader and the Vulkan loader 6.8.
The trimmed numbers are the baseline: what an Electron application can ship
without building Electron. The baseline is the latest stable release on npm
when it is measured (`upstream.lock`), 45.2.1 (Chromium 156.0.8078.75) since
2026-11-02.

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
