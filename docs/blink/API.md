# zebkit's API on Blink

What an application gets from the engine: the plan is section 7 of
`docs/blink/ROADMAP.md`; this is what is built. The engine is
`libzebkit.so.1`, with `zebkit-helper` beside it for the renderer, GPU and
utility processes, and the headers in the package's `include/`.

```sh
cc app.c -I<package>/include -L<package> -l:libzebkit.so.1 -Wl,-rpath,<package>
```

| Layer | Header | What |
|---|---|---|
| 0. Core | `<zebkit/zebkit.h>` | Start the engine, run its loop, views, script, CDP, calls between the page and the application |
| 1. Libraries | `<zebkit/vendor/<library>.h>` | The C libraries inside the engine, with their own APIs |
| 2. Services | `<zebkit/services.h>` | Compression, cryptography, URLs, images, ICU's data, HTTP, over Chromium's own code |
| 3. Bindings | `bindings/python/zebkit` | Layers 0 and 2 for Python |
| 4. JavaScript | `zebkit.*` in a page | Layers 0 and 2 for the page's script, as the application grants it |

`scripts/engine-test.sh <profile>` tests every layer from programs built
against the package (`tests/engine/`).

## 0. The core

```c
#include <zebkit/zebkit.h>

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

- **One call starts and runs the engine.** `zebkit_main()` starts
  Chromium's content layer the way Android WebView does
  (`BrowserMainRunner` initialized by the embedder), calls `ready` on the
  main thread when views can be made, and returns after `zebkit_quit()`
  or when the last view closes. Chromium's switches in `argv` are
  honoured: `--remote-debugging-port`, `--user-data-dir`, `--no-sandbox`,
  `--headless`, `--enable-logging=stderr` and the rest.
- **Threads.** Everything is for the thread that called `zebkit_main()`,
  and every callback runs on it; `zebkit_post_task()` is for any thread.
- **Windows.** A view has a window when there is a display (Wayland or
  X11) and none otherwise; it loads, runs and can be driven over CDP
  either way.
- **Script.** `zebkit_view_evaluate()` runs script in the page and calls
  back with its value as JSON, a promise waited for, or with the message of
  what was thrown. It is Chrome's `Runtime.evaluate`, through an internal
  DevTools session of the view.
- **CDP.** `zebkit_cdp_listen()` serves the Chrome DevTools Protocol from
  Chromium's own DevTools server, which is what Chrome serves with
  `--remote-debugging-port`; `zebkit_cdp_get_url()` is the browser
  target's WebSocket address for Puppeteer and Playwright.
- **Data.** The profile is in `--user-data-dir`, by default
  `$XDG_CONFIG_HOME/zebkit/<program name>`.

## 1. The libraries inside the engine

The application includes the library's header and writes the library's own
API. Every function is exported from the engine as
`zebkit_vendor_<library>_<function>`, and the header maps the one name to
the other, so the engine exports none of the libraries' own names: another
copy of a library in the process, a system one or the application's, is a
different one and does no harm. In C++ the API is also in the namespace
`zebkit::vendor::<library>`.

```c
#include <zebkit/vendor/sqlite.h>
#include <zebkit/vendor/zstd.h>

sqlite3 *db;
sqlite3_open("app.db", &db);
size_t size = ZSTD_compress(out, capacity, data, length, 19);
```

| Library | Header | Functions | Notes |
|---|---|---:|---|
| zlib | `zlib.h` | 88 | Chromium's fork, with its SIMD |
| Zstandard | `zstd.h` | 68 | Compression and decompression |
| Brotli | `brotli.h` | 32 | Encoder and decoder |
| BoringSSL | `boringssl.h` | 2,960 | `libcrypto` and `libssl`; its C++ helpers are left out (`BORINGSSL_NO_CXX`) |
| SQLite | `sqlite.h` | 263 | Built as Chromium builds it: call `sqlite3_initialize()` first (no automatic initialization); exclusive locking by default (`PRAGMA locking_mode=NORMAL` to share a file); no JSON functions, no extensions |
| ICU | `icu.h` | 955 | The C API; call `zebkit_icu_init()` first when the engine is not running. Its data is English, Chinese, Japanese and Korean |
| HarfBuzz | `harfbuzz.h` | 484 | Shaping, OpenType tables, subsetting |
| libxml2 | `libxml.h` | 1,008 | XML and HTML parsing, the tree, XPath, the reader and writer |
| libwebp | `libwebp.h` | 105 | Decoding, encoding, demuxing, muxing |
| libjpeg-turbo | `libjpeg.h` | 63 | The libjpeg API |
| FFmpeg | `ffmpeg.h` | 534 | As Chromium builds it: demuxing and decoding of what web pages play; no encoders, no scaling |
| dav1d | `dav1d.h` | 19 | AV1 decoding |
| Opus | `opus.h` | 71 | Encoding and decoding |
| libvpx | `libvpx.h` | 36 | VP8 and VP9, decoding and encoding |

6,686 functions in all, and the engine exports nothing else but zebkit's
own API (`scripts/engine-test.sh` checks both). Only the headers the
library's header reaches are shipped, not the library's internals.

What is there is what the engine is built with: a function is exposed when
the library's public headers declare it and the library's compiled objects
define it. `src/engine/vendor/gen_vendor.py` finds them in the build, at
the end of it, and writes the entry points and the headers; nothing is
listed by hand. Chromium renames some libraries' functions (zlib's
`Cr_z_`, SQLite's `chrome_sqlite3_`, ICU's version suffix,
libjpeg-turbo's `chromium_`) with macros in their headers; the generator
follows them. FreeType is not there yet: its headers include one another
through macros, which the package's layout cannot follow.

## 2. Services

`<zebkit/services.h>`: plain functions over Chromium's own code, for what
is not one library's API.

| Functions | What | From |
|---|---|---|
| `zebkit_compress`, `zebkit_decompress` | gzip, zlib, raw deflate, Brotli, Zstandard; a limit against data that expands without end | zlib, Brotli, Zstandard |
| `zebkit_digest_compute`, `zebkit_hmac`, `zebkit_random_bytes`, `zebkit_aes_gcm_seal`, `zebkit_aes_gcm_open` | SHA-1 and SHA-2, HMAC, secure random bytes, AES-GCM | BoringSSL |
| `zebkit_url_get`, `zebkit_url_registrable_domain` | A URL parsed and canonicalized as a browser does, its parts and origin; the registrable domain by the public suffix list | `url/`, `net/` |
| `zebkit_image_decode`, `zebkit_image_encode` | PNG, JPEG and WebP to and from RGBA | `ui/gfx/codec`, libwebp |
| `zebkit_icu_init` | ICU's data, for layer 1's ICU without the engine | `base/i18n` |
| `zebkit_fetch` | An HTTP request through the engine's network stack: HTTP/1.1, 2 and 3, its cache, cookies and proxy settings, without CORS | The network service |

All but `zebkit_fetch()` work without the engine running.

## 3. Bindings

`bindings/python/zebkit` is the core and the services for Python, through
`ctypes`, with nothing to compile:

```python
import zebkit

def ready():
    view = zebkit.View("https://example.com/")
    view.grant("https://example.com", zebkit.ALLOW_APP)
    view.bind("greet", lambda name, origin: f"hello, {name}")

zebkit.main(ready)
```

It finds the engine in `$ZEBKIT_DIR`. Rust (the existing crates, written
for the WebKit engine) is next.

## 4. JavaScript in the page

A page the application grants it to has a global `zebkit`:

```js
const sum = await zebkit.app.add(2, 3);               // the application's function
const response = await zebkit.fetch('https://api.example/x', {method: 'POST', body: '{}'});
await zebkit.fs.writeFile('notes.txt', 'text');      // under the directory granted
const packed = await zebkit.compress('zstd', bytes, 19);
const hash = await zebkit.crypto.digest('SHA-256', 'abc');
const {width, height, data} = await zebkit.image.decode(pngBytes);
const url = await zebkit.url.parse('../x', location.href);
```

| Object | Functions |
|---|---|
| `zebkit.app.<name>(...args)` | What the application binds with `zebkit_view_bind()`; arguments and result as JSON |
| `zebkit.fetch(url, {method, headers, body})` | `{status, url, headers, body}`, `body` a `Uint8Array` |
| `zebkit.fs` | `readFile`, `readText`, `writeFile`, `readDir`, `stat`, `mkdir`, `remove(path, {recursive})` |
| `zebkit.compress(format, bytes, level)`, `zebkit.decompress(format, bytes)` | `gzip`, `zlib`, `deflate-raw`, `br`, `zstd` |
| `zebkit.crypto` | `digest(algorithm, data)`, `hmac(algorithm, key, data)`, `randomBytes(count)` |
| `zebkit.image` | `decode(bytes)`, `encode('png' \| 'jpeg' \| 'webp', {width, height, data}, quality)` |
| `zebkit.url` | `parse(url, base)`, `registrableDomain(host)` |

Every function returns a promise; bytes are `Uint8Array`s, and a string is
taken as its UTF-8 where bytes are expected.

**What a page may use is the application's to say**, per view, per origin
and per capability: `zebkit_view_grant(view, "https://app.example",
ZEBKIT_ALLOW_APP | ZEBKIT_ALLOW_FILES, "/path/to/data")`. A view with no
grant gives its pages no `zebkit` at all; in a view with grants, a page of
another origin has the object, and every call it makes is refused.

- **The origin is the browser's**, the committed origin of the frame the
  call came from, not anything the page's script says.
- **Frames of another site never get the object**: they are other
  DevTools targets, which the view's session does not attach to.
- **`zebkit.fs` stays in its directory**: a path with `..`, a symbolic
  link, or one that resolves outside is refused.
- **Isolated worlds are not heard**; only the page's own main world.

How it works: the view's internal DevTools session adds a binding
(`Runtime.addBinding`, as Playwright's `exposeFunction` does) and a script
that runs before the page's own (`Page.addScriptToEvaluateOnNewDocument`);
the script makes the `zebkit` object, each call goes to the browser
process as JSON, and the answer comes back by `Runtime.evaluate` in the
calling context. The renderer stays sandboxed; the work is done in the
browser process, files and compression on Chromium's thread pool. Bytes
travel as base64, which is fine for what a page usually moves and slow for
hundreds of megabytes; a Mojo channel is the way to go faster, if it is
needed.

## Loading the library

An application is linked with `libzebkit.so.1`, or loads it with `dlopen()`
(Python's `ctypes` does). The engine applies its own packed relocations
when it is loaded (`patches/1005`), and thread-local data is copied by the
loader for the threads that exist before that: a thread that existed
before a `dlopen()` of the engine keeps a stale copy of one pointer of
Chromium's `base`. Link the engine, or load it before starting threads.

Every child process is `zebkit-helper`, beside the library, whatever the
application's executable is.

## Size and speed

Measured on Linux x86-64 (`scripts/measure.sh`, `scripts/bench.sh`):

| | Content shell, before | Library, now |
|---|---:|---:|
| Package | 134.8 MiB | 142.5 MiB |
| As `.tar.xz` | 43.2 MiB | 44.6 MiB |
| The engine's binary | 122.8 MiB | 123.6 MiB |
| Headers (`include/`) | none | 7.6 MiB |

The library is 0.8 MiB larger than the content shell for all of the
above: the libraries were inside it already, and exporting them keeps
only what the engine had left out of them. The rest of the growth is the
libraries' headers. Speed is the same within the spread of
`scripts/bench.sh`, although PartitionAlloc no longer takes `malloc` over
and V8 and Blink's heap use the thread-local model of a library.
