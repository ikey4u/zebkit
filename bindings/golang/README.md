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
go get github.com/ikey4u/zebkit/bindings/golang@v<version>   # the engine's version
export ZEBKIT_DIR=/path/to/zebkit-<version>-<target>
export CGO_CFLAGS="-I$ZEBKIT_DIR/include"
export CGO_LDFLAGS="-L$ZEBKIT_DIR -Wl,-rpath,$ZEBKIT_DIR"   # Linux
export CGO_LDFLAGS="-F$ZEBKIT_DIR -Wl,-rpath,$ZEBKIT_DIR"   # macOS
go run .
```

Each release tags the module `bindings/golang/v<version>` at the release's
commit, which is what `@v<version>` fetches (releases before 0.5.0 have no such
tag). The run path lets the program find the engine where it was built; to ship,
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
`ResolveValue`, `ResolveJSON`, `Reject` and `Close`, a scheme `Request`'s `Respond`
and `RespondFile`, and an `OffscreenFrame`'s `Release`; `View.IsClosed` and
`IsEngineThread` may be asked from any goroutine. The PDF, canvas, text and services APIs (but
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
  collection, a `Request` let go unanswered is answered 404, a `Call` let go
  unanswered is dropped (its promise rejected: "the application dropped the
  call"; `Call.Close` does it at once), and a stream let go open is ended as
  failed, but only once the garbage collector finds them (late, or never); a
  hook's request left unanswered ends at its deadline, and its outcome is
  told with reason `timeout` (`OnDownloadResult` and the other `...Result`
  functions).
- Asynchronous forms: `ListenCDPAsync`, `OpenExternalAsync` and
  `ShowInFolderAsync` do not wait on the engine's thread; their `done` is
  called once, on that thread, after they return (with the error where they
  fail). `OpenExternal` and `ShowInFolder` return once the system is asked.

## What it covers

All of the C API (`docs/API.md`), as Rust and Python have it;
`scripts/check-bindings.py` checks every exported function against each
binding:

- The engine: `Main`, or `Init`, `Pump` and `Shutdown` in a loop of your own;
  `Post`, startup settings, cookie storage, contexts (`NewContext`), CDP
  (`ListenCDP`, `ListenCDPAsync`), and scripts outside any page
  (`NewScript`).
- Views: windows with options, native windows and attaching, loading,
  `Evaluate`, `Capture`, settings, user agents, popups and new windows,
  files dropped, drag regions, the page's process gone (`OnProcessGone`) or
  not answering (`OnResponsiveness`); off-screen views (`NewOffscreenView`,
  with the engine's reason when it cannot make one), with
  their frames as shared memory, dmabuf planes, IOSurfaces or Windows handles,
  and the input sent to them.
- Pages calling the application (`Grant`, `Bind`), the application's URL
  schemes, and every hook of `<zebkit/hooks.h>`: permissions, navigation,
  certificates and client certificates, HTTP authentication, the network at
  every stage, downloads (`OnDownload`, its outcome on `OnDownloadResult`,
  their course on `OnDownloadProgress`), file choosers, media access and screen capture, the
  eye dropper, sharing, device choosers, the context menu, reserved keys and
  fullscreen.
- `<zebkit/page.h>`: dialogs, printing to PDF, closing, drag and drop, find
  in page (`Find`, `OnFind` and `View.FindResult`, which give a typed
  `FindResult`, `StopFinding`).
- Playwright's API inside the engine: typed, in the `automation` package
  (below), and at its low level `CallAutomation`, `CallAutomationPixels`,
  `View.OnAutomationEvent`, `ViewByPageID` and `ListenAutomation`.
- PDF (`OpenPDF`, rendering, text, search, annotations, forms, redaction,
  page editing, signatures, and every PDF hook), the canvas and text
  shaping, media capture, taps and tools (`FFmpeg`, `FFprobe`,
  `MediaProbe`, and `Remux`, `ExtractAudio`, `ExtractVideo` with their
  `...With` forms taking streams, formats and progress), and the services
  (compression, digests, encryption, images, URLs, `Fetch`, whose
  `FetchResponse.HeaderList` keeps repeated headers).
- The desktop and the system: menus, the Dock and the taskbar, the tray,
  notifications, opening URLs and folders (`OpenExternal`, `ShowInFolder` and
  their `...Async` forms), displays, power events, keeping
  awake.

## Playwright's API

`github.com/ikey4u/zebkit/bindings/golang/automation` is Playwright's API on
a view's page, run inside the engine (`docs/AUTOMATION.md`), as Rust's
`zebkit::automation` and Python's `zebkit.automation` have it, with Go's
names. It keeps the package's rules: the engine's thread, a `done` called
once with the result and an `error` (`automation.IsTimeout(err)` for a
timeout), `done(error)` for a command that answers nothing.

```go
page := automation.NewPage(view) // one Page a view
page.On(automation.EventDialog, func(e *automation.PageEvent) { e.Dialog.Accept(nil) })
page.Route("**/data.json", func(route *automation.Route, _ *automation.Request) {
	route.Fulfill(automation.FulfillOptions{JSON: map[string]string{"from": "route"}}, nil)
}, nil)
page.Goto("https://example.com/", func(response *automation.Response, err error) {
	page.GetByRole("link", automation.RoleOptions{Name: "More information"}).Click(func(err error) {
		automation.Expect(page.Locator("h1")).ToHaveText("Example Domains", func(err error) { zebkit.Quit() })
	})
})
```

- `Page` (and its main `Frame`'s methods): `Goto`, `Reload`, `GoBack`,
  `GoForward`, `Evaluate`, `EvaluateHandle`, `Screenshot`,
  `ScreenshotPixels`, `PDF`, `AddInitScript` (a `Disposable`), `Route`,
  `RouteRegex`, `Unroute`, `ExposeFunction`, `ExposeBinding`, `Frames`,
  `FindFrame`, cookies, permissions, `Keyboard`, `Mouse`, `Touchscreen`,
  `Clock`, `Tracing`, `Request` (an `APIRequestContext`), `Workers`.
- Events: `page.On(kind, func(*PageEvent))` returns a `ListenerID` for
  `RemoveListener`; `WaitForEvent(kind, predicate, done)` waits for the
  next one (and returns its id, to stop waiting); `OnProtocolEvent` sees
  every protocol event. A dialog or file chooser listener holds the page's
  dialogs or choosers for it to answer; with none (or once the last one is
  removed) a dialog is dismissed (a `beforeunload` one accepted) and a file
  chooser cancelled, as Playwright does.
- `Request`, `Response` (with `Body` and `Text`), `Route`, `BindingCall`,
  `Dialog`, `FileChooser`, `Download`, `Worker`, `ConsoleMessage`.
- `Locator`, `FrameLocator` and the `GetBy...` of each, `JSHandle` and
  `ElementHandle`, `NewContext` (a `BrowserContext` with its cookies,
  permissions and pages), and `Expect(locator)` / `ExpectPage(page)`.
- Values for the page (an evaluation's argument, an exposed function's
  answer) are encoded with `encoding/json` (`json.RawMessage` as it is; a
  handle stands for its object); options are the protocol's, JSON objects by
  the package's one rule. Text to match is a `string` or a
  `*regexp.Regexp`.

## Systems

Linux x86-64, macOS on Apple silicon and Windows x64. The checks in
`internal/` pass on all three, on macOS and Windows with the sandbox on.

### Windows

Chromium's sandbox on Windows needs the program that starts the engine's
children to be those children too (`docs/WINDOWS.md`), so the program links
`zebkit_sandbox.lib`. The package does the rest: it calls
`zebkit_win_startup()` from a C initializer (`__attribute__((constructor))`,
the C runtime's `.CRT$XCU`), which the C runtime's startup runs before it calls
Go's `main`: before the Go runtime starts and before any `init`. A child runs
the engine's part there and exits, as a C program's child returns from `main`,
so the children have no Go runtime in them: no Go threads, no `init` of any
package, and none of the Go runtime's exception handlers, the last of which
would end a renderer when V8's WebAssembly trap handler resolves an
out-of-bounds access. Only the C++ static constructors of other cgo code linked
into the program may run before it (and so in every child); the C++ library's
in `zebkit_sandbox.lib` do, before it, as they must.

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
go install github.com/ikey4u/zebkit/bindings/golang/cmd/zebkit-cc@v<version>
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
`tests/engine/pages/media` (the default, from `bindings/golang`); it serves
the pages beside it (`tests/engine/pages`) itself.
`scripts/engine-test.sh` runs it on Linux, and
builds it into the macOS and Windows test bundles (`--build-only`), where it
runs from the package.

The binding is under the LGPL 2.1 or later, as the rest of zebkit (`LICENSE`).
