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

- The engine: `Main`, `Init`, `Pump`, `Shutdown`, `Quit`, `Post`,
  `PostDelayed`, `Version`, `EngineVersion`, `IsHeadless`, `Environment`,
  `OnEnvironmentChanged`, `Capabilities`.
- Views: `NewView`, `NewViewWithOptions` (the window's frame and options),
  load, reload, back and forward, resize, focus, close, URL and title,
  `Evaluate`, `AddScript`, `Capture`, `OnLoad`, `OnTitle`, `OnClose`,
  `Window`, `SetWindow`, `WindowAction`, `OnWindowState`.
- Pages calling the application: `Grant`, `Bind` and `Call`.
- The application's URL schemes: `SchemeRegister`, `SchemeHandle`, `Request`.
- Hooks: `SetPermission`, `OnPermission`, `OnNavigation`, `OnMediaAccess`
  (camera, microphone, and `getDisplayMedia()`'s source), `OnEyeDropper`,
  `OnShare`, `OnDeviceChooser` (WebUSB, WebHID, Web Serial), `OnContextMenu`.
- The desktop and the system: `AppSetMenu`, `AppUpdateMenuItem`, `AppMenu`,
  `AppSetBadge`, `AppSetProgress`, `AppRequestAttention`, `AppSetDockMenu`,
  `NewTray`, `OnNotification`, `NotificationClicked`, `NotificationClosed`,
  `OpenExternal`, `ShowInFolder`, `Displays`, `OnDisplaysChanged`,
  `OnPowerEvent`, `PreventSleep`, `AllowSleep`.

Not yet in Go, and in C, Rust and Python: the PDF API, Playwright's API
(`automation.h`), off-screen views, the canvas and text APIs, media capture
and the other hooks of `<zebkit/hooks.h>` (network, downloads,
certificates, HTTP authentication, file choosers). The C API is the
reference for all of it (`docs/API.md`).

## Systems

Linux x86-64 and macOS on Apple silicon, where `internal/check` passes
against the release engine (on macOS with the sandbox on). Windows is not
supported yet: Go's cgo builds with a MinGW toolchain, and the engine's
Windows sandbox needs the program linked with `zebkit_sandbox.lib`, built
with Microsoft's C runtime.

## Checks

```sh
go run ./internal/check --headless
```

runs the binding against a real engine: pages, calls, schemes, the hooks
and the system's services. `scripts/engine-test.sh` runs it on Linux.

The binding is under the LGPL 2.1 or later, as the rest of zebkit (`LICENSE`).
