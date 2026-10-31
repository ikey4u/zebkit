# A window and event loop owned by the application

These applications put a web view and a PDF view in their own window.
GTK, Qt, AppKit or winit owns the window, menus and native event loop.
The engine starts headless and delivers off-screen frames; it creates no
browser window. The PDF pane uses PDFium directly. No web server or page
bridge is required.

The wakeup callback only queues a toolkit event with an absolute deadline.
The toolkit preserves the earliest deadline, consumes it before pumping,
and calls `zebkit_pump()` on the initializing thread. Idle hosts wait for
an event or a real deadline. Startup, normal pumping and shutdown are
separate. The C++ examples share `model.h`; the Rust example uses the
existing `zebkit` and `zebkit-build` crates.

## X11: the engine's window inside the application's

`x11.c` is the other way, in about fifty lines of Xlib: the application gives
its window to `zebkit_view_attach_native()` and the engine draws and takes
input in a child window of it, with no frames to handle. Only X11 and macOS
(`NSView*`) can do this, and it cannot be mixed with off-screen views; on
Wayland it fails, naming off-screen views, which is why the toolkit examples
below use them.

```sh
cc -Iout/blink/slim/package/include examples/native-host/x11.c \
    -Lout/blink/slim/package -l:libzebkit.so.1 \
    -Wl,-rpath,$PWD/out/blink/slim/package -lX11 -o /tmp/x11-host
/tmp/x11-host --no-sandbox https://example.com/
```

## Linux: GTK 3 and Qt 6

From the repository root, after building and packaging the slim engine:

```sh
python3 examples/native-host/build.py gtk
out/blink/examples/gtk-host
python3 examples/native-host/build.py qt
out/blink/examples/qt-host
```

Install the toolkit's development packages first (`libgtk-3-dev` or
`qt6-base-dev` on Debian). `--package /absolute/package` selects another
engine; `--output /path/program` selects the executable location.
`--toolkit-root /path/root` supports development packages extracted to a
local root instead of installed system-wide. The caller supplies its
libraries and any development symlinks needed by that root.

Both applications forward mouse, wheel, focus, raw keys, input-method
composition/commit and caret coordinates. They draw popup-widget layers
and honor page cursor changes. The PDF buttons turn pages.

## macOS: AppKit

On the Mac, build an engine package containing the embedded lifecycle and
native editing-command APIs, then:

```sh
ZEBKIT_DIR=/absolute/package scripts/example.sh macos-appkit
# Or build the signed app without launching:
EXAMPLE_BUILD_ONLY=1 ZEBKIT_DIR=/absolute/package scripts/example.sh macos-appkit
```

This creates `dist/example/Zebkit AppKit Host.app` using the same framework,
helper and inside-out signing path as the existing C browser example.
`EXAMPLE_CODESIGN_IDENTITY` and the existing optional entitlements/profile
variables have the same meaning as for `macos-basic`.

The host installs its own `NSApplicationDelegate` and menu before starting
the engine in `applicationDidFinishLaunching`. It uses `NSTextInputClient`
for marked text, insertion and candidate-window coordinates. AppKit's
`doCommandBySelector` results are collected and sent with one native key
event using `zebkit_view_send_key_with_commands`, preserving the webpage's
ability to cancel that event. Wheel over the PDF pane to turn pages.

The AppKit source cross-compiles for Apple silicon with the repository's
SDK. Its application and input behavior still require a Mac runtime test.

## Rust: winit

```sh
ZEBKIT_DIR=/absolute/package cargo run --manifest-path examples/native-host/winit/Cargo.toml -- --headless
```

The standalone example workspace locks its winit and softbuffer dependencies;
they are not dependencies of the core `zebkit` crate. `zebkit-build` bundles
the engine beside the executable. Winit owns the window and sends wakeup
notifications through `EventLoopProxy`; `ControlFlow::WaitUntil` handles
engine deadlines. Softbuffer presents the web and PDF panes in one surface.
The window handles focus, keyboard, mouse, wheel, IME, caret position, popup
layers and cursor changes. Wheel over the PDF pane to turn pages.

## Options and validation

All four examples accept `--url=URL` and `--pdf=/absolute/document.pdf`.
Without them, a local interactive page and a small sample PDF are shown.
Pass ordinary Chromium switches as needed. On a container without user
namespaces, add `--no-sandbox`; the examples do not disable the sandbox by
default. `--smoke-test=/tmp/window.png` captures both panes and shuts down.

The Linux examples have been built and run with their own toolkit loops;
GTK, Qt and winit screenshots contain the webpage and rendered PDF. The
optional X11 regression sends native XTest events to a private Xvfb server
and uses CDP only to observe the webpage result:

```sh
python3 tests/engine/native_host_input.py out/blink/examples/gtk-host --no-sandbox --disable-gpu
python3 tests/engine/native_host_input.py out/blink/examples/qt-host --no-sandbox --disable-gpu
python3 tests/engine/native_host_input.py examples/native-host/winit/target/debug/zebkit-winit-host --no-sandbox --disable-gpu
```

It checks focus, typing, backspace, button clicks and a select popup. It
requires Xvfb and X11/XTest runtime libraries. Complex language input-method
behavior still needs testing with the host's actual input methods.

These examples use CPU-readable shared-memory web frames, not a zero-copy
GPU presentation path. Mixed engine-window/off-screen views and independent
per-view scales remain separate engine work in the roadmap.
