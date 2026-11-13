<h1 align="center"><img src="docs/images/logo.svg" width="360" alt="zebkit"></h1>

<p align="center"><b>Chrome as a library — light, fast, everywhere, and easy.</b><br>One API on Linux, macOS and Windows. Add it in a few lines.</p>

<p align="center">
<a href="https://crates.io/crates/zebkit"><img src="https://img.shields.io/crates/v/zebkit.svg?label=crates.io" alt="crates.io"></a>
<a href="https://pypi.org/project/zebkit/"><img src="https://img.shields.io/pypi/v/zebkit.svg?label=PyPI" alt="PyPI"></a>
<a href="https://github.com/ikey4u/zebkit/releases/latest"><img src="https://img.shields.io/github/v/release/ikey4u/zebkit?label=engine" alt="Engine release"></a>
</p>

## Introduction

zebkit is Chromium's engine as one library you link into your own
application: Blink, V8 with every JIT tier, PDFium, FFmpeg and the rest of
what Chrome is made of, with nothing of Chrome's browser and nothing of
Electron's runtime. Your application stays yours, in C, C++, Rust, Python,
Go or JavaScript, with its own windows and toolkit. Where it needs the web,
it adds a zebkit view; where it needs a PDF, media, compression or crypto,
it calls zebkit's API: in C, or through the bindings for Rust, Python, Go
and JavaScript (`zebkit-js`, and `zebkit.*` in the application's own pages).

<p align="center"><img src="docs/images/architecture.svg" width="900" alt="The application links one library, zebkit, which runs Chromium's content layer, Blink and V8 inside the application's process; pages run in sandboxed helper processes; on Linux, macOS and Windows"></p>

- **Integrate it, don't build on it.** An application adds zebkit as it
  adds SQLite, and keeps its process, its event loop, its toolkit and its
  windows ([What is in it](#what-is-in-it)).
- **The real Chrome engine.** Pages see Chromium 157, its user agent and
  client hints, and nothing of a debugger attached.
- **Lighter than Electron.** Against the latest Electron, 45.2.1, on Linux
  (2026-11-08, Chromium 157):
  33% smaller as an application ships it, a cold start in 2.3 s against
  5.8, a blank page in 27% less memory, JavaScript and the DOM 6% faster,
  text layout level ([Benchmark](#benchmark)).
- **Take what you need.** Web views in a window or off-screen, a full PDF
  API, media saved as it plays, 2D drawing, find in page as Chrome finds,
  the 17 C libraries inside the engine, your own URL schemes.
- **For agents and tests.** Playwright's API inside the engine, a click in
  2 to 4 ms where Playwright over CDP takes 33 (2026-11-07, Chromium 155);
  CDP; an MCP server.
- **The desktop is the application's.** Every decision the engine would
  take, and everything it would show the person, goes through a hook of
  the application's, with a safe default.

| | |
|---|---|
| Engine | Chromium 157.0.8103.38 (`upstream.lock`): content layer, Blink, V8 with every JIT tier |
| Linux | x86-64, glibc 2.17 or newer (CentOS 7 and later), X11, and Wayland with `--ozone-platform=wayland` (experimental: `docs/ROADMAP.md`): `libzebkit.so.1` and `zebkit-helper` |
| macOS | Apple silicon: `Zebkit.framework`, built natively or cross-built on Linux (`docs/MACOS.md`) |
| Windows | x64, Windows 10 or newer: `zebkit.dll` and `zebkit-helper.exe`, cross-built on Linux (`docs/WINDOWS.md`) |
| Version | `VERSION`: 0.4.0 |

## Quick start

### Get the engine

A package is a directory with the library, the helper, the engine's data
and `include/`. A release has one archive for each system, on
[GitHub's releases](https://github.com/ikey4u/zebkit/releases):

| System | Archive |
|---|---|
| Linux x86-64 | `zebkit-0.4.0-x86_64-unknown-linux-gnu.tar.xz` |
| macOS, Apple silicon | `zebkit-0.4.0-aarch64-apple-darwin.tar.xz` |
| Windows x64 | `zebkit-0.4.0-x86_64-pc-windows-msvc.tar.xz` |

Each archive also holds the tools, built for its system beside the library
and ready to run from where it is unpacked: `zebkit-run`, `zebkit-js` and
`zebkit-mcp` (`.exe` on Windows, sandboxed there too).

To build a package instead (`docs/BUILD.md`; about 120 GB of disk and
hours the first time), on Linux:

```sh
scripts/fetch-upstream.sh && scripts/build.sh slim && scripts/package.sh slim   # out/blink/slim/package
```

On an Apple silicon Mac with full Xcode, `scripts/dist.sh release` builds
`dist/release`. `mise run r` is the release of all three systems, built on
Linux: it needs Apple's SDK (`scripts/macos-toolchain.sh`) and the Windows
toolchain (`scripts/windows-toolchain.sh`) laid out first.

Below, `$PKG` is the package directory.

### Two programs

zebkit is a library: your program keeps its `main()` and calls it. This
one, `thumb.c`, renders a PDF's first page to a PNG and reads its text,
with PDFium and the image encoders inside zebkit, and nothing else to
install:

```c
#include <stdio.h>
#include <zebkit/pdf.h>
#include <zebkit/services.h>

/* Your program, your main(): zebkit is just a library it calls. */
int main(int argc, char **argv) {
    char *error = NULL;
    zebkit_pdf *pdf = zebkit_pdf_open_file(argv[1], NULL, &error);
    if (!pdf) {
        fprintf(stderr, "%s\n", error);
        return 1;
    }

    /* The first page as a PNG, at twice its size. */
    zebkit_pdf_render_options options = {.scale = 2, .background = 0xffffffff};
    int width, height;
    uint8_t *rgba = zebkit_pdf_render(pdf, 0, &options, &width, &height, &error);
    size_t size;
    uint8_t *png = zebkit_image_encode(ZEBKIT_IMAGE_PNG, rgba, width, height, 0, &size, &error);
    fwrite(png, 1, size, fopen("page1.png", "wb"));

    /* And its text. */
    char *text = zebkit_pdf_text(pdf, 0, &error);
    printf("%d pages; page 1 says: %.80s\n", zebkit_pdf_page_count(pdf), text);

    zebkit_free(text), zebkit_free(png), zebkit_free(rgba);
    zebkit_pdf_close(pdf);
    return 0;
}
```

The PDF API, compression, crypto, URLs, image codecs, ICU and the
libraries inside the engine (`zebkit_vendor_*`, FFmpeg's among them) need
no engine loop (`docs/API.md`); only HTTP through the engine's network
stack and web pages do.

Web pages need the engine running. In `app.c` the engine runs the loop:
the program starts it with `zebkit_main()` and works in its callbacks. A
program with its own loop (GTK, Qt, AppKit, winit) starts it with
`zebkit_init()` and calls `zebkit_pump()` (`examples/native-host`).

```c
#include <stdio.h>
#include <zebkit/zebkit.h>
#ifdef _WIN32
#include <zebkit/win_sandbox.h>
#endif

static void printed(zebkit_view *view, const char *json, const char *error, void *data) {
    printf("%s\n", json ? json : error);
    zebkit_quit();
}

static void loaded(zebkit_view *view, zebkit_load_event event, const char *url, void *data) {
    if (event == ZEBKIT_LOAD_FINISHED)
        zebkit_view_evaluate(view, "document.title", printed, NULL);
}

static void ready(void *data) {
    zebkit_view *view = zebkit_view_new("https://example.com/", 1024, 768);
    zebkit_view_on_load(view, loaded, NULL);
}

int main(int argc, char **argv) {
#ifdef _WIN32
    int code;
    if (zebkit_win_startup(&code))   /* this process was one of the engine's children */
        return code;
#endif
    return zebkit_main(argc, argv, ready, NULL);
}
```

On Windows the executable takes part in Chromium's sandbox: it links
`zebkit_sandbox.lib` and calls `zebkit_win_startup()` first in `main()`
(`docs/WINDOWS.md`). Linux and macOS need nothing of the kind.

### Linux

```sh
curl -LO https://github.com/ikey4u/zebkit/releases/download/v0.4.0/zebkit-0.4.0-x86_64-unknown-linux-gnu.tar.xz
tar -xJf zebkit-0.4.0-x86_64-unknown-linux-gnu.tar.xz
PKG=$PWD/zebkit-0.4.0-x86_64-unknown-linux-gnu

cc thumb.c -I$PKG/include -L$PKG -l:libzebkit.so.1 -Wl,-rpath,$PKG -o thumb
./thumb report.pdf                      # 2 pages; page 1 says: Quarterly report ...
cc app.c -I$PKG/include -L$PKG -l:libzebkit.so.1 -Wl,-rpath,$PKG -o app
./app --remote-debugging-port=9222      # with CDP for Puppeteer and Playwright
$PKG/zebkit-run https://example.com/    # the archive's own small browser window
```

The sandbox on Linux needs unprivileged user namespaces, or Chromium's
set-user-ID helper installed by root (`sudo $PKG/zebkit-sandbox-install`).
Without either the engine refuses to start and says why; in a container
of its own, add `--no-sandbox` (`docs/SANDBOX.md`).

### macOS

```sh
curl -LO https://github.com/ikey4u/zebkit/releases/download/v0.4.0/zebkit-0.4.0-aarch64-apple-darwin.tar.xz
tar -xJf zebkit-0.4.0-aarch64-apple-darwin.tar.xz
PKG=$PWD/zebkit-0.4.0-aarch64-apple-darwin

cc thumb.c -I$PKG/include -F$PKG -framework Zebkit -Wl,-rpath,$PKG -o thumb
./thumb report.pdf
cc app.c -I$PKG/include -F$PKG -framework Zebkit -Wl,-rpath,$PKG -o app
./app
$PKG/zebkit-run https://example.com/
```

The engine's helper enters macOS's sandbox itself; nothing is installed.
An application bundle with the framework inside it is what
`scripts/example.sh macos-basic` builds (`docs/MACOS.md`).

### Windows

Unpack the archive (7-Zip, or `tar -xJf` in Git Bash), then, in a command
prompt with LLVM's `clang-cl` on the path:

```bat
set PKG=%CD%\zebkit-0.4.0-x86_64-pc-windows-msvc
clang-cl /MT thumb.c /I%PKG%\include %PKG%\zebkit.dll.lib /Fe%PKG%\thumb.exe
%PKG%\thumb.exe report.pdf
clang-cl /MT app.c /I%PKG%\include %PKG%\zebkit.dll.lib %PKG%\zebkit_sandbox.lib /Fe%PKG%\app.exe
%PKG%\app.exe
%PKG%\zebkit-run.exe https://example.com/
```

The programs go beside `zebkit.dll`: the engine's sandboxed children have
no `PATH` of the parent's and find it there (`docs/WINDOWS.md`).

## Using it

### The page and the application

The application serves its own pages from a scheme and gives them
functions to call; a page has nothing of `zebkit` unless it is granted.

```c
/* The page calls zebkit.app.greet("Ada"). */
static void greet(zebkit_view *view, const char *origin, const char *args_json,
                  zebkit_call *call, void *data) {
    char answer[256];
    snprintf(answer, sizeof answer, "{\"hello\": %s}", args_json);
    zebkit_call_resolve(call, answer);
}

/* app://main/... from ./ui, in ranges for media. A real handler refuses "..". */
static void serve(zebkit_request *request, void *data) {
    char path[1024];
    snprintf(path, sizeof path, "ui%s", zebkit_request_url(request) + sizeof "app://main" - 1);
    zebkit_request_respond_file(request, path, NULL);
}

static void closed(zebkit_view *view, void *data) { zebkit_quit(); }

static void ready(void *data) {
    zebkit_view *view = zebkit_view_new("app://main/index.html", 1024, 768);
    zebkit_view_grant(view, "app://main", ZEBKIT_ALLOW_APP | ZEBKIT_ALLOW_PDF, NULL);
    zebkit_view_bind(view, "greet", greet, NULL);
    zebkit_view_on_close(view, closed, NULL);
}

int main(int argc, char **argv) {
    zebkit_scheme_register("app", ZEBKIT_SCHEME_STANDARD | ZEBKIT_SCHEME_SECURE | ZEBKIT_SCHEME_FETCH);
    zebkit_scheme_handle("app", serve, NULL);
    return zebkit_main(argc, argv, ready, NULL);
}
```

In the page, every call is a promise and bytes are `Uint8Array`s:

```js
const reply = await zebkit.app.greet('Ada');            // the application's function
const pdf = await zebkit.pdf.open(await (await fetch('report.pdf')).bytes());
const rgba = await pdf.render(0, {scale: 2});           // a page as pixels
const hits = await pdf.search('revenue');
const packed = await zebkit.compress('zstd', bytes, 19);
const hash = await zebkit.crypto.digest('SHA-256', 'abc');
```

### Fit it into the desktop

Each thing the engine would show the person, or ask of the system, has a
default that works and a function of the application's that replaces it.
Nothing reaches the system without a decision the application can take;
without a function, a page gets what a careful browser gives it, and
nothing at all where a browser would ask.

```python
import zebkit

def ready():
    # The page is the title bar: the window's buttons over it, dragged by its CSS.
    view = zebkit.View("app://main/index.html", 1000, 700, options={
        "frame": "overlay", "controls": {"height": 40, "color": "#1b1d2a", "symbolColor": "#ffffff"},
        "dragRegions": {"source": "css", "origins": ["app://main"]}})

    # The application's menu bar (macOS) and keys, with state that changes in place.
    zebkit.app_set_menu([{"title": "File", "items": [{"title": "Autosave", "id": "autosave", "checked": True}]}],
                        lambda item: zebkit.app_update_menu_item(item, checked=False))

    # A tray icon, the Dock's or the taskbar's badge, and the page's notifications as the application's.
    global tray
    tray = zebkit.Tray(open("icon.png", "rb").read(), "My application")
    zebkit.app_set_badge("3")
    zebkit.on_notification(lambda event: print("show in my own UI:", event))

    # What a page asks for: a USB device, a screen to capture, a colour, a share.
    view.on_device_chooser(lambda request: request.respond(request.metadata["devices"][0]["id"]))
    view.on_media_access(lambda request: request.respond(True, None, "view"))
    view.on_share(lambda request: request.respond(True))

    # The system: its theme and its changes, the session's lock, the screens.
    zebkit.on_environment_changed(lambda env: print("dark" if env["colorScheme"] == "dark" else "light"))
    zebkit.on_power_event(lambda event: print(event["event"]))   # suspend, resume, lock, unlock
    print(zebkit.displays())

zebkit.scheme_register("app")   # served by a scheme_handle(), as in the C example above
zebkit.main(ready)
```

| | The default | The application's |
|---|---|---|
| Window frame | the system's | `overlay` (Window Controls Overlay) or `none`, title, colours, drag regions, state, actions |
| Menus | the menu bar on macOS; the default context menu | items with state by id; the context menu replaced, extended or run |
| Dialogs | the system's alerts; the engine's prompt and sign-in where there is a window | `zebkit_view_on_dialog`, `on_http_auth`, `on_client_certificate`, the eye dropper |
| Files and devices | the system's file dialog; no device | the file chooser and File System Access; WebUSB, WebHID and Web Serial choosers |
| Capture and media | camera and microphone by permission; no screen | `getDisplayMedia()`'s source: the view itself or the screen |
| Clipboard | a page may write to it (as Chrome after a click); reading is denied | the `clipboard-read` permission |
| Notifications | undecided; then the system's own service | shown in the application's UI, with clicks and closes back to the page |
| Desktop | none | the tray, the Dock's and the taskbar's badge, progress and attention, the Dock menu, Web Share, opening URLs and folders |
| The system | a light page | the system's dark mode and contrast followed live; power, lock and display events; keeping awake |

`docs/SYSTEM_INTEGRATION.md` has each surface, its default, its status on
each system and why; `docs/API.md` has the calls in C and every binding;
`examples/custom-titlebar` is a window whose title bar is the page.

### Rust

[`zebkit`](https://crates.io/crates/zebkit) on crates.io, with
[`zebkit-build`](https://crates.io/crates/zebkit-build) to put the engine
beside your program; the build downloads the engine for your system once and
checks its SHA-256. `ZEBKIT_DIR` points it at a package instead
(`bindings/rust/README.md`).

```toml
[dependencies]
zebkit = "0.4.0"
serde_json = "1"   # for the example's JSON

[build-dependencies]
zebkit-build = "0.4.0"
```

```rust
// build.rs: puts the engine beside the executable
fn main() { zebkit_build::bundle(); }

// main.rs
fn main() {
    std::process::exit(zebkit::run(|| {
        let view = zebkit::View::new(Some("https://example.com/"), 1024, 768);
        view.grant("https://example.com", zebkit::Allow::APP, None);
        view.bind("greet", |call| {
            // The arguments are a JSON array, the answer a JSON value.
            let args: Vec<String> = serde_json::from_str(call.args_json()).unwrap_or_default();
            let name = args.first().map(String::as_str).unwrap_or("");
            call.resolve(&serde_json::to_string(&format!("hello, {name}")).unwrap());
        });
        view.on_close(zebkit::quit);
    }));
}
```

### Python

[`zebkit`](https://pypi.org/project/zebkit/) on PyPI is the binding, pure
Python on `ctypes`; the engine is the archive for your system from the
[releases](https://github.com/ikey4u/zebkit/releases) of the same version,
with `ZEBKIT_DIR` pointing at it (`bindings/python/README.md`):

```sh
pip install zebkit
export ZEBKIT_DIR=/path/to/zebkit-0.4.0-x86_64-unknown-linux-gnu
```

```python
import zebkit

def ready():
    view = zebkit.View("https://example.com/")
    view.grant("https://example.com", zebkit.ALLOW_APP)
    view.bind("greet", lambda name, origin: f"hello, {name}")

zebkit.main(ready)                 # the engine is found in $ZEBKIT_DIR
```

### Go

[`bindings/golang`](bindings/golang), on cgo: all of the C API, on Linux,
macOS and Windows; programs for Windows are cross-built with its
`cmd/zebkit-cc` (`bindings/golang/README.md`). Each release from 0.5.0 on
tags the module `bindings/golang/v<version>`, made for the engine archive of
the same version (`zebkit.BindingVersion`); 0.4.0 had no Go binding. Build
with the engine's headers and library:

```sh
go get github.com/ikey4u/zebkit/bindings/golang@v0.4.0
export CGO_CFLAGS="-I$ZEBKIT_DIR/include" CGO_LDFLAGS="-L$ZEBKIT_DIR -Wl,-rpath,$ZEBKIT_DIR"   # Linux
```

```go
os.Exit(zebkit.Main(func() {
    view, err := zebkit.NewView("", 1024, 768)
    if err != nil {
        zebkit.Quit()
        return
    }
    view.Grant("https://example.com", zebkit.AllowApp, "")
    view.Bind("greet", func(call *zebkit.Call) {
        var name string // the page's arguments, decoded in order
        if err := call.ArgsInto(&name); err != nil {
            call.Reject(err.Error())
            return
        }
        call.ResolveValue("hello, " + name) // encoded as JSON: the page gets a string
    })
    view.OnClose(zebkit.Quit)
    view.Load("https://example.com/")
}))
```

### JavaScript, without Node

`zebkit-js` runs an ES module on the engine's V8, for applications
written in JavaScript, with views, all of the C API, and native windows:

```js
// zebkit-js main.mjs
const view = new zebkit.View('app://main/index.html', {width: 1024, height: 768});
view.grant('app://main', ['app', 'pdf']);
view.bind('save', async (name, text) => {
  await zebkit.fs.writeFile(`${zebkit.args[0]}/${name}`, text);
  return {saved: true};
});
view.on('close', () => zebkit.quit());
```

`zebkit.ui` draws the window around a page, its prompts and dialogs, with
Chromium's views toolkit (`docs/UI.md`):

```js
// zebkit-js examples/ui/todo.mjs: a window of native widgets, styled with
// Tailwind's classes and kept up to date by signals (an excerpt)
const {h, signal, computed, For, window} = zebkit.ui;
const [tasks, setTasks] = signal([]);
const done = computed(() => tasks().filter((t) => t.done).length);

const Task = (task) =>
  h('view', {class: 'flex-row items-center gap-3 p-3 rounded-xl bg-[#141a2e]'},
    h('button', {class: 'w-8 h-8 p-0 rounded-full bg-[#7c5cff] text-white',
                 onClick: () => toggle(task)}, task.done ? '✓' : ''),
    h('text', {class: 'grow text-[#e6e8f2]'}, task.text));

window({title: 'Today', width: 460, height: 680}, () =>
  h('view', {class: 'flex-col gap-4 p-6 bg-[#0b1020]'},
    h('text', {class: 'text-3xl font-bold text-white'}, 'Ship something great'),
    h('text', {class: 'text-sm text-[#8a91aa]'}, () => `${done()} of ${tasks().length} done`),
    h(For, {each: tasks}, Task)));
```

![examples/ui/todo.mjs](docs/images/zebkit-ui-todo.png)

### Agents

Playwright's API runs inside the engine (`docs/AUTOMATION.md`): every
command of Playwright's protocol, in the application's process, with no
CDP and no Node.js. From C it is one call, `zebkit_automation_call(view,
"Frame.click", "{\"selector\": \"#go\"}", done, data)`; Python, Rust
(`zebkit::automation`) and Go (`github.com/ikey4u/zebkit/bindings/golang/automation`)
have it with Playwright's names:

```python
import zebkit
from zebkit.automation import Page, expect, run

async def test():
    page = Page()
    await page.goto("https://example.com/")
    await expect(page).to_have_title("Example Domain")
    await expect(page.get_by_text("documentation examples")).to_be_visible()
    await page.screenshot(path="example.png")
    zebkit.quit()

zebkit.main(lambda: run(test()), ["python", "--headless"])
```

Playwright's own clients, its test runner and its other languages included,
connect to the engine's Playwright endpoint unchanged:

```sh
$PKG/zebkit-run --headless --playwright-port=0   # prints "Playwright listening on ws://127.0.0.1:<port>/<token>"
```

```js
const browser = await chromium.connect('ws://127.0.0.1:<port>/<token>');
const page = await (await browser.newContext()).newPage();
```

Agents and tools that speak CDP drive zebkit as they would drive Chrome:

```sh
$PKG/zebkit-run --headless --remote-debugging-port=9222 https://example.com/
```

Any CDP client connects to `http://127.0.0.1:9222`.

An MCP client gets the engine's capabilities as tools from `zebkit-mcp`, a
separate program in the archive (`tools/zebkit-mcp`), with file access and
origins denied until its operator grants them ([docs/MCP.md](docs/MCP.md)):

```sh
ZEBKIT_DIR=<package> cargo build --release --manifest-path tools/zebkit-mcp/Cargo.toml   # or the archive's
$PKG/zebkit-mcp --allow-origin https://example.com --allow-write ~/agent-files   # on standard input and output
$PKG/zebkit-mcp --http 8931 --allow-origin '*'                                   # on 127.0.0.1, with a token
```

### The examples

`examples/README.md` lists them all.

```sh
mise example:linux:basic    # examples/window.c: a browser window
mise example:rust:window    # the same in Rust
mise example:ffmpeg         # examples/ffmpeg.cpp: a library inside the engine
(cd examples/gpui-browser && cargo run --release)   # a tabbed browser in GPUI-KIT
```

## What is in it

- **Integrate it, don't build on it.** Electron is a base: an application
  is built inside it, on its runtime, its windows and its packaging.
  zebkit is a part: an application adds it, as it adds SQLite, and keeps
  its process, its toolkit and its way of shipping. It runs inside the
  application's own event loop (`zebkit_init()`, `zebkit_pump()`), and draws
  in the application's windows, as frames you draw or in a child of a window
  you give it, an X11 window, an `NSView` or an `HWND` (`examples/native-host`),
  and gives back the window a page is in for the system's dialogs
  (`<zebkit/window.h>`).
- **The real Chrome engine.** Pages see Chromium 157: its user agent and
  client hints, `window.chrome`, popups blocked without a click, Chrome's
  focus ring, and nothing of a debugger attached (`docs/API.md` has what
  still differs, Google's sign-in among it).
- **No Node.** No second JavaScript runtime and no IPC through one. The
  application is native code, or JavaScript run by `zebkit-js` on the
  engine's V8.
- **Chromium's libraries are yours.** 17 C libraries inside the engine, about
  7,490 functions on each of Linux, macOS and Windows (7,487 on all three, the
  rest each system's own), exported under `zebkit_vendor_*`: SQLite, LevelDB, BoringSSL,
  zlib, Zstandard, Brotli, ICU, HarfBuzz, libxml2, libwebp, libjpeg-turbo,
  FFmpeg, dav1d, Opus, libvpx, FreeType and PDFium. You do not ship them twice.
- **PDF built in.** A full API on Chromium's PDFium (`<zebkit/pdf.h>`):
  rendering and tiles,
  text with layout, search, outline, links, annotations as an editor has them
  (line endings, cloudy borders, free text with callouts and rich text, stamps,
  replies and states, measurement, XFDF and FDF), forms, redaction, page editing,
  merging, attachments, metadata, and signatures checked for integrity; hooks for
  an application's own annotation kinds, appearances, events with undo, render
  layers, OCR, fonts, trust and actions. In C and every binding.
- **2D drawing and shaping.** `<zebkit/canvas.h>` on the engine's Skia and
  `<zebkit/text.h>` on its HarfBuzz, without shipping either again.
- **Media, saved as it plays.** Whatever a page plays, a file, MSE/DASH or
  HLS, written to MP4 or WebM as it was sent, without re-encoding and
  without a second download (`<zebkit/media.h>`); the decoded frames and
  sound as they play (`<zebkit/media_tap.h>`, `docs/MEDIA_TAP.md`); probing,
  remuxing and extracting with the engine's FFmpeg (`<zebkit/media_tools.h>`,
  `docs/MEDIA_TOOLS.md`).
- **Bytes as they are.** A page's calls to the application go over its
  frame's own Mojo channel, large ones through shared memory: no base64, the
  page's JSON handed on as it was written, as fast as Electron's IPC for small
  calls and faster for large ones.
- **The window around a page, from JavaScript.** `zebkit.ui` draws
  windows, prompts and dialogs with Chromium's views toolkit: signals,
  JSX, Tailwind classes, and automation for tests and agents. Rich UI is
  a page; `zebkit.ui` is the chrome around it (`docs/UI.md`).
- **Playwright's API, natively.** The whole of Playwright (locators,
  actions that wait until they can be done, keyboard, mouse and touch,
  routes, dialogs, downloads, contexts, workers, traces, HAR, video) runs
  inside the engine, without CDP or a Node.js driver: up to 16 times as fast
  as the same commands over CDP, a click in 2 to 4 ms instead of 33
  (2026-11-07, Chromium 155; `<zebkit/automation.h>`, `docs/AUTOMATION.md`). Playwright's own
  clients, in any of its languages, connect to it unchanged with
  `chromium.connect()`.
- **Made for agents.** `tools/zebkit-mcp` gives them pages, PDFs, files, media
  and native windows as MCP tools, over standard input and HTTP, within bounds
  its operator sets (`docs/MCP.md`). They can also drive zebkit through
  Playwright's API or over CDP, Chromium's own DevTools server, headless or
  windowed.
- **Customise it from your code, down to the desktop.** Native hooks
  (`<zebkit/hooks.h>`) decide what the engine does, in C and every binding:
  permissions,
  navigation, popups, dialogs, downloads and how each ended, certificates and client
  certificates, sign-in, network requests and responses at every stage,
  settings at run time, your own URL schemes, what a page may call, scripts
  before the page's own (`docs/HOOKS.md`). And everything a page or
  the engine would show the person or ask of the system goes through the
  application: the window's frame (system, overlay or none, with the page as
  the title bar), menus and the context menu, JavaScript dialogs and sign-in,
  file and device choosers, screen capture, notifications, the tray, the
  Dock and taskbar, sharing, the system's theme, power and displays
  ([Fit it into the desktop](#fit-it-into-the-desktop),
  `docs/SYSTEM_INTEGRATION.md`).
- **Input methods.** Chinese, Japanese and Korean are typed into X11 windows
  through IBus (ibus-daemon, or fcitx5's IBus frontend), with the
  composition drawn in the page; off-screen views compose through the
  application (`docs/ARCHITECTURE.md`).
- **Safe by default.** A page has no `zebkit` object unless the
  application grants it, per view, per origin and per capability; the
  origin is the browser's, never the page's word; the renderer stays
  sandboxed; permissions a browser would ask about are not granted.

## Benchmark

### Against Electron (2026-11-08, Chromium 157)

Against the latest stable Electron, 45.2.1 (Chromium 156), measured the same way:
one harness, the same pages, two thin hosts that do the same thing, every process
counted, medians of 5 to 7 runs taking turns, Linux x86-64, 2026-11-08
(`scripts/electron-compare.sh`, `docs/reports/electron-compare.md`). The
engine measured is Chromium 157.0.8103.38 as released in 0.3.0; 0.4.0 left
its hot paths as they were (`docs/releases/0.4.0.md`).

| | zebkit (Chromium 157) | Electron 45.2.1 | |
|---|---:|---:|---|
| As an application ships it, uncompressed / `.tar.xz` | **174.1 / 60.5 MiB** | 260.5 / 87.1 MiB | 33% / 30% smaller |
| As published, uncompressed / `.tar.xz` | **182.6 / 61.9 MiB** | 273.4 / 88.7 MiB | 33% / 30% smaller |
| Start, cold, to first paint | **2.3 s** | 5.8 s | 60% less |
| Start, warm, to first paint | **124 ms** | 236 ms | 47% less |
| Memory, a blank page | **181 MB** | 250 MB | 27% less |
| A call from the page to the host and back, 1 MiB / 16 MiB | **3.3 / 56.5 ms** | 4.2 / 71.6 ms | 21% / 21% less |
| JavaScript and DOM, ten workloads | **1086 ms** | 1161 ms | 6% less |
| DOM, 20,000 elements made and laid out (one of the ten) | 253 ms | 276 ms | 8% less, within noise |
| Text layout, a chapter of a book | 7.5 ms | 7.4 ms | level, within noise |
| A long document, 200,000 words: CPU / memory | **3.47 s / 264 MB** | 4.13 s / 333 MB | 16% / 21% less |
| A 756-page PDF in each one's viewer: CPU loading / memory | **0.73 s / 264 MB** | 1.59 s / 400 MB | 54% / 34% less |
| Ten popular sites: CPU loading / memory | **18.3 s / 4495 MB** | 19.8 s / 4661 MB | 8% / 4% less |
| Processes, a page | 7 | **6** | one more: storage sandboxed in its own, as Chrome |

- **Size.** zebkit keeps the whole web platform applications use: HTML, CSS,
  JavaScript with every JIT tier, WebAssembly, WebGL, media with H.264 and AAC,
  HTTP/3, PDF.
- **Speed.** Chrome's own profile-guided optimization reaches Blink because
  zebkit is built with CFI as Chrome is; every speed change is kept only when it
  gains more than it costs in size (`docs/SIZE_AND_SPEED.md`).
- **Where Electron is ahead**: one process fewer a page; memory on YouTube
  (574 against 678 MB); and, within noise, memory on The New York Times and
  The Verge and the CPU they take loading, where advertising and video decide.
- How each number is made: `docs/SIZE_AND_SPEED.md`.

### Playwright's commands (2026-11-07, Chromium 155)

The same Playwright commands three ways, headless, medians, 2026-11-07:
zebkit on Chromium 155.0.8059.84, Chrome 156.0.8078.4, playwright-core
1.64.1; not run again on Chromium 157 (`docs/reports/automation-bench.md`,
`scripts/automation-bench.sh`):

| | zebkit's API, in-process | Your Playwright scripts, connected to zebkit | Playwright with Chrome 156 |
|---|---:|---:|---:|
| `locator.click()` | **2.0 ms** | 3.5 ms | 33.3 ms |
| `locator.fill()` | 2.1 ms | **2.0 ms** | 2.9 ms |
| `locator.press('Enter')` | **0.59 ms** | 0.98 ms | 2.6 ms |
| `locator.textContent()` | **0.23 ms** | 0.70 ms | 0.81 ms |
| `allTextContents()` of 200 | **0.34 ms** | 1.9 ms | 1.9 ms |
| `page.goto()` | **11.2 ms** | 13.0 ms | 18.2 ms |
| `page.screenshot()` | **12.4 ms** | 16.7 ms | 33.8 ms |
| `page.screenshot({type: 'jpeg'})` | **13.9 ms** | 14.4 ms | 32.9 ms |
| the screenshot's pixels, unencoded | **9.3 ms** | | |

- **zebkit's API, in-process**: your program calls the command through
  `<zebkit/automation.h>`, Python's `zebkit.automation`, Rust's
  `zebkit::automation` or Go's `github.com/ikey4u/zebkit/bindings/golang/automation`; it runs inside the engine, with no Node.js and no protocol.
- **Your Playwright scripts, connected to zebkit**: Playwright's own Node.js
  client, unchanged, with `chromium.connect()` to zebkit's endpoint; its test
  suite (v1.64.1) passes 4,047 of its tests there on Chromium 157
  (2026-11-08), Chrome 4,196 (`docs/AUTOMATION.md`, section 7).
- **Playwright with Chrome**: Playwright as usual, over CDP.
- **Why it is faster**: one call in the renderer where Playwright's server makes
  several round trips over CDP, no JSON between processes, no Node.js process
  (Playwright's alone reached 175 MiB); and the frames a click's checks wait for
  are made at once instead of at 60 Hz on a page without a display.
- **A screenshot is 2.7 times faster**: the fresh frame it takes is made at
  once instead of at the next 60 Hz tick, the PNG is deflated at level 1 with
  the Up filter (2 ms where Chrome's default level takes 13, for 59 KB
  instead of 40 KB on this page), and the picture's shared memory is made in
  one call instead of a page fault at a time. What is left of the 12 ms is the
  page's frame (4 ms), copying the picture out of the compositor (4 ms) and
  encoding it (4 ms). The pixels themselves, with no encoding
  (`zebkit_automation_screenshot_pixels()`, zebkit's own), take 9 ms. Over CDP
  zebkit does what Chrome does: frames at 60 Hz, Chrome's PNG.

## Reference

### Documentation

| Document | What it covers |
|---|---|
| **Use** | |
| [docs/API.md](docs/API.md) | Everything an application can call, in C and in every binding (Rust, Python, Go, host JavaScript) |
| [docs/SYSTEM_INTEGRATION.md](docs/SYSTEM_INTEGRATION.md) | Every place the engine meets the system or the person: windows, menus, dialogs, permissions, devices, notifications; each one's default and status |
| [docs/HOOKS.md](docs/HOOKS.md) | Hooks (`<zebkit/hooks.h>`): how an application decides what the engine does, from native code |
| [docs/AUTOMATION.md](docs/AUTOMATION.md) | Playwright's API without CDP (`<zebkit/automation.h>`): how Playwright works, what zebkit does instead, the endpoint, what it costs |
| [docs/MCP.md](docs/MCP.md) | `tools/zebkit-mcp`: the engine as MCP tools for agents, its bounds, what is weak |
| [docs/MEDIA_TAP.md](docs/MEDIA_TAP.md) | `<zebkit/media_tap.h>`: the decoded frames and sound of what a page plays |
| [docs/MEDIA_TOOLS.md](docs/MEDIA_TOOLS.md) | `<zebkit/media_tools.h>`: probing, remuxing and extracting with the engine's FFmpeg |
| [docs/UI.md](docs/UI.md) | `zebkit.ui`: the window, prompts and dialogs around a page, what it is for and what it is not |
| [docs/SANDBOX.md](docs/SANDBOX.md) | The sandbox on each system, and running without one |
| [docs/LICENSING.md](docs/LICENSING.md) | zebkit's license, the third-party licenses inside the engine, and what shipping it involves |
| [bindings/rust/README.md](bindings/rust/README.md) | The Rust crates |
| [bindings/python/README.md](bindings/python/README.md) | The Python binding |
| [bindings/golang/README.md](bindings/golang/README.md) | The Go binding: what it covers, building with cgo, Windows |
| [examples/README.md](examples/README.md) | The examples and how to run them |
| **Build** | |
| [docs/BUILD.md](docs/BUILD.md) | Fetching Chromium, building, packaging, testing, measuring and releasing; profiles and patches |
| [docs/MACOS.md](docs/MACOS.md) | The macOS framework, native and cross builds, testing on a Mac |
| [docs/WINDOWS.md](docs/WINDOWS.md) | The Windows DLL, its sandbox, cross-building on Linux, the state of the port |
| [CONTRIBUTING.md](CONTRIBUTING.md) | Building, testing and the rules a change is checked against |
| [SECURITY.md](SECURITY.md) | Reporting a vulnerability; which versions get fixes |
| **Design** | |
| [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) | How the engine is made, and what in this repository is what |
| [docs/SIZE_AND_SPEED.md](docs/SIZE_AND_SPEED.md) | Where the size goes, what was cut and kept, the compiler against speed, the Electron baseline |
| [docs/ROADMAP.md](docs/ROADMAP.md) | The library and the engine: requirements, what is done, what is next |
| **Investigations** | |
| [docs/notes/cookie-persistence.md](docs/notes/cookie-persistence.md) | Cookies kept across restarts, and their encryption on each system |
| [docs/notes/google-signin.md](docs/notes/google-signin.md) | Google's sign-in in zebkit, compared with Chrome |
| [docs/notes/passkeys.md](docs/notes/passkeys.md) | Passkeys and WebAuthn on each system |
| **Releases and measurements** | |
| [docs/releases/](docs/releases) | Each release: what is in it, what was tested, what is missing; [next.md](docs/releases/next.md) what is on `main` since the last one |
| [docs/reports/](docs/reports) | Size reports and benchmarks, written by the scripts that measure |

### Repository

| Path | What it holds |
|---|---|
| `upstream.lock` | The Chromium release built, the pinned depot_tools, the Electron release measured against |
| `profiles/` | Build flavours as GN arguments: `slim.gn` (the product), `slim-macos.gn`, `slim-windows.gn`, and the development and measuring ones (`docs/BUILD.md`, "Profiles") |
| `patches/` | Changes to Chromium, applied in the order of `patches/series` |
| `src/engine/` | zebkit's own code, linked into Chromium as `//zebkit/engine`: the library, its C API and the headers (`include/zebkit/`: `zebkit.h`, `window.h`, `hooks.h`, `page.h`, `offscreen.h`, `services.h`, `pdf.h`, `canvas.h`, `text.h`, `media.h`, `media_tap.h`, `media_tools.h`, `automation.h`, `win_sandbox.h`) |
| `src/ffmpeg/` | zebkit's FFmpeg configuration, from `scripts/ffmpeg-config.sh` |
| `bindings/rust/`, `bindings/python/`, `bindings/golang/` | The Rust crates, the Python binding and the Go binding |
| `tools/` | `zebkit-run` (open pages, serve CDP or Playwright's endpoint), `zebkit-js` (applications in JavaScript) and `zebkit-mcp` (the engine as MCP tools) |
| `tests/` | Programs that test every layer of the API against a package; speed workloads |
| `examples/` | A browser window in C, `zebkit.ui`, applications' own windows and loops, a library inside the engine, the Electron baseline |
| `scripts/` | Every step, one script each; `mise tasks` lists the short forms |

`upstream/` (Chromium's tree, about 30 GB without a build), `out/`, `dist/`
and `.cache/` are generated and not tracked.

zebkit is Chromium plus a small patch set (`patches/`), built from source.
Where Chromium lacks something an application needs, zebkit patches
Chromium rather than working around it (`docs/BUILD.md`).

## License

zebkit is free software under the GNU Lesser General Public License, version
2.1 or (at your option) any later version: [LICENSE](LICENSE). Applications
of any license, open or closed, can use it as a library.

The engine contains Chromium and the libraries it uses (Blink, V8, Skia,
PDFium, FFmpeg in its LGPL configuration, ICU, SQLite, BoringSSL and others),
each under its own license. From the release after 0.4.0 on, every release
archive carries `LICENSE`, a `NOTICE` that says where its source is, and
`THIRD_PARTY_NOTICES.txt` with every third-party license text; 0.4.0's
archives do not have them yet. [docs/LICENSING.md](docs/LICENSING.md) says
what an application that ships zebkit has to do.

Contributions are accepted under the same license: by sending a pull request
you agree that your contribution is licensed under the LGPL 2.1 or later, as
the rest of zebkit ([CONTRIBUTING.md](CONTRIBUTING.md)).
