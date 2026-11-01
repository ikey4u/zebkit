# macOS

zebkit for macOS: `Zebkit.framework`, for Apple silicon (arm64), with the
same functions as on Linux. Release packages are cross-built on Linux;
development builds can also be built on an Apple silicon Mac.

## Build on this Mac

Install full Xcode in `/Applications/Xcode.app`, then run:

```sh
scripts/dist.sh release
scripts/example.sh macos-basic https://example.com/
```

This fetches the pinned Chromium source and macOS dependencies, applies
the patch series, builds `slim-macos-native`, and packages it in
`dist/release`. The example builds and locally signs `dist/example/Zebkit Embedded Browser.app`
with that engine. `scripts/dist.sh debug` uses `slim-macos-native-dev` and
writes `dist/debug`; `EXAMPLE_DIST=debug` selects it for the example.
`ZEBKIT_JOBS` overrides the compiler parallelism, which defaults to at most
one job per two GiB of RAM and at most the number of logical CPUs.

Native builds use the installed Xcode through `DEVELOPER_DIR`, leaving
the system's `xcode-select` setting alone. They disable the Linux-only ML
inliner and ThinLTO for local iteration, keep Chromium's process sandbox,
and do not need the SDK archive or `macos-toolchain.sh` below. Allow about
120 GB for the checkout and build; the first build can take hours.

## The package

Passkey authentication also needs host configuration: the phone route
requires a Bluetooth privacy description, and Apple's iCloud browser route
requires an approved entitlement on the signed host application. The C
example supplies the privacy description and supports an optional signing
identity, entitlements file and provisioning profile. See
[passkey support](PASSKEYS.md).

`scripts/package.sh slim-macos` makes `out/blink/slim-macos/package`:

```text
Zebkit.framework/
    Zebkit -> Versions/Current/Zebkit
    Versions/A/
        Zebkit                  the engine: Chromium, PDFium, FFmpeg, ...
        Resources/              ICU's data, V8's snapshot, content's .pak
        Helpers/Zebkit Helper.app
                                renderer, GPU and utility processes
include/zebkit/                 zebkit.h, services.h, pdf.h, vendor/
```

An application links it with `-F<dir> -framework Zebkit -Wl,-rpath,<dir>`
(or the Rust crate's `zebkit-build`, which puts it beside the executable)
and calls `zebkit_main()` as on Linux. The framework loads only the
system's frameworks and libraries; `scripts/engine-test.sh` checks that.

| | Size |
|---|---|
| The package | **150.1 MiB** (40.8 MiB as `.tar.xz`) |
| `Zebkit` | 133.3 MiB |
| ICU's data | 6.5 MiB |
| Headers | 8.4 MiB |

Measured on 2026-10-31. `slim-macos` has `slim`'s `-O2` directories and
link-time optimization, but not PGO, whose profile for macOS is not fetched yet
(`docs/SIZE_AND_SPEED.md`).

## Built on Linux

The macOS engine is cross-compiled on the Linux machine, as Chromium's own
builders compile for macOS on Linux: Chromium's clang and `ld64.lld`
target macOS; what only a Mac has is Apple's SDK.

1. On a Mac, pack the SDK and Xcode's version file:

   ```sh
   cd "$(xcrun --show-sdk-path)/.." &&
       tar -cf - --exclude 'MacOSX.sdk/usr/share/man' MacOSX.sdk | xz -T0 > MacOSX.sdk.tar.xz
   cp "$(xcode-select -p)/../version.plist" .
   ```

2. On Linux, `scripts/macos-toolchain.sh MacOSX.sdk.tar.xz version.plist`
   lays them out as a copy of Xcode under `.cache/sdk/macos/xcode` (`ZEBKIT_SDK_DIR` to put it elsewhere, `scripts/zsdk-create.sh`), linked in
   as `build/mac_files/xcode_binaries` (Chromium's hermetic Xcode), and
   fetches what DEPS pins for macOS: `mig` for Linux, clang's macOS
   runtime, ANGLE's Metal shader tools.
3. `scripts/build.sh slim-macos`, then `scripts/package.sh slim-macos`.

`profiles/slim-macos.gn` is `slim` with:

| Argument | Why |
|---|---|
| `target_os = "mac"`, `target_cpu = "arm64"` | Apple silicon |
| `use_system_xcode = false`, `mac_sdk_official_version` | the SDK from step 2 |
| `enable_dsyms = false` | no debug information to put in a dSYM |
| `v8_use_metagen_instance_types = false` | metagen reads the build graph without zebkit's root pattern |
| `use_relative_vtables_abi = false` | relative vtables need COMDATs, which Mach-O has not |
| `enable_swiftshader = false` | see below |
| `use_cups = true` | `//printing`'s macOS print context needs CUPS, the system's `libcups.2.dylib`, for printing to PDF |

What upstream does not build, and patches make work: Chrome always has
Dawn on macOS and the content shell's test support; the slim profile has
neither. `patches/0013` puts the IOSurface backing's WebGPU parts under
`USE_DAWN`, as other platforms' backings are; `patches/0014` gives content
the system's location permission manager (zebkit sets it, as Chrome does)
in place of the tests' fake, and leaves out the display test support,
which brought Perfetto's test library as a dylib of its own.

The Linux release builder has 24 cores and 94 GB, which helps with
ThinLTO's memory use. Native builds leave ThinLTO off to make iteration
practical. What only a Mac checks is signing with real certificates;
ad hoc signing does for tests.

## Decisions

- **No SwiftShader.** Chromium's software Vulkan renders when the GPU
  cannot be used. Every Mac has a Metal GPU, which ANGLE draws with; on
  Apple silicon SwiftShader is built on LLVM rather than its small JIT,
  13.6 MiB with the Vulkan loader. No web feature goes with it.
- **Headless without windows.** `--headless` views are laid out and drawn
  at the size they are given, with no `NSWindow` (Chromium's headless way
  on macOS); `zebkit_view_attach()` then sizes the view.
- **The application.** zebkit is the application's delegate: files from
  the Finder and the Dock, the Dock's reopen and quit reach
  `zebkit_app_on_*`; `zebkit_app_set_menu()` is the menu bar, with the
  application's own application menu if it gives one.
- **The helper** enters the seatbelt sandbox itself, as Chrome's helpers
  do; `--no-sandbox` turns it off.

## Off-screen views and IOSurfaces

An off-screen view (`<zebkit/offscreen.h>`, `docs/API.md`) is, on macOS, a
headless view: its `NSView` is in no window, and its compositor draws for
itself. Its frames are IOSurfaces. viz's `FrameSinkVideoCapturer` reads
each frame back into shared memory in the GPU process, and the engine
copies what changed since a surface last held a frame into an IOSurface
of a pool of its own (as many as the frames out, and one), so nothing is
encoded and an idle page copies nothing. viz can blit into IOSurfaces
itself (`kPreferMappableSharedImage`, `ZEBKIT_BUFFERS_GPU`), which would
save the read-back; for a page in no window it delivers no frame at all on
the Mac it was tried on (macOS 27, an M-series laptop), and a window, even
an unseen one, is what macOS counts as occluded behind others and stops
drawing. Making viz's own path work headless is on the roadmap.

- **In the process.** `frame->iosurface` is the `IOSurfaceRef`, the
  engine's until the frame is released. A host wraps it in a
  `CVPixelBuffer` (`CVPixelBufferCreateWithIOSurface()`, once per
  `buffer_id`: a pool is a few surfaces, used again) and draws that, as
  gpui's `paint_surface` does, or makes a Metal texture of it
  (`newTextureWithDescriptor:iosurface:plane:`, BGRA8). It releases the
  frame once its GPU work that reads the surface is done.
- **In another process.** `zebkit_frame_iosurface_mach_port()` makes a
  send right to the surface (`IOSurfaceCreateMachPort()`), which goes in a
  mach message; the other process's `IOSurfaceLookupFromMachPort()` gives
  it the same surface. A global IOSurface ID (`kIOSurfaceIsGlobal`) is not
  used: it is deprecated, and any process could look the surface up.
  `examples/offscreen-host` registers the host's port with launchd
  (`bootstrap_check_in()` under a name with its pid), the renderer looks
  it up (`bootstrap_look_up()`), and sends each buffer's right once,
  before the first frame that names it; frames, input and releases go over
  a Unix socket.
- **`<select>`.** A `<select>`'s list is the system's menu in a window of
  Chrome's, which an off-screen view has no place for: its views forbid
  external popup menus, so the list is a popup widget drawn by the page's
  renderer, like a date picker's. That widget has a borderless window of
  its own, made transparent and blind to the mouse so that it is not seen
  on the screen while it draws the popup layer's frames.
- **The scale** is the headless screen's, which zebkit sets for its
  off-screen views (`zebkit_view_set_size()`): whole, 1, 2 or 3, as a
  Mac's screens are; a fractional one is not taken by a headless `NSView`.

## Testing on a Mac without building there

The test programs are cross-compiled like the engine, signed ad hoc by
`ld64.lld`, and need neither a compiler nor Chromium's source on the Mac:

```sh
scripts/engine-test.sh slim-macos --build-only DIR    # on Linux; C, and Rust if the target is installed
tar -C DIR -cJf bundle.tar.xz .                       # copy it to the Mac, unpack it as DIR there
DIR/scripts/engine-test.sh slim-macos --prebuilt DIR/programs [--only REGEX]
```

On a Mac the suite leaves the person's files alone: `HOME`, `CFFIXED_USER_HOME`,
`TMPDIR` and `MAC_CHROMIUM_TMPDIR` are its own, and the Keychain calls are answered
from files (`tests/engine/macos_private_keychain.c`), since every persistent
profile otherwise adds an item to the login Keychain under the program's name.
`ENGINE_TEST_REAL_KEYCHAIN=1` uses the real one. What macOS writes itself is not
redirected: a test program that crashes leaves a report in
`~/Library/Logs/DiagnosticReports`. `ENGINE_TEST_WINDOWS=1` adds the tests with
windows, which take no keyboard or pointer from the person at the Mac; their pixel
checks need the display awake and Screen Recording for what started them, and say
`skip` without.

## State

`scripts/engine-test.sh slim-macos --prebuilt`, the package and programs built on
Linux, on an Apple M4 with macOS 27.0.1, 2026-10-31: every test passes, except what
is listed as not run below. The last full run had 123 tests and about 2,400 checks;
one test, `media-tap`, needed one correction to its own skip message and was then run
again alone.

| Test | Checks |
|---|---|
| Exports, loads (at `--build-only`, with `llvm-nm`, `llvm-otool`) | ok: 7,817 names |
| Vendor libraries with LevelDB and FreeType (48), C++, services (25) | passed |
| PDF (99), annotations (407), hooks (286), bindings (63), plugin (92), outline; canvas and text (57) | passed |
| C API (28), bridge (4), schemes (11), script outside a page (22), `zebkit.ui` (11), Python, CDP | passed |
| `zebkit_init`/`pump`/`shutdown`: lifecycle, host loop, late load on the main thread, startup settings (C, Python) | passed |
| Off-screen views (30), another process (13), per-view scale, patch 1053 (23), native editing commands (17) | passed |
| Hooks in C, JavaScript, Python and Rust (all groups), downloads saved by default among them | passed |
| Media capture (123, history 9), resolution change, wall clock, tools | passed |
| Permissions, navigation, user agents, certificates, HTTP authentication, network quality | passed |
| Dialogs and drag and drop (101), automation (172, Python 31), CDP (27, 31), popups (12) | passed |
| GPU in headless and with a window (`tests/engine/gpu_status.py`) | hardware compositing, raster and WebGL, ANGLE on Metal |
| A view on an `NSView` (`tests/engine/native_parent_mac.m`) | 43 of 43 with pixels, hardware compositing |
| AppKit host (`examples/native-host/appkit.mm`) | the page's and the PDF's pixels; clean shutdown |

What those tests found, now fixed:

- **Every `zebkit_init()` crashed.** Content's responsiveness watcher sends a message
  a plain `NSApplication` does not have (`patches/1060`).
- **Every page was composited in software.** On Apple silicon Chromium's only hardware
  mode is Graphite on Dawn, which `slim` leaves out; only WebGL used the GPU, which hid
  it. `patches/1061` allows Skia's GL back end on ANGLE's Metal, Chromium's mode for
  Intel Macs, for no size: `chrome://gpu`'s features are now as in Chrome.
- **Media tap crashed the renderer** on frames on the GPU; it reads them back now.
- **The AppKit host showed no page and turned black text transparent:** it ignored
  IOSurface frames, and took a bitmap image representation's bytes as B, G, R, A.

Not run, or not fully:

- **The real Keychain:** `tests/engine/cookies.py` and the off-screen and passkey tests
  ran on the private store above. Passkeys' system interface was not shown.
- **Real input:** events reach the views as AppKit hands them on, not from the system's
  event source. Clicking into a window of another application, a `<select>`'s system
  menu on an attached view and AppKit's input methods need a person.
- **A real camera or microphone**, which need a person's permission. With fake devices
  a microphone's `getUserMedia()` never settles on this Mac, in Chrome 156 as in zebkit
  (cause not found), so `permission_hooks.py` grants through the camera there.
- **WebRTC between two peers:** `media_tap.py` says `skip` on a Mac where they do not
  connect. Here they do not in zebkit (mDNS names unresolved, then checks unanswered)
  and do in Chrome 156. Local Network privacy is suspected, for a program with no
  bundle and no grant; it is not confirmed, and may be a real gap.
- **The Playwright client** (`playwright_connect.mjs`): playwright-core was not on the Mac.
- **The sandbox:** nearly all of the suite runs with `--no-sandbox`.

Measured with the fixes: `Zebkit` and the package are the same size to the byte
(139,733,664 and 157,340,514 bytes). Page speed on the GPU was not measured.

## Left

- Real windows with a person at the Mac: files dropped on a view, the menu bar's items
  and keys, and what is under "Not run" above.
- Signing for distribution (Developer ID) and notarization.
- Intel Macs (x86-64), if they are wanted.
