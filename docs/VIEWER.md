# One page, every system: the viewer

An application's pages use the global `zebkit` (docs/API.md, "JavaScript in
the page"): `zebkit.app.<name>()` for the application's functions,
`zebkit.pdf`, `zebkit.compress`, `crypto`, `image`, `url`, `fs`, `fetch`. On
Linux, macOS and Windows a page has it in zebkit's own views. On Android and
iOS, where an application shows the web with the system's web view
(docs/MOBILE.md), zebkit's **viewer** gives the page the same object. One
page, and one set of calls, on all five.

| | Draws the page | Answers `zebkit.*` |
|---|---|---|
| Linux, macOS, Windows | zebkit's engine (`zebkit_view_*`) | the engine's bridge (bridge.cc, pdf_bridge.cc) |
| Android | Android's System WebView (Chromium) | the viewer: `dev.zebkit.ZebkitViewer` over libzebkit |
| iOS | WKWebView (WebKit) | the viewer: `ZebkitViewer` (Swift) over libzebkit |

Both answer with the same code (`src/engine/page_api.h`): the bridge and the
viewer only carry the calls. zebkit.pdf's wrapper in the page is the same
script on all of them (`pdf_script.h`).

## What a page has

| | zebkit's views | The viewer |
|---|---|---|
| `zebkit.app.<name>(...)` | `zebkit_view_bind()` | `viewer.bind(name, fn)` |
| `zebkit.pdf`, `zebkit.canvas`, `zebkit.text` | yes | yes (PDFium in libzebkit) |
| `zebkit.compress`, `decompress`, `crypto`, `image` | yes | yes |
| `zebkit.url.parse` | yes | yes |
| `zebkit.url.registrableDomain` | yes | no: rejected (the engine's list of public suffixes) |
| `zebkit.fs` | under the granted directory | the same |
| `zebkit.fetch` | the engine's network stack, its cookies | the system's HTTP (URLSession, HttpURLConnection), without the web view's cookies |
| `zebkit.pathForFile` | dropped files' paths | `''` |

Grants are the same: `grant(origin, allow, filesRoot)` per origin, `"*"` for
pages the application wrote itself; a page of another origin has the object
and every call it makes is refused. Documents a page opened close when it
is left.

## The application's side

On every system the application:

1. makes a view and **grants** origins what they may use,
2. **binds** the functions its pages call as `zebkit.app.<name>`,
3. serves **its own pages** (a scheme or a host of its own),
4. **loads** a page, and may **run** script in it.

| Step | zebkit (C; Rust `zebkit::View`) | Swift `ZebkitViewer` | Java `ZebkitViewer` |
|---|---|---|---|
| Make | `zebkit_view_new()` | `ZebkitViewer(configuration:)` | `new ZebkitViewer(webView)` |
| Grant | `zebkit_view_grant(view, origin, allow, root)` | `grant(origin:allow:filesRoot:)` | `grant(origin, allow, filesRoot)` |
| Bind | `zebkit_view_bind(view, name, fn, data)` | `bind(name) { args in ... }` | `bind(name, args -> ...)` |
| Own pages | `zebkit_scheme_register()` | the configuration's `WKURLSchemeHandler` | `setAppContent(host, source)` |
| Load | `zebkit_view_load_url(view, url)` | `webView.load(...)` | `loadUrl(url)` |
| Run script | `zebkit_view_evaluate()` | `webView.evaluateJavaScript()` | `evaluate(source, callback)` |

A bound function gets the page's arguments as JSON (bytes as `{"$bytes":
base64}`) and returns a JSON value, on every system.

What zebkit's engine has besides (network hooks, automation, DevTools,
printing, windows) stays on Linux, macOS and Windows; a page that uses
only `zebkit.*` runs everywhere.

## How the viewer works

`<zebkit/viewer.h>` is the native half, in libzebkit on every system:

- `zebkit_viewer_script(post)`: the page's side, added before each
  document's own script: the same `zebkit` object as in zebkit's views, over
  a `send()` that posts each call as JSON through `post` (a script message
  handler, a JavaScript interface).
- `zebkit_viewer_handle(viewer, origin, message, &app_call)`: one posted
  call, from a frame of `origin`. Returns the JavaScript that answers it,
  which the web view runs in that frame; or NULL for a call that is the
  application's (`zebkit.app`, `zebkit.fetch`), described in `app_call`.
- `zebkit_viewer_answer(id, ok, json)`: the JavaScript that answers one of
  those.
- `zebkit_viewer_grant()`, `zebkit_viewer_reset()` (a new document: the old
  one's PDFs close), `zebkit_viewer_new()`, `zebkit_viewer_free()`.

Calls may run on any thread, and the bindings run them off the main
thread: a PDF's page is drawn there.

Limits of the system's web views:

- **Android**: a plain `WebView` does not say which frame called, so a call
  is taken to be from the main frame's origin; and a page that is not the
  application's own gets `zebkit` as it starts loading, which its first
  script may run before. The application's own pages (`setAppContent()`)
  get it before any of their script.
- **iOS**: WKWebView says each call's frame and origin, and adds the script
  before every document's own.

## Building

The viewer is part of libzebkit (docs/MOBILE.md). `scripts/package-mobile.sh
slim-android` makes `zebkit.aar`; on a Mac, `scripts/package-mobile.sh
slim-ios` makes `Zebkit.xcframework` and puts it in `bindings/swift`, the
Swift package an iOS application adds. `zebkit_mobile_test` runs the
viewer's native half, as a web view would call it.
