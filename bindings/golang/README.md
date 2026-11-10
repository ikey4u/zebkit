# zebkit for Go

Chromium's engine as a library, from Go, over the C API through cgo: web
views in windows, pages that call Go functions, the application's own URL
schemes, the hooks for what pages ask, and the system's services.

```go
package main

import (
	"fmt"
	"os"

	zebkit "github.com/ikey4u/zebkit/bindings/golang"
)

func main() {
	os.Exit(zebkit.Main(func() {
		view := zebkit.NewView("", 1024, 768)
		view.Grant("https://example.com", zebkit.AllowApp, "")
		view.Bind("greet", func(call *zebkit.Call) {
			call.Resolve(fmt.Sprintf("\"hello, %s\"", call.Args))
		})
		view.OnClose(zebkit.Quit)
		view.Load("https://example.com/")
	}, os.Args[1:]...))
}
```

## The engine

The module is the binding; the engine is the release archive for your
system, of the same version, from the
[releases](https://github.com/ikey4u/zebkit/releases). cgo builds against its
headers and links its library:

```sh
go get github.com/ikey4u/zebkit/bindings/golang@main
export ZEBKIT_DIR=/path/to/zebkit-<version>-<target>
export CGO_CFLAGS="-I$ZEBKIT_DIR/include"
export CGO_LDFLAGS="-L$ZEBKIT_DIR -Wl,-rpath,$ZEBKIT_DIR"   # Linux
export CGO_LDFLAGS="-F$ZEBKIT_DIR -Wl,-rpath,$ZEBKIT_DIR"   # macOS
go run .
```

The run path lets the program find the engine where it was built; to ship,
put the engine beside the program and use `-Wl,-rpath,$ORIGIN` (Linux) or
`-Wl,-rpath,@executable_path` (macOS) instead.

## The engine's thread

The engine runs on the program's main thread, which the package locks to the
main goroutine. Call it from the main goroutine and from the functions the
engine calls back; from other goroutines, hand work over with
`zebkit.Post(func() {...})`. `Main` runs the engine's loop; `Init`, `Pump` and
`Shutdown` fit it into a loop of your own.

## What it covers

All of the C API (`docs/API.md`), as Rust and Python have it;
`scripts/check-bindings.py` checks every exported function against each
binding:

- The engine: `Main`, or `Init`, `Pump` and `Shutdown` in a loop of your own;
  `Post`, startup settings, cookie storage, contexts (`NewContext`), CDP, and
  scripts outside any page (`NewScript`).
- Views: windows with options, native windows and attaching, loading,
  `Evaluate`, `Capture`, settings, user agents, popups and new windows,
  files dropped, drag regions; off-screen views (`NewOffscreenView`), with
  their frames as shared memory, dmabuf planes, IOSurfaces or Windows handles,
  and the input sent to them.
- Pages calling the application (`Grant`, `Bind`), the application's URL
  schemes, and every hook of `<zebkit/hooks.h>`: permissions, navigation,
  certificates and client certificates, HTTP authentication, the network at
  every stage, downloads, file choosers, media access and screen capture, the
  eye dropper, sharing, device choosers, the context menu, reserved keys and
  fullscreen.
- `<zebkit/page.h>`: dialogs, printing to PDF, closing, drag and drop.
- Playwright's API inside the engine (`Page`, `Locator`,
  `AutomationCall`, `AutomationListen`).
- PDF (`OpenPDF`, rendering, text, search, annotations, forms, redaction,
  page editing, signatures, and every PDF hook), the canvas and text
  shaping, media capture, taps and tools, and the services (compression,
  digests, encryption, images, URLs, `Fetch`).
- The desktop and the system: menus, the Dock and the taskbar, the tray,
  notifications, opening URLs and folders, displays, power events, keeping
  awake.

## Systems

Linux x86-64, macOS on Apple silicon and Windows x64. The checks in
`internal/` (257 of them) pass on all three, on macOS and Windows with the
sandbox on.

### Windows

Chromium's sandbox on Windows needs the program that starts the engine's
children to be those children too (`docs/WINDOWS.md`), so the program links
`zebkit_sandbox.lib`. The package does the rest: on Windows its `init()`
calls `zebkit_win_startup()`, so a child runs the engine's part there and
exits before the program's own code.

The engine and that library are built with clang for Microsoft's C runtime,
not MinGW, so a Go program for Windows is cross-built, from Linux or macOS,
with `cmd/zebkit-cc` as cgo's C compiler: clang for
`x86_64-pc-windows-msvc`, linking with `lld-link`, without the flags only
MinGW knows. It needs LLVM's clang and lld (Chromium's own build has them) and
the Windows SDK laid out as Chromium's `--winsysroot` takes it
(`scripts/windows-toolchain.sh`):

```sh
go install github.com/ikey4u/zebkit/bindings/golang/cmd/zebkit-cc@main
GOOS=windows GOARCH=amd64 CGO_ENABLED=1 CC=zebkit-cc ZEBKIT_CLANG=/path/to/clang \
ZEBKIT_CC_FLAGS="-Xmicrosoft-windows-sys-root /path/to/vs_files" \
CGO_CFLAGS="-I$ZEBKIT_DIR/include" \
CGO_LDFLAGS="$ZEBKIT_DIR/zebkit.dll.lib $ZEBKIT_DIR/zebkit_sandbox.lib -Wl,/STACK:0x800000" \
go build -ldflags="-s -w" .
```

Put the program beside `zebkit.dll` (the engine's children find it there).
The 8 MiB stack is `chrome.exe`'s, which the renderer it also is needs. This
is how the checks below are built for Windows; building on Windows itself is
not tested.

## Checks

```sh
go run ./internal/check --headless              # pages, calls, schemes, hooks, the system
go run ./internal/check_hooks --headless        # every hook, settings, contexts, CDP, scripts
go run ./internal/check_pdf --headless          # PDF, canvas, text, services
go run ./internal/check_automation --headless   # Playwright's API, off-screen views, page, media
```

run the binding against a real engine. `scripts/engine-test.sh` runs it on Linux, and
builds it into the macOS and Windows test bundles (`--build-only`), where it
runs from the package.

The binding is under the LGPL 2.1 or later, as the rest of zebkit (`LICENSE`).
