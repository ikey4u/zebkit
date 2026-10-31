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
   lays them out as a copy of Xcode under `.cache/macos/xcode`, linked in
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

## State

`scripts/engine-test.sh slim-macos`, on the Mac against the package built
on Linux, 2026-10-21:

| Test | |
|---|---|
| What the engine exports, what it loads | ok |
| vendor (32), services (25), PDF (83), C API (26) | all passed |
| bridge, schemes (10), script outside a page (15), `zebkit.ui` (11) | all passed |
| Python, CDP from the command line | all passed |
| Off-screen views (30) and a host in another process (13), on 2026-10-23: IOSurfaces read in the process and through mach ports, damage, input, `<select>`, scale 2 | all passed |
| Dialogs, PDF, drag and drop (`page.c`, 101; `cdp_page.py`, 14) and off-screen views again (30), on 2026-10-24, macOS 27.0.1 | all passed |

What macOS needed beyond Linux, found by those tests: the vendor headers'
`#pragma redefine_extname` takes Mach-O's leading underscore; ICU's data
is in the framework's resources, which services used before
`zebkit_main()` set up themselves; `zebkit.ui`'s widgets need content's
compositor from the views delegate; macOS's bash 3.2 takes an empty array
for an unset variable.

## Left

- Real windows: views attached in another's window, a view on an application's
  `NSView` (`zebkit_view_attach_native()`, compiled, never run), files dropped on
  a view, the menu bar's items and keys, with a person at the Mac.
- Signing for distribution (Developer ID) and notarization.
- Intel Macs (x86-64), if they are wanted.
