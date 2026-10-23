# zebkit

**Chrome's web engine as a library, for applications that would otherwise
ship Electron.**

zebkit is Chromium's content layer and Blink built as one shared library
with a helper program, and nothing of Chrome's browser. An application
links it, opens web pages in its windows, serves the Chrome DevTools
Protocol to agents, and calls the libraries Chromium already carries
(SQLite, BoringSSL, ICU, FFmpeg, PDFium and more) through a C API, with
bindings for Rust, Python and the pages' own JavaScript. There is no Node
and no second runtime: the application is native code, or JavaScript on
the engine's own V8.

## Against Electron

### The engine: a third smaller, the same speed class

| | zebkit 0.0.1-alpha.4 | Electron 44.9, trimmed | zebkit is |
|---|---:|---:|---:|
| Uncompressed | **157.3 MiB** | 255.7 MiB | 38% smaller |
| `.tar.xz` | **53.9 MiB** | 85.3 MiB | 37% smaller |
| `.zip` | **71.8 MiB** | 112.6 MiB | 36% smaller |
| Text layout benchmark | **8.7 ms** | 9.7 ms (Electron 36) | 10% faster |

Electron as shipped is 283.3 MiB; "trimmed" is a hello world with its
locales and extras removed, the fairest baseline we could make
(`examples/electron-hello`, `docs/SIZE_AND_SPEED.md`). zebkit keeps the
whole web platform that applications use (HTML, CSS, JavaScript with every
JIT tier, WebAssembly, WebGL, media with H.264 and AAC, HTTP/3) and is
built with Chromium's PGO profile and link-time optimization.

### A real application

A desktop document reader with PDF, EPUB and OCR, moved from Electron to
zebkit. The same harness, production builds, medians of five runs on
Linux:

| | zebkit | Electron | |
|---|---:|---:|---:|
| Start | **0.50 s** | 1.08 s | 2.2× faster |
| CPU at idle | **0.35%** | 51.5% | 147× less |
| Memory at idle | **290 MB** | 958 MB | 3.3× less |
| 756-page PDF: first page | **1.12 s** | 5.3 s | 4.7× faster |
| PDF: fully loaded | **3.2 s** | 11.8 s | 3.7× faster |
| PDF: longest freeze | **0.10 s** | 2.9 s | 29× shorter |
| PDF: scroll frames p50 / p95 / max | **22 / 48 / 66 ms** | 170 / 456 / 1,412 ms | 8–21× smoother |
| PDF: CPU time | **24 s** | 163 s | 6.8× less |
| PDF: memory | **1.37 GB** | 3.1 GB | 2.3× less |
| EPUB: CPU time | **1.1 s** | 89 s | 81× less |
| Installed size | **262 MiB** | 1,490 MiB | 5.7× smaller |

The gains are not only the engine's size. They come from what an
application stops doing when its runtime is not Electron.

## Why zebkit

- **No Node.** No second JavaScript runtime, no IPC between a main
  process and the renderer through it. The application is native code, or
  JavaScript run by `zebkit-js` on the engine's V8.
- **Bytes as they are.** A page's calls to the application go over a
  scheme of the engine's own, through Mojo's data pipes: no base64, no JSON
  around binary data, many calls under way at once.
- **Chromium's libraries are yours.** 15 C libraries inside the engine,
  7,230 functions, exported under `zebkit_vendor_*`: SQLite, BoringSSL,
  zlib, Zstandard, Brotli, ICU, HarfBuzz, libxml2, libwebp, libjpeg-turbo,
  FFmpeg, dav1d, Opus, libvpx and PDFium. You do not ship them twice.
- **PDF built in.** A full API on Chromium's PDFium: rendering and tiles,
  text with layout, search, outline, links, annotations with appearances,
  forms, redaction, page editing, merging, attachments, metadata, and
  signatures checked for integrity. In C and in page JavaScript.
- **Your own URL schemes.** `app://` answered by the application, for page
  loads, `fetch()` and media alike, with files served in ranges so that a
  `<video>` seeks.
- **Native UI from JavaScript.** `zebkit.ui` draws windows with Chromium's
  own views toolkit (what Chrome's interface is made of): signals, JSX,
  Tailwind classes, and automation for tests and agents.
- **Made for agents.** CDP is Chromium's own DevTools server, exactly
  what Chrome serves, for Puppeteer, Playwright and raw clients. Headless
  or windowed, the same views.
- **Safe by default.** A page has no `zebkit` object unless the
  application grants it, per view, per origin and per capability; the
  origin is the browser's, never the page's word; the renderer stays
  sandboxed.
- **An engine you can change.** zebkit is Chromium plus a small patch set
  (`patches/`), rebuilt from source; where Chromium lacks something, zebkit
  patches it rather than working around it.

## Platforms

| | |
|---|---|
| Engine | Chromium 155.0.8059.84 (`upstream.lock`): content layer, Blink, V8 |
| Linux | x86-64, glibc 2.25 or newer: `libzebkit.so.1` and `zebkit-helper` |
| macOS | Apple silicon: `Zebkit.framework`, cross-built on Linux (`docs/MACOS.md`) |
| Windows | Planned, with the same API |
| Version | `VERSION`: 0.0.1-alpha.4 |

## Using zebkit in an application

A package is a directory with the library, the helper, the engine's data
and `include/`. Build one (`docs/BUILD.md`, then `out/blink/slim/package`)
or make a release with `mise run r` (`dist/release`). Below, `$PKG` is that
directory.

### C

```c
#include <stdio.h>
#include <zebkit/zebkit.h>

/* The page calls zebkit.app.greet("Ada"). */
static void greet(zebkit_view *view, const char *origin, const char *args_json,
                  zebkit_call *call, void *data) {
    char answer[256];
    snprintf(answer, sizeof answer, "{\"hello\": %s}", args_json);
    zebkit_call_resolve(call, answer);
}

static void closed(zebkit_view *view, void *data) { zebkit_quit(); }

static void ready(void *data) {
    zebkit_view *view = zebkit_view_new("app://main/index.html", 1024, 768);
    zebkit_view_grant(view, "app://main", ZEBKIT_ALLOW_APP | ZEBKIT_ALLOW_PDF, NULL);
    zebkit_view_bind(view, "greet", greet, NULL);
    zebkit_view_on_close(view, closed, NULL);
}

/* app://main/... is served from ./ui, in ranges for media. */
static void serve(zebkit_request *request, void *data) {
    char path[1024];
    snprintf(path, sizeof path, "ui%s", zebkit_request_url(request) + sizeof "app://main" - 1);
    zebkit_request_respond_file(request, path, NULL);
}

int main(int argc, char **argv) {
    zebkit_scheme_register("app", ZEBKIT_SCHEME_STANDARD | ZEBKIT_SCHEME_SECURE | ZEBKIT_SCHEME_FETCH);
    zebkit_scheme_handle("app", serve, NULL);
    return zebkit_main(argc, argv, ready, NULL);
}
```

```sh
cc app.c -I$PKG/include -L$PKG -l:libzebkit.so.1 -Wl,-rpath,$PKG -o app
./app --remote-debugging-port=9222     # and CDP for Puppeteer or Playwright
```

(A real handler checks the path for `..`; `examples/window.c` is a complete
browser window.)

### Rust

```toml
[dependencies]
zebkit = "0.0.1-alpha.4"

[build-dependencies]
zebkit-build = "0.0.1-alpha.4"
```

```rust
// build.rs
fn main() { zebkit_build::bundle(); }
```

```rust
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

`ZEBKIT_DIR` points the build at a package; `zebkit-build` puts the engine
beside the executable. Callbacks are closures, and the engine's thread is
enforced by the types (`rust/README.md`).

### Python

```python
import zebkit                      # bindings/python, on ctypes: nothing to compile

def ready():
    view = zebkit.View("https://example.com/")
    view.grant("https://example.com", zebkit.ALLOW_APP)
    view.bind("greet", lambda name, origin: f"hello, {name}")

zebkit.main(ready)                 # the engine is found in $ZEBKIT_DIR
```

### The page's JavaScript

A page the application has granted gets a global `zebkit`; every call is a
promise, and bytes are `Uint8Array`s:

```js
const reply = await zebkit.app.greet('Ada');            // the application's function
const pdf = await zebkit.pdf.open(await (await fetch('report.pdf')).bytes());
const rgba = await pdf.render(0, {scale: 2});           // a page as pixels
const hits = await pdf.search('revenue');
const packed = await zebkit.compress('zstd', bytes, 19);
const hash = await zebkit.crypto.digest('SHA-256', 'abc');
await zebkit.fs.writeFile('notes.txt', 'text');         // inside the directory granted
```

### An application in JavaScript, without Node

`zebkit-js` runs an ES module on the engine's V8, as Electron's main
process would run, with views, the same APIs, and no grants needed:

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

### Native windows: `zebkit.ui`

```js
// zebkit-js todo.mjs
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

### Agents over CDP

```sh
tools/zebkit-run --headless --remote-debugging-port=9222 https://example.com/
```

```js
const browser = await puppeteer.connect({browserURL: 'http://127.0.0.1:9222'});
```

In a container without unprivileged user namespaces, add `--no-sandbox`.

### Try it

```sh
mise run r                  # build the release package
mise example:linux:basic    # a browser window on it
mise example:rust:window    # the same in Rust
```

## Documentation

| Document | What it covers |
|---|---|
| [docs/API.md](docs/API.md) | Everything an application can call, in C, Rust, Python and JavaScript |
| [docs/BUILD.md](docs/BUILD.md) | Fetching Chromium, building, packaging, testing and measuring; profiles and patches |
| [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) | How the engine is made, and what in this repository is what |
| [docs/SIZE_AND_SPEED.md](docs/SIZE_AND_SPEED.md) | Where the size goes, what was cut and kept, the compiler against speed, the Electron baseline |
| [docs/MACOS.md](docs/MACOS.md) | The macOS framework, built on Linux |
| [docs/ROADMAP.md](docs/ROADMAP.md) | Requirements, what is done, what is next |
| [rust/README.md](rust/README.md) | The Rust crates |
| `docs/reports/` | Size reports, regenerated by the scripts |

## Repository

| Directory | What it holds |
|---|---|
| `upstream.lock` | The Chromium release built, the pinned depot_tools, the Electron release measured against |
| `profiles/` | Build flavours as GN arguments: `slim.gn` (the product), `slim-macos.gn`, `baseline.gn` (Chromium's defaults) |
| `patches/` | Changes to Chromium, applied in the order of `patches/series` |
| `src/engine/` | zebkit's own code, linked into Chromium as `//zebkit/engine`: the library, its C API and headers |
| `src/ffmpeg/` | zebkit's FFmpeg configuration, from `scripts/ffmpeg-config.sh` |
| `rust/`, `bindings/python/` | The Rust crates and the Python binding |
| `tools/` | `zebkit-run` (open pages, serve CDP) and `zebkit-js` (applications in JavaScript) |
| `tests/` | Programs that test every layer of the API against a package; speed workloads |
| `examples/` | A browser window in C, `zebkit.ui`, a library inside the engine, the Electron baseline |
| `scripts/` | Every step, one script each |

`upstream/` (Chromium's tree, about 30 GB), `out/` and `.cache/` are
generated and not tracked.
