# Roadmap: zebkit on Blink

How zebkit becomes a cross-platform engine built on Chromium's Blink, with
the size of an engine and not of a browser: what Electron, Chromium and
Blink are made of, which parts zebkit keeps, and the steps from where the
build is today to the size goal.

It checks the usual advice for slimming Chromium and Electron against what
the Linux build on this machine measures. Where the two disagree, the
measurement wins, and the disagreement is noted.

Status: draft for review, 2026-10-18; size limit changed 2026-10-19.

## 1. Goal and requirements

The product is an engine to embed in applications, agents among them, that
drive it over the Chrome DevTools Protocol. It is not a browser and not an
application framework.

| Requirement | Source |
|---|---|
| HTML, CSS and JavaScript run as in Chrome. Features beyond that core (WebRTC, WebGPU, WebXR, WebNN, device APIs) may go. | Owner, 2026-10-17 |
| CDP, compatible with Puppeteer, Playwright and raw clients | Owner |
| Media playback, H.264 and AAC included | Owner, 2026-10-10 |
| The JIT stays; no cut that costs much speed for little size | Owner, 2026-10-09 |
| ICU data for at least English, Chinese, Japanese and Korean | Owner |
| Linux: glibc 2.31 first, then 2.17 | Owner, 2026-10-17 |
| The same functions on Linux, macOS and Windows | Owner |
| Size: at most 150 MiB uncompressed, everything the package ships. 134.8 MiB is accepted (2026-10-19); the earlier 80 MiB target is dropped. | Owner, 2026-10-19 |
| A large size advantage over Electron, which is the baseline (section 3); the 150 MiB limit is what is checked | Owner, 2026-10-18 and 2026-10-19 |
| The libraries inside Chromium, and what else in it is useful, exported for applications as a C API, with bindings for other languages and for a page's JavaScript (section 7) | Owner, 2026-10-19 |
| Compressed size counts as well as the uncompressed one | Owner, 2026-10-18 |

Kept by zebkit's own choice, open to the owner's overrule: WebGL, WebAssembly,
HTTP/3 and the software GPU (SwiftShader). Removed by zebkit's choice:
WebRTC with camera and microphone capture, and the device APIs (WebAuthn,
USB, HID, serial, Bluetooth).

## 2. What Electron, Chromium and Blink are made of

Electron is three layers. zebkit takes the bottom one only.

```
Electron glue       app, BrowserWindow, ipcMain, menus, tray,     not taken
                    dialogs, auto-update, asar, preload scripts
Node.js             libuv, Node's modules, N-API (on Chromium's    not taken
                    V8, not a second copy)
Chromium's content  the multi-process browser, without //chrome    taken
layer               (Chrome's own UI, profiles, sync, extensions)
```

The content layer, by part, with what zebkit does with each:

| Part | What it is | zebkit |
|---|---|---|
| `content/` | Processes, `WebContents`, navigation, sandbox, Mojo IPC, the DevTools backend | Kept; it is the API zebkit embeds through |
| Blink (`third_party/blink`) | DOM, CSS, layout, paint, the Web APIs, their V8 bindings | Kept, less the modules the requirements let go |
| V8 | JavaScript and WebAssembly, with all JIT tiers | Kept |
| `cc/`, `components/viz` | Compositor, GPU process, display | Kept |
| Skia, ANGLE, SwiftShader | 2D graphics, OpenGL ES over the system's GPU API, a GPU in software | Kept |
| `net/`, `services/network`, BoringSSL | HTTP/1–3, QUIC, cache, cookies, TLS | Kept |
| `media/`, FFmpeg, libvpx, dav1d | `<video>`, `<audio>`, MSE, EME with Clear Key | Kept, decoders only |
| WebRTC, Dawn (WebGPU), WebXR, WebNN, device APIs | | Removed |
| PDFium, printing, spellcheck, translation, screen AI | Services of Chrome's browser layer | Not linked by a content embedder |
| `base/`, ICU, Mojo, Perfetto, crashpad | Foundations | Kept; ICU data filtered |

Blink cannot be embedded on its own. It runs inside `content/`'s renderer
process, draws through `cc`, loads through `services/network` and runs
script on V8. The smallest sensible unit is the Content API, which is what
Electron, CEF and Chromium's own `content_shell` and `headless_shell` embed.

## 3. Where the size goes: measured

### The baseline: Electron

`scripts/electron-baseline.sh` packages a hello-world application
(`examples/electron-hello`, one window with one page) the way
electron-packager does, runs it to show it works, and counts every file,
uncompressed and compressed, as `scripts/measure.sh` counts zebkit. Electron is the current
stable release, 44.9.0 (Chromium 152, Node.js 24), pinned in
`upstream.lock`. The report is `docs/reports/electron.md`. The script also
measures any other release beside it; the next one, 45.0.0-beta.5
(Chromium 156, one release ahead of zebkit's 155), is in
`docs/reports/electron-45.0.0-beta.5.md`.

| Electron 44.9.0, Linux x86-64, MiB | Uncompressed | `.tar.xz` | `.zip` |
|---|---:|---:|---:|
| As Electron ships | 283.3 | 87.1 | 117.2 |
| Trimmed as far as it goes without rebuilding: one locale, no license file | **255.7** | **85.3** | **112.6** |

| Electron 45.0.0-beta.5, Linux x86-64, MiB | Uncompressed | `.tar.xz` | `.zip` |
|---|---:|---:|---:|
| As Electron ships | 273.9 | 88.9 | 117.8 |
| Trimmed | 261.0 | 87.3 | 115.0 |

Electron 45 is 5.3 MiB larger trimmed than 44.9 (its binary is 224.1 MiB
against 218.9), so the 44.9.0 baseline is the stricter comparison.

Compressed sizes are of the whole package directory: `.tar.xz` with
`xz -6`, as zebkit's releases ship (`scripts/release.sh`), and `.zip` at the
highest deflate level, the form Electron is distributed in. Both are
computed by one helper, `compressed_size` in `scripts/lib.sh`, for Electron
and zebkit alike.

Of the 255.7 MiB uncompressed:

| Part | MiB |
|---|---:|
| The `electron` binary (Chromium, Node.js and Electron, stripped) | 218.9 |
| `resources.pak` and the scale packs, DevTools front end included | 13.9 |
| ICU data, all languages | 10.4 |
| SwiftShader and the Vulkan loader | 6.8 |

The trimmed numbers are the baseline: 255.7 MiB uncompressed and 85.3 MiB
as `.tar.xz`. They are what an Electron application can ship without
building Electron itself.

The size requirement is a limit of 150 MiB uncompressed (owner,
2026-10-19), 59% of the baseline; there is no separate bar against
Electron. The engine at 134.8 MiB is just over half of the baseline.

### zebkit

The numbers below are for Linux x86-64, Chromium 155.0.8059.84, from
`scripts/measure.sh` (the package) and `scripts/mapsize.py` (the linker map
of the engine).

| Build, MiB | Package | `.tar.xz` | `.zip` | Engine binary | Of Electron, uncompressed | Of Electron, `.tar.xz` |
|---|---:|---:|---:|---:|---:|---:|
| First slim build, 2026-10-17 | 214.8 | 67.2 | 90.0 | 187.0 in the map | 84% | 79% |
| `slim` measured 2026-10-18 | 173.0 | 51.5 | 70.8 | 157.8 | 68% | 60% |
| `slim` measured 2026-10-18, step 1 done | 151.9 | 47.7 | 65.0 | 139.5 | 59% | 56% |
| `slim` measured 2026-10-19: no test support, size-trained inliner, `-Oz` off the hot path | 141.0 | 44.5 | 60.3 | 129.1 | 55% | 52% |
| `slim` measured 2026-10-19: no on-device model service, Blink's hot path with its own inliner | 139.1 | 43.8 | 59.4 | 127.1 | 54% | 51% |
| `slim` measured 2026-10-19: V8's compilers for size, the rest of V8 at `-O2` (accepted) | 134.8 | 43.2 | 58.3 | 122.8 | 53% | 51% |
| `slim` measured 2026-10-20: the engine as `libzebkit.so.1` with the API of section 7, its headers included | **142.5** | **44.6** | **60.2** | 123.6 | 56% | 52% |

Against Electron, both releases:

| zebkit 134.8 MiB against | Uncompressed | `.tar.xz` | `.zip` |
|---|---:|---:|---:|
| Electron 44.9.0, trimmed (the baseline) | 53%, 1.90× smaller | 51%, 1.97× smaller | 52%, 1.93× smaller |
| Electron 44.9.0, as shipped | 48%, 2.10× smaller | 50%, 2.02× smaller | 50%, 2.01× smaller |
| Electron 45.0.0-beta.5, trimmed | 52%, 1.94× smaller | 49%, 2.02× smaller | 51%, 1.97× smaller |
| Electron 45.0.0-beta.5, as shipped | 49%, 2.03× smaller | 49%, 2.06× smaller | 49%, 2.02× smaller |

zebkit's package compresses to 32% of its size and Electron's trimmed one
to 33%, so the advantage is a little larger compressed than uncompressed.
Against Electron as its users actually ship it, zebkit is half the size or
less in every form.

The engine binary is nearly all of the package, and code is nearly all of
the binary: 106.2 of 121.5 MiB in the last map. The table below is from
the 139.1 MiB build; since then V8 is 4.1 MiB smaller. The largest parts:

| Part | MiB | | Part | MiB |
|---|---:|---|---|---:|
| Blink core | 15.4 | | Skia | 5.2 |
| V8, without its compiler | 10.7 | | ANGLE | 4.8 |
| V8's optimizing compiler | 9.1 | | WebRTC | 3.1 |
| `content/browser` | 7.0 | | Rust crates | 2.8 |
| Blink's V8 bindings | 5.3 | | `net` | 2.7 |
| Strings and constants the linker merges | 4.4 | | Blink platform | 2.7 |
| C runtime and libc++ | 3.7 | | Parts under 0.5 MiB each | 22.9 |

`docs/reports/slim-map.md` has the whole table.

What this says about the usual advice:

- **Locale packs.** A content embedder has no `locales/*.pak`; the strings
  Blink needs are in the one resource pack. Not a lever here.
- **The resource pack.** Trimmed from 6.9 to 0.5 MiB by leaving out the
  DevTools front end and the internal pages (`scripts/trim-pak.py`). Done.
- **ICU data.** 9.2 MiB as Chromium ships it, 6.5 MiB with zebkit's filter
  (`scripts/build-icu-data.sh`). The 2–4 MiB often quoted holds without
  Chinese, Japanese and Korean, whose dictionaries and collation data are
  most of what stays.
- **Unwind tables.** Official builds already leave them out on Linux; the
  binary has no `.eh_frame`.
- **ThinLTO settings.** The content shell links with `--lto-O0`, so the
  import limit makes no difference, and a higher LTO level would make the
  binary larger.
- **SwiftShader.** Kept on Linux: agents run on servers without a GPU, where
  it is the only way to composite and run WebGL. 4.0 MiB with the Vulkan
  loader.
- **Open codecs only.** Ruled out by the media requirement.
- **"Android ships Chromium in 50–70 MB."** True only of 32-bit ARM, as
  section 4 measures. Chromium's own build files give 178 MB of arm64
  native code for Chrome on Android in M124
  (`build/config/compiler/BUILD.gn`).

## 4. The Android engine, investigated

The question: Chromium is much smaller on Android than on the desktop, so
would taking the Android browser engine and porting it to Linux, Windows and
macOS give a small engine?

### What the Android engine is

There is no separate Android engine. Android WebView (`//android_webview`)
and Chrome for Android are built from the same Blink, V8 and `content/` as
the desktop, in the same source tree. WebView is a Content-API embedder,
exactly what zebkit's own embedder will be (step 2 in section 5): it uses
`content/` and what is under it, and nothing of `//chrome`. What makes it
Android is the embedder layer and the build settings, not the engine.

### What it weighs

Chromium publishes builds of every platform from the same commits
(`chromium-browser-snapshots`). The builds below are within three commits of
each other (revisions 1721514–1721517, 2026-10-18), all developer builds
with the same settings. That makes their absolute sizes larger than an
official build but their ratios comparable. Each number is the binary's
loaded sections, unwind tables left out (official desktop builds have none).

| Build | CPU | Code | Other loaded data | Total, MiB |
|---|---|---:|---:|---:|
| Content shell, Linux | x86-64 | 171.4 | 40.0 | 211.3 |
| Content shell, Android | x86-64 | 157.9 | 20.0 | 177.9 |
| Content shell, Android | arm64 | 144.1 | 17.3 | 161.4 |
| Content shell, Android | 32-bit ARM | 77.4 | 14.4 | 91.7 |
| WebView, Android | x86-64 | 162.0 | 20.9 | 182.9 |
| WebView, Android | arm64 | 147.9 | 18.1 | 166.0 |
| WebView, Android | 32-bit ARM | 79.4 | 15.1 | 94.5 |

What the table says:

1. **On the same CPU, the Android engine is 16% smaller, not several
   times.** Of the 33.4 MiB difference, 15 MiB is the relocation table,
   which the Linux build stores unpacked and Android packs to under 0.5 MiB,
   and 1.7 MiB is vtable data. zebkit's `slim` profile packs relocations
   and uses relative vtables already (section 5, step 1). The rest is
   13.5 MiB of code, 8%: the parts Android builds differently, its window
   system among them (Android has no Aura, Views, X11 or Wayland).
2. **WebView is the content shell's size,** 3% more. It is not a slimmer
   engine.
3. **The small Android figures are 32-bit ARM.** It is 43% smaller than
   arm64 with the same source: Thumb-2 instructions take 2 or 4 bytes, and
   Chromium builds only 32-bit Android for size. In this tree, 64-bit Android
   counts as "high end" and is built for speed (`is_high_end_android` in
   `build/config/chrome_build.gni`). x86-64, which zebkit ships on Linux and
   Windows, has no compact encoding of that kind.
4. **arm64 is 9% smaller than x86-64.** zebkit gets that on macOS already,
   and would on Windows and Linux on ARM.

### What a port would take

WebView's embedder is 70 thousand lines of Java and 53 thousand of C++, a
third of the C++ files tied to Android through JNI. It draws into an
Android app's view hierarchy, and relies on the system for windows, the GPU
driver, media codecs, fonts and crash reporting. On the desktop each of
those has to come back, and what brings them back is the desktop code the
content shell already has.

### Conclusion

Porting the Android engine would cost the work of a new embedder and gain
little on x86-64: what is portable in it, relocation packing and relative
vtables, zebkit already has. Not taken. What is taken from Android:

- Its size settings that work on x86-64. Packed relocations, relative
  vtables and `-Os` are in `slim`, and so is the size-trained inliner
  Chromium uses for 32-bit Android, except on Blink's hot path (section 5,
  step 4).
- Its shape: an embedder on the Content API with no browser UI and no
  `//chrome`, which is section 5, step 2.

## 5. The route

### Step 0. Measure the stock build: done

The content shell as an official release build, measured by component.
Linker maps are generated on every build, and `scripts/mapsize.py`
attributes them by source directory, which is what supersize does.

### Step 1. The build-configuration diet: done

Everything Chromium has a switch for, plus small patches where a switch
needs one. In `profiles/slim.gn`:

| Change | Measured |
|---|---|
| `-Os` instead of `-O2`, without PGO | in 214.8 → 173.0 |
| Relative vtables (half the size, no load-time relocation) | in 214.8 → 173.0; relocations 14.5 → 6.3 MiB |
| Resource pack without DevTools and internal pages | 6.9 → 0.5 MiB |
| No CFI checks (hardening, not a feature) | in 173.0 → 151.9 |
| No Dawn, WebNN back ends, JPEG XL, AV1 encoding, CUPS, Kerberos, GTK, Qt shims, BlueZ, Widevine | in 173.0 → 151.9; Dawn stayed linked through the on-device model service until `patches/1010` (step 2) |
| Packed relative relocations, applied by the engine itself so that they work on glibc before 2.36 (`patches/1005`) | in 173.0 → 151.9; relocations 6.3 → under 0.1 MiB. The engine starts and serves CDP. |
| No WebXR, media remoting, Cast streaming, report delivery, VA-API, OpenH264 encoder, PipeWire capture | in 173.0 → 151.9 |
| No web-test harness in the shell (`patches/1006`) | in 173.0 → 151.9 |
| Locals not zero-initialized by the compiler (`init_stack_vars`), as on Android | in 173.0 → 151.9 |
| ICU data for English and CJK | 9.2 → 6.5 MiB |

The changes from 173.0 to 151.9 were built together, so the 21.1 MiB is not
split between them.

PDF, printing and on-device translation have switches that Chrome's own
build files assert on, and a content embedder does not link them anyway.

### Step 2. zebkit's own embedder in place of the content shell

The content shell is a test program. Through Chromium's test support it
linked a benchmark library (libpfm4, 1.5 MiB), test services and Blink's
testing internals. The first part of this step is done:

- `patches/1009-content-shell-without-test-support.patch` takes the test
  support out of the content shell. What the shell needs of it, it compiles
  itself; the services only tests use are left out. It also makes the
  engine run with Chromium's default features instead of its testing
  configuration of field trials (see "Speed" below).
- `patches/1010-utility-without-on-device-model.patch`: the on-device model
  service, which runs the models behind Chrome's built-in AI APIs, was
  linked into every utility process whatever its switch said, and Dawn
  (3.0 MiB) and the SPIR-V tools (0.8 MiB) with it. Now only where the
  switch is on; the content shell has no model to run.

TFLite and XNNPACK (2.3 MiB) are still linked, through WebRTC's echo
estimator, the audio service's voice isolation and the WebNN service
(step 3).

For scale: Chromium's own production embedder for automation,
`headless_shell`, which links no test support, is 2.3% smaller than the
content shell from the same commit (206.5 against 211.3 MiB in developer
builds).

Then zebkit replaces the shell with an embedder written for the job:

- `ContentMainDelegate`, `ContentBrowserClient`, `ContentRendererClient`,
  `ContentUtilityClient` and a `WebContentsDelegate`, started from
  `content/shell` without its test hooks.
- **The form zebkit has today**: a shared library the application loads,
  `libzebkit.so.1` (a framework on macOS, a DLL on Windows), and a small
  helper program for the renderer, GPU and utility processes. That keeps the
  embedding API stable across the engine switch.
- **Windows.** Aura and Views on Linux and Windows; Cocoa on macOS. The
  view is a child of the application's window, as the WebKit ports do now.
  Electron's `shell/browser/native_window_views.cc` is a reference for this.
- **CDP** from `content/`'s DevTools backend: `DevToolsAgentHost`, the
  remote-debugging port and pipe, as Chrome serves them. `//headless` is the
  reference for the browser-level domains an automation client expects
  (`Target`, `Browser`), and real Chrome is what the results are compared
  with (`docs/CDP.md`).
- **Page-to-application calls**, when an embedder wants them: an object
  bound with gin in the renderer, carried over Mojo to the browser process.
- **No Node.js.** What an application needs beyond the page comes from the
  engine's own libraries, through zebkit's API (section 7).

Expected: the test support and the shell's own UI out of the binary, 3–5
MiB.

### Step 3. Patches for what has no switch

The parts the requirements let go but that `content/` and Blink's modules
link unconditionally. Each is a patch to carry through every Chromium
release, so each has to be worth it:

| Part | In the last map |
|---|---:|
| WebRTC, with Blink's peer connection and capture modules | 3.5 MiB and more |
| Device APIs: `device/`, `services/device`, WebAuthn | 1.5 MiB and more |
| TFLite and XNNPACK, for WebRTC's echo estimator, the audio service's voice isolation and WebNN | 2.3 MiB |

WebRTC is the costliest to carry: Blink's platform library depends on it,
not only the peer connection modules, and 349 dependency paths lead from
the shell to it. A patch would span hundreds of files at every Chromium
release, for about 5 MiB. It comes last, if the budget still needs it.

### Step 4. The compiler, measured against speed: done

- **The size-trained inliner.** Chromium inlines with a model trained for
  size when it builds 32-bit Android, and asserts that only Android asks
  for it; `patches/1008-ml-size-inliner-for-linux.patch` lifts that for
  Linux. Chromium's `-Os` configuration passes the model to the compiler,
  so it applies to everything at `-Os`; V8 builds at `-O3` and does not
  get it.
- **`-Oz` off the hot path.** `content/browser`, `net`, the services,
  device access, the shell's window toolkit, tracing and storage, through
  `patches/1007-min-size-for-chosen-targets.patch`. Blink, V8, Skia, the
  compositor, `base`, media and BoringSSL keep `-Os`.
- **Blink's hot path keeps LLVM's own inliner.** With the model, laying out
  20,000 lines of text took a third longer. The same patch lets a profile
  name directories that keep `-Os` with the default inliner; `slim` names
  Blink core (DOM, style, layout) and the bindings. That costs 2.4 MiB and
  brings layout back to the speed of the 151.9 MiB build. Blink's platform
  library, ICU and HarfBuzz were tried too and made no difference.

Measured together with step 2: 151.9 → 141.0 MiB with the model everywhere
at `-Os`, 139.1 MiB with Blink's hot path excluded and the on-device model
service gone.

**Speed** (`scripts/bench.sh`, best of three, milliseconds, lower is
better):

| Build | loop | objects | strings | json | regex | sort | dom |
|---|---:|---:|---:|---:|---:|---:|---:|
| 151.9 MiB | 41–43 | 94–111 | 28–33 | 272–311 | 281–329 | 96–107 | 599–657 |
| 151.9 MiB with Chromium's default features | 41–44 | 101–103 | 30 | 284–302 | 293–321 | 90–97 | 642–666 |
| 139.1 MiB | 39–40 | 103–105 | 28–33 | 261–276 | 276–291 | 89–99 | 641–693 |

The ranges are the spread over repeated runs on an idle machine. JavaScript
is unchanged. The DOM workload is up to 7% slower, and that is not the
compiler: the 151.9 MiB build ran with Chromium's testing configuration of
field trials, which turns on experiments, and without it
(`--disable-field-trial-config`) it is as fast as the new one. In that configuration one experiment makes
layout after a width change twice as fast; which one, and whether zebkit
should turn it on, is open.

- **V8 compiled for size where speed does not suffer.** On the desktop V8
  replaces Chromium's optimization with `-O3`, so none of the above reached
  it. `patches/1011-v8-optimized-for-size.patch` lets a profile choose
  V8's optimization, for all of V8 and for its optimizing compilers
  (Turbofan and Turboshaft) apart. `slim` compiles the compilers at `-Os`
  with the size-trained inliner (9.1 → 5.4 MiB) and the rest of V8 at
  `-O2`, as Chromium does on Android (10.7 → 10.3 MiB): 139.1 → 134.8 MiB,
  with no difference in `scripts/bench.sh`, whose new `parse`, `gc` and
  `jit` workloads time V8's own C++. All of V8 at `-Os` with the
  size-trained inliner gave 131.2 MiB, but JSON was 45% slower, regular
  expressions 55%, parsing 70% and JIT compilation 30%, so it was not
  taken.

- V8's Maglev tier stays: it is 2.4 MiB, and taking it out would cost
  speed for little size.

### Step 5. Platforms

- **glibc 2.17.** Packed relocations no longer depend on the glibc version
  (step 1). What is left is building on an older sysroot and checking which
  newer symbols the engine pulls in.
- **Windows and macOS**, from the same profile, with the same functions as
  Linux.
- **The sandbox where there is none.** Chromium's Linux sandbox needs
  unprivileged user namespaces or a set-user-ID helper. Containers and
  build machines often have neither, and the engine then refuses to start
  unless it is told to run without a sandbox. Agents run in such places, so
  zebkit needs a stated answer: ship the helper, document
  `--no-sandbox` for containers, or both.

## 6. Budget

The limit is 150 MiB uncompressed (owner, 2026-10-19), and the 134.8 MiB
build is accepted. That leaves 15 MiB of headroom for zebkit's own embedder
and for Chromium's growth from release to release.

### A. What costs no web feature: done for size

| | MiB |
|---|---:|
| Package after step 1, measured | 151.9 |
| Test support out of the shell (step 2, `patches/1009`), size-trained inliner and `-Oz` (step 4), measured together | −10.9 |
| On-device model service out (step 2, `patches/1010`), Blink's hot path without the inliner (step 4), measured together | −1.9 |
| V8's compilers for size, the rest of V8 at `-O2` (step 4, `patches/1011`), measured | −4.3 |
| **Package now, measured** | **134.8** |
| As `.tar.xz` | 43.2 |

What remains of A is no longer needed to meet the limit:

- zebkit's own embedder (step 2) is needed for the product, not for size;
  it should take 1–2 MiB out with the shell's UI.
- WebRTC, TFLite and the device APIs (step 3, about 6 MiB) are patches to
  carry at every Chromium release. Not done unless the limit tightens.
- Smaller leftovers found in the linker map, about 3 MiB together: ANGLE's
  OpenGL back end beside its Vulkan one, Skia's Graphite back end (which
  cannot run without Dawn), the Lottie player, the IAMF decoder, and C++
  function and type names kept as strings.

The size-trained inliner gave less in the linked binary than in the
sample: much of what it saves are copies of inline functions that the
linker folds anyway, and Blink core and the rest of V8 keep LLVM's own
inliner for speed.

Chromium's size advice is often that a size-built main binary is 45–60 MB.
The parts the requirements keep add up to more than that on x86-64: Blink
(about 25 MiB with its bindings and platform), V8 (22), the content layer
and network stack (about 14), Skia and ANGLE (10), the C runtime and merged
constants (9), and a long tail of small components (24).

### B. What costs a feature or speed: not needed

None of these is needed under the 150 MiB limit. They stay listed for the
case that the limit tightens:

| Lever | Estimate, MiB | What it costs |
|---|---:|---|
| No WebGL and GPU stack (ANGLE, SwiftShader, the GPU command buffer); compositing in software | 9–12 | No WebGL; heavy animation and canvas slower |
| V8 without Turbofan; Maglev and Sparkplug, both JIT tiers, stay | about 9 | Long-running, compute-heavy script slower; typical pages much less |
| No WebAssembly | 2–3 | Sites that use Wasm break |
| Blink's modules cut to the HTML, CSS and JavaScript core: payments, speech, USB, notifications, file system access and the like | 2–4 | Those APIs are gone; all of Blink's modules together are 4.4 MiB |
| CJK ICU data as a separate file, English built in | 3–4 | Counts only if the separate file is optional |

From the 134.8 MiB build, all of B together would give roughly 103–110 MiB.
80 MiB, the earlier target, was out of reach with the requirements of
section 1.

Ruled out:

- Compressing the executable: the target is the uncompressed size, and a
  self-unpacking binary would unpack again in every renderer process.
- Electron's or CEF's own builds: larger, not smaller.
- Servo or Ladybird: too far from Chrome's site compatibility, and no CDP.
- Windows' system WebView2: a different engine with a different API from
  Linux and macOS.
- Porting the Android engine (section 4).

For comparison, zebkit on WebKit was 83.1 MiB on Linux with media playback.
WebKit was left for its three separate ports, not for its size.

## 7. Native APIs: Chromium's libraries for applications

### Why

Chromium carries a library for nearly everything an application needs
besides a web page: compression, cryptography and TLS, SQLite, ICU, image
codecs, fonts and text shaping, audio and video codecs, and a network stack
with HTTP/2, HTTP/3, a cache and cookies. Electron has all of them inside
its binary and exposes none: its native surface is Node.js, and an
application that wants SQLite or an image codec adds an npm module with a
second copy, compiled for each Electron version.

zebkit exposes what is inside the engine instead. To native code it is a C
API, with bindings for other languages on top; to a page's JavaScript it is
a `zebkit` object, in pages the application allows. The code is in the
binary already, so the cost is the parts of a library the engine did not
link before, which is measured for each one against the 150 MiB limit.

### The layers

| Layer | What it is | Form |
|---|---|---|
| 0. Core | Start the engine, run its loop, make views, load pages, run script, CDP, calls from a page to the application | `<zebkit/zebkit.h>` |
| 1. Libraries | The C libraries inside the engine, with their own APIs | `<zebkit/vendor/<library>.h>`, as FFmpeg was on WebKit (`docs/VENDOR.md`) |
| 2. Services | A C API over Chromium's own C++: what is not a library of its own | `<zebkit/<area>.h>` |
| 3. Bindings | Layers 0–2 for other languages | Generated from the C headers |
| 4. JavaScript | Layers 0–2 for the page's script | `zebkit.*`, granted by the application |

**Layer 1** keeps the rules zebkit had for FFmpeg: the application writes
the library's own API; every function is exported from the engine as
`zebkit_vendor_<library>_<function>`, and the header maps one name to the
other, so another copy of the same library in the process cannot collide
with the engine's. What is exposed is what the engine is built with, found
in the build, not listed by hand. Only C libraries: a C++ library inside the
engine is compiled against Chromium's own copy of libc++, whose types an
application cannot share, so the C++ ones (Skia, V8, Chromium's `net` and
`base`) are reached through layer 2.

| Library | For |
|---|---|
| zlib, Brotli, Zstandard | Compression |
| BoringSSL | Hashes, ciphers, signatures, key exchange, certificates, TLS |
| SQLite | Databases |
| ICU | Unicode: normalization, case, segmentation, collation, formatting |
| HarfBuzz, FreeType | Text shaping, font files |
| libxml2 | XML and HTML parsing, XPath |
| libjpeg-turbo, libwebp | JPEG and WebP |
| FFmpeg, dav1d, Opus, libvpx | Audio and video: demuxing, decoding, VP8/VP9 and Opus encoding |

**Layer 2**, first areas, each a small header of plain functions:

| Header | What | From |
|---|---|---|
| `net.h` | HTTP requests: HTTP/1.1, HTTP/2, HTTP/3, the engine's cache, cookies and proxy settings | The network service |
| `url.h` | Parsing and canonicalizing URLs; the registrable domain (public suffix list) | `url/`, `net/base` |
| `image.h` | Decoding PNG, JPEG, WebP, GIF, AVIF, BMP and ICO; encoding PNG, JPEG and WebP | Skia's codecs |
| `compress.h` | gzip, deflate, Brotli and Zstandard in one call | zlib, Brotli, Zstandard |
| `crypto.h` | Digests, HMAC, random bytes, AES-GCM | BoringSSL |

Later areas: 2D drawing (Skia's canvas), shaping a line of text, decoding
media frames, a key-value store, and the desktop services Electron has
(clipboard, notifications, file dialogs, screens, power, the system's
keychain) from `ui/` and `components/`. And script outside any page: a V8
isolate in the application's process, for an application written in
JavaScript, as Electron's main process is.

**Layer 3** is generated from the C headers by one script, so the headers
stay the one definition. Python (with `ctypes`, nothing to compile) and
Rust (the existing crates' raw layer) first; Go, C# and Node's N-API follow
the same generator.

**Layer 4** gives a page the same capabilities, as promises:

| In the page | What |
|---|---|
| `zebkit.app.<name>(...)` | A function the application binds (`zebkit_view_bind`) |
| `zebkit.fetch(url, options)` | A request through the engine's network stack, without CORS |
| `zebkit.fs` | Reading and writing files under a directory the application grants |
| `zebkit.compress`, `zebkit.crypto`, `zebkit.image`, `zebkit.url` | Layer 2 |

A page gets nothing unless the application grants it, per view, per origin
and per capability; frames of other origins get nothing. In the renderer
the object is bound with gin; each call goes over Mojo to the browser
process, where layer 2 does the work, so the renderer stays sandboxed.

### What it decides

- **The engine's form.** An API in the application's process needs the
  engine as a shared library: `libzebkit.so.1`, and `zebkit-helper` for the
  renderer, GPU and utility processes, as zebkit was on WebKit. It is
  started the way Android WebView starts it: `BrowserMainRunner`
  initialized by the embedder, its loop run when the application asks.
- **The allocator.** A library must not replace the application's
  `malloc`, so PartitionAlloc no longer does
  (`use_partition_alloc_as_malloc = false`); Blink and V8 keep their own
  heaps. Measured with `scripts/bench.sh`.
- **Packed relocations** have to work in a shared library too: from
  `.init_array`, with the library's own thread-local block.

### Order of work

| Phase | What | State |
|---|---|---|
| N1 | The engine as `libzebkit.so.1` and `zebkit-helper`; layer 0 | Done 2026-10-20 |
| N2 | Layer 1: the generator and the libraries above | Done 2026-10-20: 14 libraries, 6,686 functions; FreeType not yet |
| N3 | Layer 2: `net`, `url`, `image`, `compress`, `crypto` | Done 2026-10-20, in one header, `<zebkit/services.h>` |
| N4 | Layer 3: Python, then Rust | Python done 2026-10-20; Rust next |
| N5 | Layer 4: `zebkit.app`, then the capabilities | Done 2026-10-20 |
| N6 | Script outside a page; later areas of layer 2 | Script done 2026-10-20: `zebkit_script_*`, `zebkit-js` |
| N7 | Native UI from JavaScript: `zebkit.ui` on `ui/views` (below) | First version done 2026-10-20: elements, signals, Tailwind classes, automation, capture; the measurement against a page is next |

Each phase is measured for size and speed and tested from a program that
uses it. `docs/blink/API.md` is what is built, and
`scripts/engine-test.sh` tests every layer from programs built against the
package.

### Where it stands, 2026-10-20

- **Size.** The package is 142.5 MiB (44.6 MiB as `.tar.xz`), under the
  limit. The library is 0.8 MiB larger than the content shell was; the
  libraries' headers, only those an application's include reaches, are
  7.6 MiB.
- **Speed.** The same as the content shell's within `scripts/bench.sh`'s
  spread, without PartitionAlloc as `malloc` and with the thread-local
  model of a library for V8 and Blink's heap (`zebkit_v8_tls_local_dynamic`
  in `patches/1011`, `blink_heap_inside_shared_library`, which CEF uses
  for the same reason).
- **Layer 4 is built on the view's DevTools session** rather than on new
  renderer code: `Runtime.addBinding` and a script that runs before the
  page's, the way Playwright exposes functions. The browser decides the
  calling frame's origin; frames of other sites are other targets and get
  nothing. Bytes travel as base64, slow for hundreds of megabytes; a Mojo
  channel is the faster way if it is needed.
- **Still the content shell's classes** for windows and the browser
  context, test-only in Chromium; zebkit's own embedder (step 2) replaces
  them.
- **Loading with `dlopen()`.** A thread that existed before the library
  was loaded keeps a stale copy of one pointer of `base`'s thread-local
  data, which the library cannot relocate. Linking the library, or loading
  it before threads start, is what is supported.

### Native UI from JavaScript (N7)

Asked for by the owner on 2026-10-19 and 2026-10-20: applications written
in JavaScript, with their interface made of the system's widgets rather
than a web page, the way Native Shell (github.com/longbridge/nsh) does it,
or Native Shell itself as a library inside zebkit.

**Native Shell, read (version 0.1.1, 2026-10-20).** A Rust engine:
TSX compiled ahead of time by its CLI (rolldown, Oxc and its own JSX
compiler, Solid-style: components run once, signals update the nodes they
feed, no virtual DOM, not React), run on QuickJS, one interpreter per
view. A shared core holds the reactive graph, the layout (Taffy: flexbox
and grid), Tailwind v4 classes parsed in Rust, and a diff of mount
operations that a backend per platform applies to the system's toolkit:
GTK 4 and Pango on Linux, AppKit, Win32 with Direct2D, UIKit, Android
views, or GPUI. Apache-2.0, with extensive tests and documentation.

**Not as a library.** Four things stand against linking it into zebkit:

- **A second JavaScript engine.** Its runtime layer, 49,000 lines, calls
  QuickJS's C API directly, with no interface to put V8 behind; its own
  documentation puts QuickJS at 10 to 50 times slower than V8.
- **GTK 4 on Linux.** The backend is GTK 4 and Pango and owns the GTK main
  loop; zebkit has no GTK, and Chromium owns the loop. Its GPUI backend
  brings its own windowing and Vulkan through wgpu instead.
- **Duplicates of what zebkit exposes.** Its own HTTP and TLS (ureq,
  rustls), cryptography, zlib, image decoding and a Tokio runtime.
- **No C API on the desktop.** It embeds through Rust there; only iOS has
  a C ABI.

**zebkit's own, on what the engine has.** The same idea, built from the
engine's parts:

| Native Shell | zebkit |
|---|---|
| QuickJS, one interpreter per view | V8, outside any page (N6) |
| GTK 4, AppKit, Win32 backends | Chromium's `ui/views`: the toolkit Chrome draws its own interface with on Linux, Windows and macOS, already in the binary |
| Taffy | `views::FlexLayout` (flexbox) first; Taffy, a Rust crate, if grid is wanted |
| Pango, Core Text, DirectWrite | `gfx::RenderText` (HarfBuzz) |
| Its modules for files, network, crypto, zlib | `zebkit.*`: layers 1 and 2 |
| Signals, `View`, `Text`, `Input` | The same model: what Native Shell shows works, and its design can be followed where it fits; its code is Apache-2.0 |

One toolkit for three systems, as Chrome has it, instead of a backend per
system; a web view is a widget among the others, since the engine is
there. What does not carry over is the look of each system's own widgets:
`ui/views` draws its own, as Chrome's interface does.

The parts, in order:

1. **N6, script outside a page:** a V8 isolate in the application's
   process with layers 0 to 2 as `zebkit.*`, ES modules from files, and
   `zebkit-js app.js` to run one. An application then is JavaScript, as
   with Electron's main process, without Node.
2. **N7, `zebkit.ui`:** windows and widgets of `ui/views` from that script
   (view, text, button, input, image, scroll, web view), flexbox layout,
   events.
3. **Signals and JSX:** signals that update widgets in place, and a JSX
   runtime over `zebkit.ui`; TSX compiled ahead of time by an existing
   compiler (esbuild or Oxc).
4. **Measured against a web page:** startup, memory and frame time of the
   same interface written for `zebkit.ui` and as React in a page, before
   it goes further.

## 8. Costs to plan for

- **Builds.** The checkout is about 30 GB without history; a full build of
  the engine takes hours on this machine (24 cores, 94 GB of memory), and a
  change of compiler flags rebuilds everything. Changes are batched into
  one rebuild where possible.
- **Patches.** Chromium releases every four weeks, and every patch has to
  apply to each release. Switches are preferred to patches, and patches are
  kept small; `patches/series` lists them.
- **Security.** Updates come from upstream continually. zebkit has to follow
  stable releases closely, which is one more reason to keep the patch set
  small.

## 9. For the owner to decide

1. Decided 2026-10-19: the limit is 150 MiB, and 134.8 MiB is accepted.
   No lever of section 6, part B, is taken.
2. Decided 2026-10-19: the 150 MiB limit is the size requirement; there is
   no separate bar against Electron.
3. Decided by section 7: a library plus a helper program.
4. Decided by section 7: calls from a page to the application are part of
   layer 4.
