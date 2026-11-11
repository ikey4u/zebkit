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
		view, err := zebkit.NewView("", 1024, 768)
		if err != nil {
			fmt.Fprintln(os.Stderr, err)
			zebkit.Quit()
			return
		}
		view.Grant("https://example.com", zebkit.AllowApp, "")
		view.Bind("greet", func(call *zebkit.Call) {
			var name string
			if err := call.ArgsInto(&name); err != nil {
				call.Reject(err.Error())
				return
			}
			call.ResolveValue("hello, " + name) // a page's zebkit.app.greet("Go") gets "hello, Go"
		})
		view.OnClose(zebkit.Quit)
		view.Load("https://example.com/")
	}, os.Args[1:]...))
}
```

## The engine

The module is the binding; the engine is the release archive for your
system, of the same version, from the
[releases](https://github.com/ikey4u/zebkit/releases). `zebkit.BindingVersion`
is the version the binding is made for: `Init` and `Main` refuse an engine of
another major or minor version, and warn for another patch level. cgo builds
against the engine's headers and links its library:

```sh
go get github.com/ikey4u/zebkit/bindings/golang@main   # until the binding has release tags
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
`Shutdown` fit it into a loop of your own (`Shutdown` inside a callback is
refused, and the engine runs on). `Post` before the engine has started, or
after it stopped, returns `ErrNotStarted`, and the function is never called.

One rule holds for every function and method that reaches the engine: called
off the engine's thread it panics, rather than run there. The exceptions are
what the C API lets any thread call: `Post` and `PostDelayed`, a `Call`'s
`ResolveValue`, `ResolveJSON` and `Reject`, a scheme `Request`'s `Respond`
and `RespondFile`, and an `OffscreenFrame`'s `Release`; `View.IsClosed` may
be asked from any goroutine. The PDF, canvas, text and services APIs (but
`Fetch`) need no engine thread.

## Conventions

- JSON going in: an argument documented as JSON is a `string`, `[]byte` or
  `json.RawMessage` taken as JSON text as it is, or any other value, encoded
  with `encoding/json`. Where the C API has a default for NULL, `nil` (and
  `""`) gives it.
- JSON coming out: small answers of a fixed shape are decoded (a struct,
  `map[string]any`, `int`, `bool`); open documents are `json.RawMessage`.
- Errors: what the engine can refuse returns an `error` (an `*zebkit.Error`;
  `errors.Is(err, zebkit.ErrRefused)`); constructors return `(value, error)`;
  a `bool` is only a predicate.
- Callbacks: a `nil` function removes the one set before (`Bind(name, nil)`
  unbinds); a one-shot `done` may be `nil`.
- Closing: `Close` everywhere. A view also closes when its window or its page
  closes it, with its context, or with `Quit`: there is one `*View` per view,
  `OnClose` is told, and its methods then do nothing or return
  `zebkit.ErrClosed`. An `OffscreenFrame` must be `Release`d by the
  application; `Pixels` is valid until then, `CopyPixels` keeps them.
- Answering: answer a scheme `Request` and a page's `Call` on every path,
  and `End` a `NetworkStream`. As Rust does on drop and Python on
  collection, a `Request` let go unanswered is answered 404, and a stream let
  go open is ended as failed, but only once the garbage collector finds them
  (late, or never); a `Call` never answered stays in the engine's memory, and
  a hook's request left unanswered ends at its deadline.

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
- Playwright's API inside the engine (`Page`, `Locator`, `CallAutomation`,
  `CallAutomationPixels`, `ListenAutomation`, `NewBrowserContext`).
- PDF (`OpenPDF`, rendering, text, search, annotations, forms, redaction,
  page editing, signatures, and every PDF hook), the canvas and text
  shaping, media capture, taps and tools (`FFmpeg`, `FFprobe`,
  `MediaProbe`, and `Remux`, `ExtractAudio`, `ExtractVideo` with their
  `...With` forms taking streams, formats and progress), and the services
  (compression, digests, encryption, images, URLs, `Fetch`, whose
  `FetchResponse.HeaderList` keeps repeated headers).
- The desktop and the system: menus, the Dock and the taskbar, the tray,
  notifications, opening URLs and folders, displays, power events, keeping
  awake.

## Systems

Linux x86-64, macOS on Apple silicon and Windows x64. The checks in
`internal/` pass on all three, on macOS and Windows with the sandbox on.

### Windows

Chromium's sandbox on Windows needs the program that starts the engine's
children to be those children too (`docs/WINDOWS.md`), so the program links
`zebkit_sandbox.lib`. The package does the rest: on Windows its `init()`
calls `zebkit_win_startup()`, so a child runs the engine's part there and
exits before `main.main`. That is not first in the process, as the C API asks:
the Go runtime has started, and the `init` functions of packages initialized
before zebkit's have run in every child too. Keep those free of side effects,
or do that work in packages that import zebkit, or in `main`.

The engine and that library are built with clang for Microsoft's C runtime,
not MinGW, so a Go program for Windows is cross-built, from Linux or macOS,
with `cmd/zebkit-cc` as cgo's C compiler: clang for
the Windows target of `GOARCH`, linking with `lld-link`, without the GNU ld
options and MinGW libraries Go's linker asks for. When it links, it adds the
8 MiB stack (`chrome.exe`'s, which the renderer the program also is needs)
and, with `ZEBKIT_DIR` set, the engine's `zebkit.dll.lib` and
`zebkit_sandbox.lib`. It needs LLVM's clang and lld (Chromium's own build has
them) and the Windows SDK laid out as Chromium's `--winsysroot` takes it
(`scripts/windows-toolchain.sh`):

```sh
go install github.com/ikey4u/zebkit/bindings/golang/cmd/zebkit-cc@main
GOOS=windows GOARCH=amd64 CGO_ENABLED=1 CC=zebkit-cc ZEBKIT_CLANG=/path/to/clang \
ZEBKIT_SYSROOT=/path/to/vs_files ZEBKIT_DIR=/path/to/zebkit-<version>-windows-x64 \
CGO_CFLAGS="-I$ZEBKIT_DIR/include" go build -ldflags="-s -w" .
```

Put the program beside `zebkit.dll` (the engine's children find it there).
This is how the checks below are built for Windows; building on Windows
itself is not tested.

## Checks

```sh
go run ./internal/check --headless              # pages, calls, schemes, hooks, the system
go run ./internal/check_hooks --headless        # every hook, settings, contexts, CDP, scripts
go run ./internal/check_pdf --resources=DIR --headless   # PDF, canvas, text, services
ZEBKIT_MEDIA_DIR=DIR go run ./internal/check_automation --headless   # Playwright's API, off-screen views, page, media
```

run the binding against a real engine (add `--no-sandbox` where the
sandbox is not installed, and `--user-data-dir=` a fresh directory to keep
runs apart). `check_pdf`'s `--resources` is PDFium's `testing/resources`
(found by itself in a checkout with Chromium's source), and
`check_automation`'s `ZEBKIT_MEDIA_DIR` is the checkout's
`tests/engine/pages/media` (the default, from `bindings/golang`).
`scripts/engine-test.sh` runs it on Linux, and
builds it into the macOS and Windows test bundles (`--build-only`), where it
runs from the package.

The binding is under the LGPL 2.1 or later, as the rest of zebkit (`LICENSE`).
