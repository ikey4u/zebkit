<h1 align="center"><img src="docs/images/logo.svg" width="360" alt="zebkit"></h1>

<p align="center"><b>Chrome as a library — light, fast, everywhere, and easy.</b><br>One API on Linux and macOS today, Windows next. Add it in a few lines.</p>

## Introduction

zebkit is Chromium's engine as one library you link into your own
application: Blink, V8 with every JIT tier, PDFium, FFmpeg and the rest of
what Chrome is made of, with nothing of Chrome's browser and nothing of
Electron's runtime. Your application stays yours, in C, C++, Rust, Python
or JavaScript, with its own windows and toolkit. Where it needs the web,
it adds a zebkit view; where it needs a PDF, media, compression or crypto,
it calls zebkit's API: in C, or through the bindings for Rust, Python and
JavaScript (`zebkit-js`, and `zebkit.*` in the application's own pages).

<p align="center"><img src="docs/images/architecture.svg" width="900" alt="The application links one library, zebkit, which runs Chromium's content layer, Blink and V8 inside the application's process; pages run in sandboxed helper processes; Linux and macOS today, Windows planned"></p>

- **Integrate it, don't build on it.** Electron is a base: an application
  is built inside it, on its runtime, its windows and its packaging.
  zebkit is a part: an application adds it, as it adds SQLite, and keeps
  its process, its toolkit and its way of shipping. Running inside the
  application's own event loop (GTK, Qt, AppKit, Win32, winit) is next on
  the roadmap (`docs/ROADMAP.md`, "The library").
- **Take what you need.** Web views, in a window or off-screen with their
  frames handed to you; a full PDF API; saving what a page plays; your own
  URL schemes; native UI from JavaScript; Playwright's API and CDP for
  agents. Use one, or all.
- **The real Chrome engine.** Pages see Chromium 155: its user agent and
  client hints, `window.chrome`, popups blocked without a click, and
  nothing of a debugger attached (`docs/API.md` has what still differs,
  Google's sign-in among it).
- **Lighter than Electron.** 38% smaller, and real web pages cost 21% less
  CPU and 17% less memory than in the latest Electron
  ([Benchmark](#benchmark)).
- **No Node.** No second JavaScript runtime and no IPC through one. The
  application is native code, or JavaScript run by `zebkit-js` on the
  engine's V8.
- **Chromium's libraries are yours.** 16 C libraries inside the engine,
  7,421 functions in Linux slim, exported under `zebkit_vendor_*`: SQLite, BoringSSL,
  zlib, Zstandard, Brotli, ICU, HarfBuzz, libxml2, libwebp, libjpeg-turbo,
  FFmpeg, dav1d, Opus, libvpx, FreeType and PDFium. You do not ship them twice.
- **PDF built in.** A full API on Chromium's PDFium: rendering and tiles,
  text with layout, search, outline, links, annotations with appearances,
  forms, redaction, page editing, merging, attachments, metadata, and
  signatures checked for integrity. In C and in page JavaScript.
- **Media, saved as it plays.** Whatever a page plays, a file, MSE/DASH or
  HLS, written to MP4 or WebM as it was sent, without re-encoding and
  without a second download (`<zebkit/media.h>`).
- **Bytes as they are.** A page's calls to the application go over a
  scheme of the engine's own, through Mojo's data pipes: no base64, no JSON
  around binary data, many calls under way at once.
- **The window around a page, from JavaScript.** `zebkit.ui` draws
  windows, prompts and dialogs with Chromium's views toolkit: signals,
  JSX, Tailwind classes, and automation for tests and agents. Rich UI is
  a page; `zebkit.ui` is the chrome around it (`docs/UI.md`).
- **Playwright's API, natively.** The whole of Playwright (locators,
  actions that wait until they can be done, keyboard, mouse and touch,
  routes, dialogs, downloads, contexts, workers, traces, HAR, video) runs
  inside the engine, without CDP or a Node.js driver: 3 to 10 times faster
  than Playwright over CDP, and a click in 3 to 5 ms instead of 33
  (`<zebkit/automation.h>`, `docs/AUTOMATION.md`). Playwright's own
  clients, in any of its languages, connect to it unchanged with
  `chromium.connect()`.
- **Made for agents.** Agents drive zebkit through Playwright's API or over
  CDP, Chromium's own DevTools server, headless or windowed. A general MCP
  server giving them pages, PDFs, files, media and native windows as tools
  is planned (`docs/ROADMAP.md`, "Agents").
- **Customise it from your code.** Native hooks decide what the engine
  does: new windows and popups, dialogs, drag and drop, your own URL
  schemes, what a page may call, scripts before the page's own.
  *Planned:* hooks at every point where Chromium decides, permissions,
  navigation, network requests, certificates, downloads and more
  (`docs/ROADMAP_HOOKS.md`).
- **Safe by default.** A page has no `zebkit` object unless the
  application grants it, per view, per origin and per capability; the
  origin is the browser's, never the page's word; the renderer stays
  sandboxed; permissions a browser would ask about are not granted.

| | |
|---|---|
| Engine | Chromium 155.0.8059.84 (`upstream.lock`): content layer, Blink, V8 with every JIT tier |
| Linux | x86-64, glibc 2.17 or newer (CentOS 7 and later): `libzebkit.so.1` and `zebkit-helper` |
| macOS | Apple silicon: `Zebkit.framework`, built natively or cross-built on Linux (`docs/MACOS.md`) |
| Windows | Planned, with the same API |
| Version | `VERSION`: 0.0.1-alpha.12 |

## Quick start

### Get the engine

A package is a directory with the library, the helper, the engine's data
and `include/`. Either unpack a release's archive
(`zebkit-<version>-<target>.tar.xz` from GitHub's releases), or build one:

```sh
mise run r      # dist/release (Linux) and dist/release-macos (macOS); hours the first time
```

On an Apple silicon Mac with full Xcode installed, `scripts/dist.sh release`
builds the engine locally into `dist/release`. Then run
`scripts/example.sh macos-basic` to build and launch the C embedding demo
in `dist/example/Zebkit Embedded Browser.app`.

Building needs about 30 GB for Chromium's tree (`docs/BUILD.md`). Below,
`$PKG` is the package directory.

### Add it to a program you already have

zebkit is a library: your program keeps its `main()` and calls it. This
one renders a PDF's first page to a PNG and reads its text, with PDFium
and the image encoders inside zebkit, and nothing else to install:

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

```sh
cc thumb.c -I$PKG/include -L$PKG -l:libzebkit.so.1 -Wl,-rpath,$PKG -o thumb   # Linux
cc thumb.c -I$PKG/include -F$PKG -framework Zebkit -Wl,-rpath,$PKG -o thumb   # macOS
./thumb report.pdf     # 2 pages; page 1 says: Quarterly report ...
```

The PDF API, compression, crypto, URLs, image codecs, ICU and the
libraries inside the engine (`zebkit_vendor_*`, FFmpeg's among them) need
no engine loop (`docs/API.md`); only HTTP through the engine's network
stack and web pages do.

### Add a web view

Web pages need the engine running. Today the engine runs the loop: the
program starts it with `zebkit_main()` and works in its callbacks.
Starting it from inside the program's own loop (GTK, Qt, AppKit, Win32,
winit) with `zebkit_init()` comes next (`docs/ROADMAP.md`, "The library").

```c
#include <stdio.h>
#include <zebkit/zebkit.h>

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
    return zebkit_main(argc, argv, ready, NULL);
}
```

```sh
cc app.c -I$PKG/include -L$PKG -l:libzebkit.so.1 -Wl,-rpath,$PKG -o app   # Linux
cc app.c -I$PKG/include -F$PKG -framework Zebkit -Wl,-rpath,$PKG -o app   # macOS
./app --remote-debugging-port=9222     # with CDP for Puppeteer and Playwright
```

In a container without unprivileged user namespaces, add `--no-sandbox`.

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

### Rust

```toml
[dependencies]
zebkit = "0.0.1-alpha.12"

[build-dependencies]
zebkit-build = "0.0.1-alpha.12"
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
            let answer = format!("\"hello, {}\"", call.args_json());
            call.resolve(&answer)
        });
        view.on_close(zebkit::quit);
    }));
}
```

`ZEBKIT_DIR` points the build at a package (`rust/README.md`).

### Python

```python
import zebkit                      # bindings/python, on ctypes: nothing to compile

def ready():
    view = zebkit.View("https://example.com/")
    view.grant("https://example.com", zebkit.ALLOW_APP)
    view.bind("greet", lambda name, origin: f"hello, {name}")

zebkit.main(ready)                 # the engine is found in $ZEBKIT_DIR
```

### JavaScript, without Node

`zebkit-js` runs an ES module on the engine's V8, for applications
written in JavaScript, with views, the same APIs, and native windows:

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

```js
// zebkit-js todo.mjs: a window of native widgets
const {h, signal, For, window} = zebkit.ui;
const [todos, setTodos] = signal([]);
const [draft, setDraft] = signal('');

window({title: 'To do', width: 420, height: 440}, () =>
  h('view', {class: 'flex-col gap-3 p-6 bg-gray-50'},
    h('input', {placeholder: 'What needs doing?', value: draft, onInput: setDraft,
                onSubmit: () => { setTodos((t) => [...t, draft()]); setDraft(''); }}),
    h(For, {each: todos}, (todo) => h('text', {}, todo)),
    h('text', {class: 'text-sm text-gray-500'}, () => `${todos().length} things`)));
```

![examples/ui/todo.mjs](docs/images/zebkit-ui-todo.png)

### Agents

Playwright's API runs inside the engine (`docs/AUTOMATION.md`): every
command of Playwright's protocol, in the application's process, with no
CDP and no Node.js. From C it is one call, `zebkit_automation_call(view,
"Frame.click", "{\"selector\": \"#go\"}", done, data)`; Python has it with
Playwright's names:

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
tools/zebkit-run --headless --playwright-port=0   # prints "Playwright listening on ws://127.0.0.1:<port>/"
```

```js
const browser = await chromium.connect('ws://127.0.0.1:<port>/');
const page = await (await browser.newContext()).newPage();
```

Agents and tools that speak CDP drive zebkit as they would drive Chrome:

```sh
tools/zebkit-run --headless --remote-debugging-port=9222 https://example.com/
```

Any CDP client connects to `http://127.0.0.1:9222`. An MCP server that gives
agents zebkit's capabilities as tools is planned (`docs/ROADMAP.md`,
"Agents").

### The examples

```sh
mise example:linux:basic    # examples/window.c: a browser window
mise example:rust:window    # the same in Rust
mise example:ffmpeg         # examples/ffmpeg.cpp: a library inside the engine
```

## Benchmark

Every comparison is with the latest stable Electron, 44.9 (Chromium 152).
A comparison of whole applications, the same one on both, is still to be
made (`docs/ROADMAP.md`).

| | zebkit `slim`, 2026-10-27 | Electron 44.9, trimmed | zebkit is |
|---|---:|---:|---:|
| Uncompressed | **158.7 MiB** | 255.7 MiB | 38% smaller |
| `.tar.xz` | **54.3 MiB** | 85.3 MiB | 36% smaller |
| `.zip` | **72.3 MiB** | 112.6 MiB | 36% smaller |
| Text layout (a chapter of a book) | 10.9 ms | **9.2 ms** | 18% slower |
| A web page: CPU in its first 30 s, on average | **4.6 s** | 5.5 s | 16% less |
| A web page: memory, on average | **416 MB** | 498 MB | 16% less |

- **Size.** Electron as shipped is 283.3 MiB; "trimmed" is a hello world
  with its locales and extras removed, the fairest baseline we could make
  (`examples/electron-hello`). zebkit keeps the whole web platform that
  applications use: HTML, CSS, JavaScript with every JIT tier,
  WebAssembly, WebGL, media with H.264 and AAC, HTTP/3.
- **Text layout.** A chapter of a book in columns, laid out again as its
  window is resized (`scripts/layout-bench.sh`). Both engines lay out the
  same lines; zebkit's code runs slower for being compiled for size.
  Compiled for speed (`slim-speed`) it is within 4% of Google's own build
  of Chromium 155, which is itself 8% behind Electron's Chromium 152, at
  a cost of 19% in size; the release stays `slim`
  (`docs/SIZE_AND_SPEED.md`).
- **Web pages.** Eleven popular sites (Wikipedia, YouTube, Amazon, The New
  York Times, GitHub and others) and three local pages, each loaded for
  10 s and shown for 20 s more, each alone in a freshly started engine in
  a 1280 by 800 window, the median of 3 runs, CPU and memory of every
  process (the averages above are over the 14). A simple page takes
  172 MB, two thirds of it the engine's own code, which every further
  page shares. zebkit costs less CPU on 12 of the 14 and less memory on
  all 14; on BBC News and Stack Overflow the CPU is level. Reddit, the
  fifteenth, showed zebkit a reCAPTCHA in place of the page and is left
  out (`docs/reports/web-bench.md`, `scripts/web-bench.sh`).
- How each number is made: `docs/SIZE_AND_SPEED.md`.

Playwright's commands, natively and through Playwright over CDP, headless,
medians (`docs/reports/automation-bench.md`, `scripts/automation-bench.sh`):

| | zebkit, native | zebkit, Playwright's client to its endpoint | Chrome 156, Playwright over CDP |
|---|---:|---:|---:|
| `locator.click()` | **4.9 ms** | 3.5 ms | 33.4 ms |
| `locator.fill()` | **0.66 ms** | 2.4 ms | 2.7 ms |
| `locator.press('Enter')` | **0.40 ms** | 0.82 ms | 2.3 ms |
| `locator.textContent()` | **0.22 ms** | 0.65 ms | 0.90 ms |
| `allTextContents()` of 200 | **0.26 ms** | 2.1 ms | 1.8 ms |
| `page.screenshot()` | **29.9 ms** | 44.2 ms | 33.4 ms |

- **One call in the renderer** where Playwright's server makes several
  round trips over CDP, and no JSON between processes; no Node.js process
  (Playwright's alone reached 175 MiB).
- **A click without waiting for the display**: the frames Playwright's
  checks wait for are made at once while a command is in flight on a page
  without a display, instead of at 60 Hz.

## Reference

### Documentation

| Document | What it covers |
|---|---|
| [docs/API.md](docs/API.md) | Everything an application can call, in C, Rust, Python and JavaScript |
| [docs/BUILD.md](docs/BUILD.md) | Fetching Chromium, building, packaging, testing, measuring and releasing; profiles and patches |
| [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) | How the engine is made, and what in this repository is what |
| [docs/AUTOMATION.md](docs/AUTOMATION.md) | Playwright's API without CDP: how Playwright works, what zebkit does instead, the endpoint, what it costs |
| [docs/SIZE_AND_SPEED.md](docs/SIZE_AND_SPEED.md) | Where the size goes, what was cut and kept, the compiler against speed, the Electron baseline |
| [docs/MACOS.md](docs/MACOS.md) | The macOS framework, native and cross builds |
| [docs/ROADMAP.md](docs/ROADMAP.md) | The library and the engine: requirements, what is done, what is next |
| [docs/ROADMAP_HOOKS.md](docs/ROADMAP_HOOKS.md) | Hooks: how an application decides what the engine does, from native code |
| [docs/UI.md](docs/UI.md) | `zebkit.ui`: the engine's own small UI, what it is for and what it is not |
| [rust/README.md](rust/README.md) | The Rust crates |
| `docs/reports/` | Size reports, regenerated by the scripts |

### Repository

| Path | What it holds |
|---|---|
| `upstream.lock` | The Chromium release built, the pinned depot_tools, the Electron release measured against |
| `profiles/` | Build flavours as GN arguments: `slim.gn` (the product), `slim-macos.gn`, `baseline.gn` (Chromium's defaults) |
| `patches/` | Changes to Chromium, applied in the order of `patches/series` |
| `src/engine/` | zebkit's own code, linked into Chromium as `//zebkit/engine`: the library, its C API and headers |
| `src/ffmpeg/` | zebkit's FFmpeg configuration, from `scripts/ffmpeg-config.sh` |
| `rust/`, `bindings/python/` | The Rust crates and the Python binding |
| `tools/` | `zebkit-run` (open pages, serve CDP or Playwright's endpoint) and `zebkit-js` (applications in JavaScript) |
| `tests/` | Programs that test every layer of the API against a package; speed workloads |
| `examples/` | A browser window in C, `zebkit.ui`, a library inside the engine, the Electron baseline |
| `scripts/` | Every step, one script each; `mise tasks` lists the short forms |

`upstream/` (Chromium's tree, about 30 GB), `out/`, `dist/` and `.cache/`
are generated and not tracked.

zebkit is Chromium plus a small patch set (`patches/`), built from source.
Where Chromium lacks something an application needs, zebkit patches
Chromium rather than working around it (`docs/BUILD.md`).
