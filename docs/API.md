# zebkit's API

What an application gets from the engine. How it works inside is
`docs/ARCHITECTURE.md`; what is still to come, `docs/ROADMAP.md`. The engine is
`libzebkit.so.1` on Linux, `Zebkit.framework` on macOS and `zebkit.dll` on
Windows; on Linux and macOS a helper beside it (`zebkit-helper`, `Zebkit
Helper.app`) is the renderer, GPU and utility processes, and on Windows the
application's own executable is ([WINDOWS.md](WINDOWS.md#the-sandbox)). The
headers are in the package's `include/`.

```sh
cc app.c -I<package>/include -L<package> -l:libzebkit.so.1 -Wl,-rpath,<package>   # Linux
cc app.c -I<package>/include -F<package> -framework Zebkit -Wl,-rpath,<package>   # macOS
clang-cl /MT app.c /I<package>\include <package>\zebkit.dll.lib <package>\zebkit_sandbox.lib   # Windows
```

| Layer | Header | What |
|---|---|---|
| 0. Core | `<zebkit/zebkit.h>`, `<zebkit/window.h>`, `<zebkit/hooks.h>`, `<zebkit/offscreen.h>`, `<zebkit/page.h>`, `<zebkit/automation.h>`, `<zebkit/win_sandbox.h>` | Start the engine, run its loop, views and their windows, script in pages and outside them, native interface (`zebkit.ui`), CDP, calls between the page and the application; the hooks; views drawn off-screen into shared surfaces; dialogs, printing to PDF, drag and drop; Playwright's API; Windows' sandbox |
| 1. Libraries | `<zebkit/vendor/<library>.h>` | The C libraries inside the engine, with their own APIs |
| 2. Services | `<zebkit/services.h>`, `<zebkit/pdf.h>`, `<zebkit/canvas.h>`, `<zebkit/text.h>`, `<zebkit/media.h>`, `<zebkit/media_tap.h>`, `<zebkit/media_tools.h>` | Compression, cryptography, URLs, images, ICU's data, HTTP, PDF, 2D drawing and shaping text, media, over Chromium's own code |
| 3. Bindings | `bindings/rust/`, `bindings/python/zebkit`, `bindings/golang/`, `tools/zebkit-js` | All of the C API for Rust, Python, Go and host JavaScript (`scripts/check-bindings.py` checks it) |
| 4. JavaScript | `zebkit.*` in a page | Layers 0 and 2 for the page's script, as the application grants it |

`scripts/engine-test.sh <profile>` tests every layer from programs built
against the package (`tests/engine/`).

Persistent cookies are kept encrypted in the profile, with one of three keys,
chosen on the main thread before `zebkit_main()`:

- **The system's key store**, the default (`ZEBKIT_COOKIE_STORAGE_OS`): the
  Keychain on macOS, the desktop's keyring over D-Bus on Linux, a key the
  user's DPAPI protects on Windows; cookies in `Network/Cookies`.
- **A private key file of the engine's**:
  `zebkit_set_cookie_storage(ZEBKIT_COOKIE_STORAGE_FILE, NULL)`, or
  `--zebkit-cookie-storage=file` (and `--zebkit-cookie-key-directory=DIR`). The key
  is `<user-data-dir>/Secrets/cookie.key` unless an absolute directory is given,
  made on the first start, private to its owner (links and other users' access
  refused); cookies in `Network/Cookies.host`. It does not reach the system's
  key store, and protects only as well as the file's permissions: back up the
  key and the profile together.
- **The application's own 32-byte key**: `zebkit_set_cookie_encryption_key()`,
  which the application keeps; also `Network/Cookies.host`.

A key that is missing or invalid for an existing `Cookies.host` fails the start
(exit code 1); the engine never makes a new key over it. Switching between them
does not merge the stores, so a switch can mean signing in again. Session
cookies stay session-only, and in-memory contexts keep no cookies on disk. How
this was found and checked: [notes/cookie-persistence.md](notes/cookie-persistence.md).

## Contents

- [0. The core](#0-the-core)
- [The window: <zebkit/window.h>](#the-window-zebkitwindowh)
  - [Borrowing a view's system window](#borrowing-a-views-system-window)
- [The desktop, the system and the page's choosers](#the-desktop-the-system-and-the-pages-choosers)
- [Runtime view preferences](#runtime-view-preferences)
- [Runtime settings of the engine and of a view](#runtime-settings-of-the-engine-and-of-a-view)
- [Runtime document permissions](#runtime-document-permissions)
- [Runtime document navigation policy](#runtime-document-navigation-policy)
- [Views: popups, console, certificates, user agent, sign-in](#views-popups-console-certificates-user-agent-sign-in)
  - [Adopting a site's authorization popup](#adopting-a-sites-authorization-popup)
  - [New-window decisions and creation metadata](#new-window-decisions-and-creation-metadata)
  - [Native document-console events](#native-document-console-events)
  - [Document certificate errors](#document-certificate-errors)
  - [Atomic user agent and client hints](#atomic-user-agent-and-client-hints)
  - [HTTP authentication](#http-authentication)
  - [What a page asks of its window, and its network](#what-a-page-asks-of-its-window-and-its-network)
- [1. The libraries inside the engine](#1-the-libraries-inside-the-engine)
- [2. Services](#2-services)
  - [PDF](#pdf)
  - [A PDF in a page](#a-pdf-in-a-page)
  - [Annotations](#annotations)
  - [Hooks](#hooks)
  - [Canvas and text](#canvas-and-text)
- [3. Bindings](#3-bindings)
- [4. JavaScript in the page](#4-javascript-in-the-page)
- [Script outside any page](#script-outside-any-page)
- [Native interface: zebkit.ui](#native-interface-zebkitui)
- [Off-screen views](#off-screen-views)
  - [Another process](#another-process)
  - [In a host window](#in-a-host-window)
- [Dialogs, PDF, drag and drop, find in page](#dialogs-pdf-drag-and-drop-find-in-page)
- [Media: what a page plays, saved as it plays](#media-what-a-page-plays-saved-as-it-plays)
  - [Native media processing tools](#native-media-processing-tools)
- [Automation: a screenshot's pixels](#automation-a-screenshots-pixels)
- [Loading the library](#loading-the-library)
- [Switches that are not an application's](#switches-that-are-not-an-applications)

## 0. The core

Applications that own their event loop can use `zebkit_init(argc, argv)`
instead of `zebkit_main()`. A zero return means views can be created on
that thread. Register `zebkit_set_wakeup(callback, data)` before init for
event-driven integration. The callback may run on any engine thread, even
during initialization: it must only queue a host event or timer, never call
back into the engine or block. Preserve the earliest requested deadline;
consume it before calling `zebkit_pump()` on the initializing thread. The
pump processes ready engine and native events without waiting. A host may
instead pump periodically, while continuing its normal platform event loop.

Call `zebkit_shutdown()` outside engine callbacks to close views and stop
the engine while the application's loop continues. Shutdown waits for any
wakeup callback in progress and prevents later ones. Both pump and shutdown
reject another thread or recursive calls. Initialization is once per process,
including after shutdown; the two entry points cannot be mixed. The
Rust, Python and Go bindings provide lifecycle and wakeup functions; the Rust
`embedded` example and Python lifecycle test use deadline-based host loops.
`examples/native-host` has hosts in GTK, Qt, AppKit and winit.

`zebkit_init_with_settings(argc, argv, json, &error)` validates startup
settings before initialization or file IO. Unknown fields, invalid types
and relative storage/log paths return an allocated error to free with
`zebkit_free()`; validation failure permits a corrected retry. Settings
replace corresponding command-line switches; other switches remain available.
Python accepts `init(argv, settings={...})`; Rust provides
`init_with_settings(args, json)`.

| Setting | Value |
|---|---|
| `profileDirectory` | Absolute profile path |
| `proxy` | Chromium proxy-server syntax, for example `http://127.0.0.1:8080` |
| `userAgent` | User agent string |
| `headless`, `sandbox` | Boolean |
| `gpu` | `auto`, `disabled`, `software` |
| `logging` | Object: `destination` (`stderr`, `file`, `none`), `path` (required absolute path for `file`), optional `level` (integer 0–3) |
| `cookies` | Object: `storage` (`os`, `file`), optional absolute `keyDirectory` for file storage, `thirdParty` boolean |
| `applicationIntegration` | Boolean: opt into zebkit's macOS delegate, menu and Dock policy |
| `httpCache` | Object: `storage` (`disk`, the default for a profile on disk, as Chrome's: `<profile>/Cache`; `memory`; `off`), `maxSize` (bytes, 0 or absent: Chromium's choice from the free space). Contexts keep theirs in memory ([Connection reuse](#connection-reuse)) |
| `preconnect` | List of `http(s)` URLs: a connection to each origin, for the engine's profile, as the engine starts |

The embedded entry skips Chromium's process-wide signal-handler, locale,
thread-name, process-title, crash-handler and environment setup. DBus session
connections require an explicit address rather than altering the host's
environment to prevent autolaunch. File logging appends and child processes
inherit its destination and path. macOS application integration is opt-in;
platform runtime validation remains separate from cross-compilation.

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
  main thread when views can be made, and returns after `zebkit_quit()`.
  Closing the last window does not end it: that is the application's to
  act on. Chromium's switches in `argv` are
  honoured: `--remote-debugging-port`, `--user-data-dir`, `--no-sandbox`,
  `--headless`, `--enable-logging=stderr` and the rest.
- **Lighter than a browser by default.** Third-party cookies are blocked,
  as Safari and Firefox block them: on pages with advertising each network
  that would sync a cookie opens a frame, and with site isolation a
  process (one news site: 63 such frames, 3 without).
  `--zebkit-allow-third-party-cookies` allows them. No spare renderer is
  kept warm for the next navigation, as Electron keeps none;
  `--zebkit-spare-renderer` keeps one. The user agent and client hints are
  Chromium's own for its version, with `--headless` too: Chrome's
  `Chrome/157.0.0.0`, not `HeadlessChrome`, since headless views are shown
  to people and sites take HeadlessChrome for a bot.
  `--zebkit-headless-user-agent` gives Chrome's headless string;
  `--user-agent` replaces it.
- **What sites read to know it is Chrome.** `window.chrome` has
  `chrome.loadTimes()` and `chrome.csi()` from Chrome's renderer, plus the
  legacy `chrome.app` hosted-app queries. Without a hosted-app registry,
  these report `false`, `null`, `cannot_run` and asynchronously
  `not_installed`; no extension runtime is linked. Popups are blocked as
  Chrome blocks them: a page opens a window or a tab only
  while the person clicks or types; `--disable-popup-blocking` lets every
  one open. `--window-size=W,H` is the window's size, as in Chrome. A
  headless view's screen is that size too, or 1920 by 1080, not Ozone's
  1 by 1 pixel. `navigator.webdriver` is true with
  `--enable-automation`, `--remote-debugging-pipe` or
  `--remote-debugging-port=0`, false otherwise. In zebkit,
  `--headless` selects rendering without native windows; it is not
  forwarded to the renderer as an automation switch. Serving CDP through
  `zebkit_cdp_listen()` alone does not enable automation mode either.
- **A CDP client changes nothing the page sees.** Describing the errors a
  page logs runs none of the page's code (`patches/1026`): Chrome's V8
  calls the page's `Error.prepareStackTrace` and getters on an error when a
  client has `Runtime` enabled, which pages and bot checks read as a
  debugger attached. The client still has each error with its stack
  (`tests/engine/cdp_browser.py`).
- **Google sign-in compatibility.** In the tested slim C host, restoring
  the legacy `chrome.app` queries changed email/Next from an unsafe-browser
  rejection to the password page. Enabling, disabling, then re-enabling
  only this API in the same binary produced pass, rejection (`rrk=46`),
  pass, with fresh profiles on 2026-10-26. This is a reproducible
  compatibility result, not a published Google login requirement or
  official support commitment. Password/MFA and full login were not tested.
  See [the source references and comparison](notes/google-signin.md).
- **A site's "Sign in with ..." popup.** Sites sign in with Google and
  other providers through a popup, as in Firefox: FedCM, which Chrome
  answers with dialogs of its own and the engine has none for, is off
  (`--zebkit-fedcm` keeps it). The popup opens from the person's click; an
  application that shows it itself takes it with
  `zebkit_view_on_window_open()`, a view still linked to the site
  (`window.opener`, `postMessage()`, `window.closed`), off-screen when the
  site's view is, its frames to draw for the person. Whether the provider
  accepts the sign-in in it is the provider's decision (the compatibility check above);
  once signed in, the session is the profile's (`--user-data-dir`), and a
  headless run with the same profile has it.
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
  `$XDG_CONFIG_HOME/zebkit/<program name>`
  (`~/Library/Application Support/zebkit/<program name>` on macOS). The
  log goes to stderr. With no home directory to keep a profile in, the
  engine uses a private temporary one, deleted as it stops; a
  `--user-data-dir` that cannot be made fails the start (exit code 1).
  Persistent cookies are encrypted in the profile and survive clean
  restarts, with the key chosen above, under the layers; with the default
  the system's key store must be available. Use the same profile path after
  restarting.
- **Passkeys.** Chromium FIDO requests expose a shared host event/action
  interface: `zebkit_view_on_authentication()` and
  `zebkit_view_authentication_action()`. Hosts render their own UI, with
  default desktop presentation when no handler is installed. Off-screen
  views opt in with `zebkit_view_set_authentication_ui()` and maintain focus.
  OS platform credentials depend on platform support and permissions;
  conditional autofill remains unavailable. The events' JSON (version 1) and
  the actions are in [the passkeys notes](notes/passkeys.md#event-schema-version-1).
- **URL schemes of the application's own.** `zebkit_scheme_register()`,
  before `zebkit_main()`, makes `app:` (say) a scheme of the engine's in
  every process: standard (an origin, relative URLs), secure, open to
  `fetch()`, past pages' Content-Security-Policy, as its flags say.
  `zebkit_scheme_handle()` answers its requests, page loads, subresources,
  `fetch()` and media alike: `zebkit_request_respond()` with a status,
  headers and a body, from any thread, or `zebkit_request_respond_file()`
  with a file read as it is sent, in the range a `Range` header asks for, as
  a `<video>` seeking asks. This is Chromium's way for its own schemes (a
  URL loader factory per scheme in the browser process), and Electron's
  `protocol.handle`. The same from the command line:
  `--zebkit-scheme=app,standard,secure,fetch`.
- **The application.** `zebkit_app_set_menu()` takes the application's
  menu as JSON: items with an id, which the application's function gets,
  standard roles (undo, copy, minimize, quit, ...), submenus and
  separators, each with a key. On macOS it is the menu bar (a first
  `appMenu` menu is the application menu); on Linux and Windows, whose
  windows have no menu bar, the same keys, with Ctrl for ⌘, work in the
  application's windows when the page does not take them.
  `zebkit_app_on_open_files()` gets the files the system opens with the
  application (the Finder, the Dock), `zebkit_app_on_reopen()` the Dock
  icon clicked with no window shown, and `zebkit_app_on_quit_request()`
  ⌘Q and logging out, to quit (`zebkit_quit()`) or not. In Rust
  `zebkit::app`; in Python `zebkit.app_*`.

Every function of `<zebkit/zebkit.h>`, whose comments are the full
reference:

| Functions | What |
|---|---|
| `zebkit_main`, `zebkit_quit`, `zebkit_post_task`, `zebkit_post_delayed_task`, `zebkit_is_headless` | The engine, its loop, its thread |
| `zebkit_version`, `zebkit_engine_version`, `zebkit_free` | Versions; freeing what the engine returns |
| `zebkit_app_set_menu`, `zebkit_app_on_open_files`, `zebkit_app_on_reopen`, `zebkit_app_on_quit_request` | The application's menu and its part in the system |
| `zebkit_view_new`, `_close`, `_load_url`, `_reload`, `_stop`, `_go_back`, `_go_forward`, `_is_loading`, `_get_url`, `_get_title` | A view and its page |
| `zebkit_view_resize`, `_set_size_limits`, `_set_title`, `_focus` | Its window: size, smallest size and whether it may be resized (Electron's `minWidth`, `minHeight`, `resizable`), title, raising |
| `zebkit_view_attach`, `_set_bounds`, `_set_visible` | A view inside another view's window, a browser in an application |
| `zebkit_view_attach_native`, `_detach_native` | A view's window inside a window of the application's: an X11 window id, or an `NSView*` on macOS; `_set_bounds`, `_set_visible`, `_resize` and `_focus` then act there. Refused, naming off-screen views, on Wayland and under `--headless` |
| `zebkit_view_get_native_window` | Borrow the system window containing the page: platform kind plus X11 ID, HWND or NSWindow*. After attachment, the top-level window containing the host. False and NONE/0 for headless/off-screen/Wayland or invalid calls; engine thread only. Never destroy/release the handle; query again after attach/detach |
| `zebkit_view_export_native_window` | The window as the system's portals name a parent window, asked for asynchronously: `"x11:<XID>"` or `"wayland:<handle>"` (xdg-foreign), so that a portal's dialog is the window's own; the callback, `(view, handle, data)`, once, after the call, NULL when there is none (always on Windows and macOS) or the view closed first |
| `zebkit_view_on_load`, `_on_title`, `_on_close` | Loads (started, committed, finished, failed), title changes, closing |
| `zebkit_view_on_process_gone` | The page's process went, as Electron's `render-process-gone`: `{"reason", "exitCode"}`, the reason `crashed`, `killed`, `oom`, `abnormal-exit`, `clean-exit`, `launch-failed`, `integrity-failure` or `memory-eviction`; the view stays open, a reload starts a process again |
| `zebkit_view_on_responsiveness` | The page stopped answering input and answers again, as Electron's `unresponsive` and `responsive`: `{"responsive": false}`, then `true`. Chromium's hang monitor: after 15 s of input unanswered, while the view is shown and no debugger (a CDP client, a Playwright client's CDP session) is attached; automation (`zebkit_automation_*()`, the Playwright endpoint's own commands) is not a debugger, and a page it drives is told |
| `zebkit_view_on_new_window` | What a page opens in a new window, to the application as a URL |
| `zebkit_view_on_window_open` | A window a page opens, to the application as a view still linked to its opener (a site's sign-in popup); off-screen when its opener is |
| `zebkit_view_on_drop` | Paths of files dropped on the view; `zebkit.pathForFile()` in the page |
| `zebkit_view_evaluate`, `_add_script` | Script in the page; script in every document before the page's own (a preload) |
| `zebkit_view_set_user_agent`, `zebkit_view_capture` | The User-Agent; the view as PNG, with or without a window |
| `zebkit_view_grant`, `_bind`, `zebkit_call_resolve`, `_resolve_json`, `zebkit_call_reject`, `zebkit_call_free` | What a page may use of `zebkit.*`; the application's functions it may call (`_resolve_json` takes JSON with its length, a part of a larger string as it is; `zebkit_call_free` drops a call the application will not answer, rejecting the page's promise) |
| `zebkit_script_new`, `_run_module`, `_evaluate`, `_on_idle`, `_destroy` | Script outside any page |
| `zebkit_cdp_listen`, `zebkit_cdp_listen_async`, `_get_port`, `_get_url`, `_stop` | The DevTools endpoint; `_async` does not wait for the server's thread and gives the port and the browser target's address. Asking for another address or port while it runs is an error |
| `zebkit_scheme_register`, `_handle`, `zebkit_request_url`, `_method`, `_headers`, `_body`, `zebkit_request_respond`, `_respond_file` | URL schemes of the application's own |

## The window: `<zebkit/window.h>`

A view's window made with options before the system's window exists, its
frame, state, actions, events and the regions it is dragged from. The design
and its reasons are `docs/SYSTEM_INTEGRATION.md`, section 7; `examples/custom-titlebar/`
is a page in the title bar.

| C | Python | Rust | host JavaScript |
|---|---|---|---|
| `zebkit_view_new_with_options(context, url, options_json, &error)` | `View(url, w, h, options={...})` | `View::new_with_options(url, json)` | `new zebkit.View(url, {width, height, window: {...}})` |
| `zebkit_view_get_window(view)` | `view.window` | `view.window()` | `view.window` |
| `zebkit_view_set_window(view, json, &error)` | `view.set_window({...})` | `view.set_window(json)` | `view.setWindow({...})` |
| `zebkit_view_window_action(view, action, &error)` | `view.window_action(name)` | `view.window_action(name)` | `view.windowAction(name)` |
| `zebkit_view_on_window_state(view, func, data)` | `view.on_window_state(fn)` | `view.on_window_state(fn)` | `view.on('window-state', fn)` |
| `zebkit_view_set_drag_regions(view, json, &error)` | `view.set_drag_regions([...])` | `view.set_drag_regions(json)` | `view.setDragRegions([...])` |
| `zebkit_view_set_popup_window_options(view, json, &error)` | `view.set_popup_window_options({...})` | `view.set_popup_window_options(Some(json))` | `view.setPopupWindowOptions({...})` |
| `zebkit_get_capabilities()` | `zebkit.capabilities()` | `zebkit::capabilities()` | `zebkit.capabilities()` |

The options, all optional; those marked *creation* are fixed once the window
is made, and changing them later fails with `requires-recreate`:

| Option | Default | Meaning |
|---|---|---|
| `width`, `height` *creation* | the engine's size | the content area, in DIPs |
| `frame` *creation* | `"system"` | `"system"`; `"overlay"`: the page from the window's top with the window's buttons over it, as an installed web app's Window Controls Overlay (`navigator.windowControlsOverlay`, `env(titlebar-area-x/-y/-width/-height)`, `@media (display-mode: window-controls-overlay)`); `"none"`: no title bar and no buttons, resize edges kept |
| `fallback` *creation* | `"error"` | `"system"`: where the system cannot give the frame, its own frame; the state says which |
| `show` *creation* | `true` | `false`: made hidden and not activated, until the `show` action |
| `minWidth`, `minHeight`, `resizable` | none, `true` | as `zebkit_view_set_size_limits()` |
| `x`, `y` | the system's place | the window's top left on the screen; refused on Wayland, where the compositor places windows |
| `title` | the page's | the window's title; `null` for the page's again |
| `controls` | the system's | `{height, color, symbolColor}`: the title bar area's height in an overlay, and the colours of the buttons the engine draws on Windows and Linux (macOS draws its own; its taller title bars are the system's unified toolbar styles) |
| `background` | the toolkit's | `"#rrggbb"`: the window before the page's first frame, and under a transparent page |
| `appearance` | `"system"` | `"light"`, `"dark"`: the frame's look, not the page's `prefers-color-scheme` |
| `pageFullscreen` | `"content"` | `"window"`: a page's `requestFullscreen()` makes the window fullscreen, as in Chrome |
| `dragRegions` | none | `{source: "css", origins: [...]}`: the page's `app-region: drag` and `no-drag`, while its main frame is of one of those origins (iframes never count, a document of another origin has none); `{source: "host"}`: `zebkit_view_set_drag_regions()` |

The state, from `zebkit_view_get_window()` and with every event: `revision`,
`owner` (`engine`; `view` or `application` for a view in another's window,
whose actions are refused; `none` without a window), `state` (`normal`,
`minimized`, `maximized`, `fullscreen`), `pageFullscreen`, `visible`,
`focused`, `bounds` (screen DIPs, with the frame), `content`, `scale`,
`frame` `{requested, effective}`, `titlebarArea` (what the page has as
`env(titlebar-area-*)`), `controls` (the buttons' rectangles over the page,
content DIPs), `capabilities`, `options` `{explicit}`. An action's `true`
means asked; the state event says what the system did. A press in a drag
region is the system's drag (snapping, the system menu on Windows, moving
on Wayland from the press itself); a double click there does what the
system's title bar does.

### Borrowing a view's system window

`zebkit_view_get_native_window(view, &window)` returns a borrowed
`zebkit_native_window {kind, handle}`. AppKit's handle is an `NSWindow*`,
not the `NSView*` accepted by `zebkit_view_attach_native`. Windows returns
an HWND; X11 returns a window ID. It is always a top-level window, the one a
dialog can be made transient for: after attachment, the top-level window that
contains the application's parent (on X11 the client window its window manager
manages, not the manager's frame). The handle is not owned or retained and becomes invalid when its
window closes. Do not destroy or release it; query again after attachment or
detachment. Calls must run on the engine thread.

Rust exposes `View::native_window() -> Option<NativeWindow>` with `X11`,
`Win32` and `AppKit` variants. Python's `View.native_window()` returns a dict
with `kind` (`x11`, `win32`, `appkit`) and integer `handle`, or `None` when
unavailable. Host JavaScript's `view.nativeWindow` returns the same fields,
with the handle as a BigInt (convert explicitly when serializing JSON), or
`null`. This trusted-host API is not exposed through a page bridge.

Headless and off-screen views return no system window, and neither does
Wayland, which has no global window ids.

`zebkit_view_export_native_window(view, callback, data)` names the window as
the XDG Desktop Portal's `parent_window` does, so that a portal's dialog (a
file chooser, a screen share) is the window's own: `"x11:<XID>"` of the same
top-level window, or on Wayland `"wayland:<handle>"`, the toplevel exported
through xdg-foreign, as Chrome's own portal dialogs ask for it. Wayland answers
asynchronously, so the callback runs once, on the engine thread, after the call
returns, also when the request is dropped; its string is the callback's to copy,
and NULL means there is none (headless, off-screen, a compositor without
xdg-foreign, the view closed; always on Windows and macOS, whose handle is the
one above). A view that closes first is told NULL before its `on_close`, the
view still valid, as every callback the engine owes a view. The callback is
`(zebkit_view *view, const char *handle, void *data)`; before 0.5.0 it was
`(void *data, const char *handle)`. Rust has `View::export_native_window(|handle| ...)`, Python
`View.export_native_window(function)`, host JavaScript a promise,
`await view.exportNativeWindow()`.

zebkit's windows on Linux are X11's unless the application passes
`--ozone-platform=wayland`. On Wayland, off-screen views beside windows are not
done yet: `zebkit_view_new_offscreen()` returns NULL there (under `--headless`
they work). A C query returns false and clears a supplied output to
NONE/0 when unavailable, for a NULL view or on the wrong thread. A NULL output
is rejected. A stale, already-freed C view pointer remains invalid, as with the
other C view APIs. Python and JavaScript keep their normal closed-view checks.

## The desktop, the system and the page's choosers

What the engine does for the application on the person's desktop, and the
page's requests that only the application answers. The design and the
defaults are `docs/SYSTEM_INTEGRATION.md`, sections 8 to 14; each surface's
status is in its section 5. Engine thread only.

| C | Python | Rust | host JavaScript |
|---|---|---|---|
| `zebkit_app_update_menu_item(id, json, &error)`, `zebkit_app_get_menu()` | `zebkit.app_update_menu_item(id, enabled=..., checked=..., title=...)`, `zebkit.app_menu()` | `app::update_menu_item(id, json)`, `app::menu()` | `zebkit.app.updateMenuItem(id, {...})`, `zebkit.app.menu()` |
| `zebkit_app_set_badge(text)`, `zebkit_app_set_progress(p)`, `zebkit_app_request_attention(critical)` | `zebkit.app_set_badge(text)`, `app_set_progress(p)`, `app_request_attention()` | `app::set_badge`, `app::set_progress`, `app::request_attention` | `zebkit.app.setBadge`, `setProgress`, `requestAttention` |
| `zebkit_app_set_dock_menu(json, func, data, &error)` (macOS) | `zebkit.app_set_dock_menu(items, fn)` | `app::set_dock_menu(json, fn)` | `zebkit.app.setDockMenu(items, fn)` |
| `zebkit_tray_new(png, size, tooltip, &error)` and `_set_icon`, `_set_tooltip`, `_set_menu`, `_on_click`, `_close` | `zebkit.Tray(png, tooltip)` | `Tray::new(png, tooltip)` | `new zebkit.Tray(png, tooltip)` |
| `zebkit_on_notification(func, data)`, `zebkit_notification_clicked(id, action)`, `zebkit_notification_closed(id, by_user)` | `zebkit.on_notification(fn)`, `notification_clicked`, `notification_closed` | `system::on_notification`, ... | `zebkit.onNotification(fn)`, `notificationClicked`, `notificationClosed` |
| `zebkit_open_external(url, &error)`, `zebkit_show_in_folder(path, &error)` | `zebkit.open_external(url)`, `show_in_folder(path)` | `system::open_external`, `system::show_in_folder` | `zebkit.openExternal(url)`, `showInFolder(path)` |
| `zebkit_open_external_async(url, func, data)`, `zebkit_show_in_folder_async(path, func, data)` | `zebkit.open_external_async(url, fn)`, `show_in_folder_async(path, fn)` | `system::open_external_async`, `system::show_in_folder_async` | `await zebkit.openExternalAsync(url)`, `showInFolderAsync(path)` |
| `zebkit_get_displays()`, `zebkit_on_displays_changed(func, data)` | `zebkit.displays()`, `on_displays_changed(fn)` | `system::displays()`, `system::on_displays_changed` | `zebkit.displays()`, `onDisplaysChanged(fn)` |
| `zebkit_on_power_event(func, data)` | `zebkit.on_power_event(fn)` | `system::on_power_event` | `zebkit.onPowerEvent(fn)` |
| `zebkit_prevent_sleep(keep_display_on, reason)`, `zebkit_allow_sleep(token)` | `zebkit.prevent_sleep(reason)`, `allow_sleep(token)` | `system::prevent_sleep`, `system::allow_sleep` | `zebkit.preventSleep(reason)`, `allowSleep(token)` |
| `zebkit_view_context_menu_run(view, menu_id, command, &error)`, `zebkit_view_context_menu_show(...)` | `view.context_menu_run(menu_id, command)`, `view.context_menu_show(menu_id, items, fn)` | `view.context_menu_run`, `view.context_menu_show` | `view.contextMenuRun`, `view.contextMenuShow` |
| `zebkit_view_on_eye_dropper(view, func, data)`; `zebkit_eye_dropper_respond(request, "#rrggbb" or NULL)` | `view.on_eye_dropper(fn)`; `request.respond(color)` | `view.on_eye_dropper(fn)` | `view.onEyeDropper(fn)` |
| `zebkit_view_on_share(view, func, data)`; `zebkit_share_respond(request, shared)` | `view.on_share(fn)`; `request.respond(True)` | `view.on_share(fn)` | `view.onShare(fn)` |
| `zebkit_view_on_device_chooser(view, func, data)`; `zebkit_device_chooser_respond(request, id or NULL)` | `view.on_device_chooser(fn)`; `request.respond(id)` | `view.on_device_chooser(fn)` | `view.onDeviceChooser(fn)` |
| `zebkit_view_on_media_access(...)` with `"type": "display"` | `view.on_media_access(fn)`; `request.respond(True, None, "view")` | `view.on_media_access(fn)` | `view.onMediaAccess(fn)` |

- **Events** are JSON. Power: `{"event": "suspend" | "resume" | "lock" |
  "unlock"}`, `{"event": "power-source", "onBattery"}`. Lock and unlock come
  from the system's own notifications: macOS's screen-lock notifications,
  Windows' session changes, and on Linux the desktop screen saver's D-Bus
  signal (GNOME, Cinnamon, MATE, Xfce and freedesktop's); a Linux desktop
  without a screen saver service tells nothing. Displays:
  `[{id, bounds, workArea, scale, rotation, primary}]` in DIPs.
  Notifications, when the application shows them: `{"event": "show", id,
  persistent, origin, title, body, icon, image, badge, tag, lang, silent,
  requireInteraction, renotify, actions}` and `{"event": "close", id}`; it
  tells clicks and closes back with `zebkit_notification_clicked()` and
  `zebkit_notification_closed()`. Without a function the system shows them
  (Linux's notification service, macOS for a bundled application, Windows'
  notification area).
- **The badge** is text on macOS and Windows (an overlay icon), a number on
  Linux's launcher docks; `""` clears it. A progress below 0 clears it.
- **Device choosers**: the request's `kind` (`usb`, `hid`, `serial`),
  `origin`, `filters` as the page gave them, and `devices`, each with an
  `id`, a `name` and what the kind has (`vendorId`, `productId`,
  `serialNumber`, `path`, HID `collections`). `request.info` has the list as
  it is now. An id not listed is refused. A device chosen is the origin's
  until the engine ends or the page's `forget()`.
- **Screen capture**: `getDisplayMedia()` reaches the media access hook with
  `"type": "display"`, `audio`, and `sources` (`view`, and `screen` where the
  system's screen can be captured). The video answer is the source's id; an
  audio answer gives the view's sound with it.
- **Opening**: `zebkit_open_external()` gives the URL to the system's
  handler (a browser for `https:`, the mail program for `mailto:`, a
  `file:` URL's default program) and refuses `javascript:`, `data:`, `blob:`,
  `about:` and invalid URLs; `zebkit_show_in_folder()` takes an absolute
  path. The URL or path is one argument to the system, never a shell's.
  Both work off the engine's thread and return true once the request is
  valid and handed to the system; the `_async` forms tell how it went,
  `(done, error, data)`, once, after the call (an invalid URL or path, a file
  that is not there, no program for the URL). macOS's Finder says nothing of
  itself: done once it is asked.

## Runtime view preferences

`<zebkit/hooks.h>` has `zebkit_view_set_settings(view, json, &error)` and
`zebkit_view_get_settings(view)`. A batch merges top-level keys, null resets
one, and invalid input changes nothing. The getter returns explicit overrides;
free it with `zebkit_free`. These preferences persist across navigations and
apply to all frames through Chromium's own WebPreferences.

For example, `{"javaScript":false,"images":false,"defaultFontSize":20,
"colorScheme":"dark","autoplay":"document-activation"}`. Rust exposes
`View::set_settings` and `settings`; Python `view.set_settings({...})` and
`view.settings`; host JavaScript `view.setSettings({...})` and `view.settings`.
Temporary CDP emulation is separate. The complete field list, limits, reset
behavior and tests are in [the hooks contract](HOOKS.md#each-hook).

## Runtime settings of the engine and of a view

`<zebkit/hooks.h>`: `zebkit_set_engine_settings(json, &error)`, `zebkit_get_engine_settings()`,
`zebkit_view_set_runtime_settings(view, json, &error)`, `zebkit_view_get_runtime_settings(view)`;
Rust `zebkit::set_engine_settings`, `View::set_runtime_settings`; Python `zebkit.set_engine_settings`,
`view.set_runtime_settings`; JavaScript `zebkit.setEngineSettings`, `view.setRuntimeSettings`. On the
engine's thread. JSON up to 64 KiB, merged by key; `null` removes one, an invalid batch changes nothing.
The getters give `{"explicit": ..., "effective": ...}`.

| Key | Values | Scope | Default |
|---|---|---|---|
| `thirdPartyCookies` | `block`, `allow` | engine, the view's context; the latest write wins | `block` |
| `dialogs` | `default`, `hold`, `accept`, `dismiss` | engine, view | `default` |
| `downloads` | `save`, `deny` | engine, view; a function set still decides | `save` |
| `newWindows` | `block-without-gesture`, `allow`, `block` | engine, view | `block-without-gesture` |
| `spareRenderer` | boolean | engine | `false` |
| `clientCertificateStores` | boolean | engine | `false` |

A view's value goes before the engine's, except `thirdPartyCookies`. `--zebkit-allow-third-party-cookies`
and `--zebkit-spare-renderer` stay the startup defaults, as does `--zebkit-client-certificate-stores`
for `clientCertificateStores`: whether a TLS server's request for a client certificate is offered the
operating system's identities (see the client certificate hook). Cookies change asynchronously, and a document
may keep those it has read.

## Runtime document permissions

`<zebkit/hooks.h>` configures rules with `zebkit_view_set_permission`, queries
policy with `zebkit_view_get_permission` and resets view rules independently
of context overrides. Install `zebkit_view_on_permission` to receive prompts;
reply once through `zebkit_permission_respond(request, state, remember)`.
The default deadline is 30 seconds. Metadata and outcome callbacks include
origins and request IDs; navigation, closure and deadlines invalidate old tokens.
Document security and Permissions Policy still apply. CDP and context grants
use Chromium's own override table and subscriptions; later changes take effect
in the permission's native Chromium scope. Worker policy remains context scoped.

Rust, Python and host JavaScript expose the same rules and request lifecycle.
See [the hooks contract](HOOKS.md#each-hook) for limits and bindings.

Besides Chromium's permissions (by their Permissions API names, such as
`geolocation`, `camera`, `clipboard-read`), three are the engine's own, asked
and set the same way:

| Permission | Asked when | Metadata | Default |
|---|---|---|---|
| `file-system-write` | a page writes, through File System Access, to a file it opened (`createWritable()`, `requestPermission({mode: 'readwrite'})`), not one it saved | `path`, `name`, `kind` (`file`, `directory`) | denied |
| `file-system-read` | a page reads a handle beyond what the person picked or dropped (one restored from storage) | the same | denied |
| `protocol-handler` | `navigator.registerProtocolHandler()`, with a click | `scheme`, `url` | denied; nothing is registered with the system |

`pointer-lock` is granted by default where the view has a window and refused
without one (`--headless`, off screen), where there is no system pointer;
`keyboard-lock` is denied by default. An unanswered engine permission (no hook,
the deadline) is asked again the next time; an answer, or a rule, holds.

## Runtime document navigation policy

`zebkit_view_on_navigation` pauses navigation stages for a host decision.
`zebkit_navigation_respond` continues, cancels or starts a replacement GET in
the same frame. Metadata includes navigation/frame IDs, redirects and response
headers; a default 30-second deadline continues unanswered stages. Old tokens
cannot answer later stages. Handler removal continues pending stages.
`zebkit_view_on_navigation_result` tells each outcome: `state` `continued`,
`cancelled` or `redirected` (with `replacementUrl`) and a `reason`, as every
hook's outcome ([the outcome](HOOKS.md#the-outcome)).

Rust uses `View::on_navigation` and `NavigationAction`; Python
`view.on_navigation` and `request.respond`; host JavaScript `view.onNavigation`
and `request.respond`. BFCache restoration and prerender activation use an
`activation` stage before the new page becomes active; `activationType` is
`back-forward-cache` or `prerender`. At this early stage the sanitized referrer
is unavailable (`referrerAvailable: false`, empty `referrer`). These decisions
support the same deadline, cancellation and replacement lifecycle. Renderer-only
same-document changes remain outside the policy. Internal inspection now uses
tab targets, so evaluating a page does not disable prerendering. New page targets
inherit native initialization scripts and the host's UA override before their
own scripts execute. Script registrations have view-owned opaque identifiers;
removal clears both current target registrations and future replay state.
See [the hooks contract](HOOKS.md#each-hook).

## Views: popups, console, certificates, user agent, sign-in

What a view decides about its popups, its console, certificates, its user agent and HTTP sign-in, beside the hooks of `<zebkit/hooks.h>`.

### Adopting a site's authorization popup

Use `zebkit_view_on_window_open()` to receive Chromium's actual popup and
`zebkit_view_get_target_id()` to attach a CDP client to it. The returned
identifier is freed with `zebkit_free()`. Rust exposes `View::target_id()`
and `clear_new_window_handler()`. Python exposes `View.target_id()`.
A URL-only `on_new_window` callback cancels the original operation and cannot
preserve a form's POST or `window.opener`; see
[the integration regression](notes/google-signin.md#integrated-browser-preserve-the-original-authorization-popup).

### New-window decisions and creation metadata

The native `<zebkit/hooks.h>` extension adds
`zebkit_view_on_window_open_event(view, callback, data)` as an asynchronous
JSON observer. `phase: "decision"` reports the actual Chromium URL, frame name,
gesture, disposition, container type, source origin, opener/top URLs, referrer,
opener suppression and the policy result. `features` contains `bounds`, explicit
`hasX`, `hasY`, `hasWidth`, `hasHeight` flags, `popup` and `alwaysOnTop`.
An explicitly requested zero coordinate differs from an omitted coordinate.
These are Chromium's requested window parameters, not measured host bounds.

`phase: "created"` identifies the actual new view with `targetId` and reports
its creation URL, frame name, disposition, features, gesture and opener origins.
Use the existing `zebkit_view_on_window_open` callback to receive that view.
`zebkit_view_get_window_open_info(child)` returns the same creation snapshot as
owned JSON, freed with `zebkit_free`; it remains unchanged after navigation.
Ordinary views return `{}`; invalid or non-UI-thread calls return NULL.
The old blocked event retains exactly `url`, `frameName` and `userGesture`.
`zebkit_view_on_popup` continues to describe off-screen select widgets.

Python exposes `on_window_open(child, url)`, `on_window_open_event(dict)` and
`window_open_info`. Rust adds `on_window_open_event`, its clear method and
`window_open_info` alongside its existing child-view callback. Host JavaScript
adds `windowOpenInfo` and the `window-open` event; its created event has `view`,
an adopted `zebkit.View` for the original window, or null if it closed before
event delivery. The child has its own lifetime
and can remain open after its parent closes.

This extension is undergoing runtime validation. Linux and macOS native
compilation checks, Rust checks and JavaScript syntax checks pass; real window
behavior checks remain pending. This status does not imply macOS runtime validation.

### Native document-console events

`zebkit_view_on_console(view, callback, data)` observes Chromium document
console messages without evaluating page objects. Its borrowed JSON includes
`level` (`debug`, `info`, `warning`, `error`), `message`, `source`, `line`,
`frameId`, `mainFrame`, `origin` and optional `stackTrace` (null when absent).
The stack is Chromium's untrusted exception text, not structured JavaScript
frames; it is generally unavailable. Worker and DevTools-only console events
are outside this hook.

Delivery is asynchronous on the engine thread. Replacement, removal and view
closure cancel queued reports. Each view queues at most 256 messages; `dropped`
is a decimal string counting skipped reports since the previous delivery.
Message/source/stack limits are 65536/16384/65536 UTF-16 code units, without
splitting a surrogate pair; `truncated` reports clipping. Linux compilation and
Python test syntax checks pass. Python `View.on_console`, Rust `View::on_console`
and `clear_console_handler`, and Host JavaScript's `console` view event expose
this observer. Linux native and binding runtime regressions pass, including
callback replacement, closure, frame isolation and bounded output. Worker
console coverage remains separate work.

### Document certificate errors

`zebkit_view_on_certificate` receives an opaque single-use request and borrowed
JSON. Default handling denies the TLS error. `zebkit_certificate_request_info`
returns owned JSON while pending; `zebkit_certificate_respond(request, allow)`
returns false for stale/expired requests or attempts to ignore a strict error.
A rejected strict-error bypass leaves the request pending so the application
can deny it. There is no remembered exception in this API.

Metadata includes a decimal-string `id`, URL, `mainFrame`, numeric error and
`errorName`, `strictEnforcement`, `canIgnore`, certificate subject/issuer,
validity dates, status bits, `knownRoot`, and a PEM `certificateChain`.
The chain is limited to 16 certificates/1 MiB and reports `chainTruncated`.
Metadata also includes `state`, `timeoutMs` and `deadlineUnixMs`; deadlines use
a monotonic clock. At most 1024 requests wait. The default deadline is 30000 ms;
`zebkit_view_set_certificate_timeout` accepts 1..300000 for future requests.
Top-level navigation, closure, stopping the corresponding navigation and
handler replacement invalidate pending requests. Outcome events contain
`continued`/`denied` and a reason. Calls and replies run on the engine thread.

Python exposes `CertificateRequest`, `View.on_certificate`,
`on_certificate_result` and `set_certificate_timeout`. Rust exposes the
corresponding `CertificateRequest`, View methods and clear methods. Host
JavaScript uses `onCertificate`, `setCertificateTimeout` and the
`certificate-result` view event. A JavaScript request has `metadata`, `info`
and `respond(allow = false)`.

This is the document-error hook. Chromium's CDP certificate handling may
consume an error first. Client certificate selection, requests without a
WebContents and finer subframe cancellation remain separate work. Certificate
metadata is not a trust verdict. Real Linux TLS tests pass through C, Rust,
Python and host JavaScript, including strict-error denial, deadlines and
callback lifetime. Pooled HTTP/1.1 and HTTP/2 tests verify denied requests send
no headers or body. The complete glibc 2.17 suite also includes real C TLS
requests. Mac compilation evidence does not establish Mac runtime behavior.

### Atomic user agent and client hints

`zebkit_view_set_user_agent_override(view, json, callback, data)` accepts a JSON
object with required string `userAgent` and optional `acceptLanguage`, `platform`
and `userAgentMetadata`, using Chromium's `Emulation.setUserAgentOverride`
parameter format. The input is limited to 65536 UTF-8 bytes; unknown top-level
options are rejected. On a live view, on the engine's thread, it returns true,
and the callback is told exactly once, after it returns: `{}` and no error on
success, or a null value and the error, invalid JSON or options among them.
It returns false, and the callback is never called, only for a NULL or closed
view or off the engine's thread.

Chromium validates metadata before it is saved for future targets. Invalid
updates leave the previous renderer/network override and replay state intact.
Loads wait for completion; a navigation selected inside the completion replaces
an older waiting URL. Closing completes pending callbacks with an error before
the view's close notification. An empty `userAgent` with no metadata clears this
session's override. Independent external CDP sessions retain Chromium's scope;
the last command across different sessions is not guaranteed to win.

Python provides `View.set_user_agent_override(options, function=None)`, with a
dictionary or JSON string and completion `(value, error)`. Rust's corresponding
View method accepts JSON and a `FnOnce(Result<String, Error>)`. Both follow the
C rule: invalid options return true and are told once, after the call returns;
false, the function never called, only for a closed view (or, in Python, the
wrong thread). Host JavaScript's
`view.setUserAgentOverride(options)` returns a promise. Existing string-only
`zebkit_view_set_user_agent` remains available and ignores invalid HTTP-header
values without changing saved settings.

Linux tests cover document-first-script UA, brands, high-entropy metadata,
request headers after Accept-CH, worker metadata, reset, rejected updates,
completion-triggered navigation and closure. Rust and host JavaScript runtime
tests also pass. Mac syntax evidence remains distinct from runtime validation.

### HTTP authentication

`zebkit_view_on_http_auth` receives an engine-owned opaque request token and
borrowed JSON for an HTTP or proxy challenge. Reply once on the engine thread
with `zebkit_http_auth_respond(request, username, password)`; both NULL cancel,
while two non-NULL strings provide credentials, including empty strings. Each
string must be valid UTF-8 and at most 65536 bytes. Invalid replies leave the
request pending. `zebkit_http_auth_request_info` returns owned JSON while pending
and NULL after settlement; free returned strings with `zebkit_free`.

Metadata contains `id`, `url`, `realm`, `scheme`, `challenger`, `host`, `port`,
`proxy`, `navigation`, `mainFrame`, `firstAttempt`, `metadataTruncated`, `state`,
`timeoutMs` and `deadlineUnixMs`. `mainFrame` identifies a primary-main-frame
navigation, rather than an inferred fetch initiator. URL userinfo is redacted;
credentials supplied in replies are not added to metadata or result JSON.

`zebkit_view_set_http_auth_timeout` accepts 1..300000 ms for future challenges,
default 30000. Monotonic deadlines, navigation, stopping, closing and handler
replacement invalidate requests. `zebkit_view_on_http_auth_result` observes
asynchronous outcome metadata with `state` `provided` or `cancelled` and a
`reason` ([the outcome](HOOKS.md#the-outcome)). A closing view's
challenges end without an outcome (`on_close` is the signal). At most 1024 challenges wait.

Python exposes `HttpAuthRequest`, `View.on_http_auth`, `on_http_auth_result` and
`set_http_auth_timeout`; requests provide `info` and `respond(user, password)`
or `respond()` to cancel. Rust exposes `HttpAuthRequest::provide`, `cancel`,
`info` and corresponding View registration, clear and timeout methods. Host
JavaScript uses `onHttpAuth`, `setHttpAuthTimeout`, the `http-auth-result` event,
and request `metadata`, `info`, `respond(user, password)` or `respond()`.

An installed native view handler takes precedence at the browser login delegate.
Without one, existing automation/shell handling remains. Chromium's context auth
cache and earlier CDP interception retain their native scopes. Requests without
a WebContents remain separate work. Real Linux HTTP 401/proxy 407 tests and
Python/Rust/host JavaScript callback lifetime tests pass; this does not establish
Mac runtime behavior.

### What a page asks of its window, and its network

`<zebkit/hooks.h>` has the details of each. Requests are single-use, on the engine
thread, with a deadline of 1..300000 ms (default 30000): a function may answer later,
replace itself or close the view. A popup's opener decides until the popup has its own
function, and once that opener closed, the popup is refused.

| Hook | C | Python | Rust | Host JavaScript | Unanswered |
|---|---|---|---|---|---|
| Links to other programs | `zebkit_view_on_external_protocol` | `on_external_protocol` | `on_external_protocol` | `onExternalProtocol` | told only |
| Downloads | `zebkit_view_on_download`, `_progress`, `_result`, `zebkit_download_respond`, `zebkit_view_cancel_download` | `on_download`, `on_download_result`, `DownloadRequest.respond('save', path)` | `on_download`, `on_download_result`, `DownloadRequest::save`, `cancel` | `onDownload`, `onDownloadResult`, `respond('save', path)` | cancelled |
| Client certificates | `zebkit_view_on_client_certificate`, `zebkit_client_certificate_respond`, `zebkit_client_certificate_choose` | `on_client_certificate`, `respond('provide', pem, key)`, `choose(index)` | `on_client_certificate`, `provide`, `respond`, `choose` | `onClientCertificate`, `respond('provide', pem, key)`, `respond('choose', index)` | request fails |
| Isolated worlds | `zebkit_view_add_world_script` | `add_world_script(source, world, main_frame_only)` | `add_world_script` | `addWorldScript(source, {world, mainFrameOnly})` | not asked |
| File choosers | `zebkit_view_on_file_chooser`, `zebkit_file_chooser_respond` | `on_file_chooser`, `respond([paths])` | `on_file_chooser`, `respond(&[paths])` | `onFileChooser`, `respond([paths])` | cancelled |
| Camera, microphone | `zebkit_view_on_media_access`, `zebkit_media_access_respond` | `on_media_access`, `respond(allow)` | `on_media_access`, `respond(allow, None, None)` | `onMediaAccess`, `respond(allow)` | refused |
| Keys | `zebkit_view_set_reserved_keys`, `zebkit_view_on_reserved_key` | `set_reserved_keys`, `on_reserved_key` | same | `setReservedKeys`, `onReservedKey` | not asked |
| Context menu | `zebkit_view_on_context_menu` | `on_context_menu` | `on_context_menu` | `onContextMenu` | told only |
| Fullscreen | `zebkit_view_set_fullscreen_policy`, `zebkit_view_on_fullscreen` | `set_fullscreen_policy`, `on_fullscreen` | `set_fullscreen_policy`, `on_fullscreen` | `setFullscreenPolicy`, `onFullscreen` | not asked |
| Network, also `zebkit_view_set_network_filter_for_workers`, `zebkit_view_on_network_body`, `zebkit_network_respond_file`, `zebkit_network_respond_stream` in each language (`docs/HOOKS.md`) | `zebkit_view_set_network_filter`, `zebkit_view_on_network`, `zebkit_network_respond`, `zebkit_view_on_network_event` | `set_network_filter`, `on_network`, `respond('block')`, `on_network_event` | `set_network_filter`, `on_network`, `respond(json, body)`, `on_network_event` | `setNetworkFilter`, `onNetwork`, `respond({action}, body)`, `onNetworkEvent` | goes on |

Asked hooks also have `zebkit_view_set_<hook>_timeout` and `zebkit_<hook>_request_info`.
Each tells its outcome in one form, the request's metadata with `state` and `reason`
([the outcome](HOOKS.md#the-outcome)): to its `*_result` function, to the network's events
for a held request, and for downloads to `zebkit_view_on_download_result`, whose `state` is
`saved` or `cancelled`. Requests of a closing view end without an outcome, `on_close`
being the signal (`respond()` returns false from then on), and those the engine ends as it
stops are refused and never told. A refused reply says why: `char **error` in C, `request.error` in host
JavaScript. Console and network events count what they had to drop as `dropped`, a number.
A device chooser's serial `filters` have `vendorId` and `productId`, as USB's and HID's do.

How each download ends is told to `zebkit_view_on_download_progress()`'s function: `outcome`
`complete` (with `savedPath`, the file's final path), `cancelled` (by the application, the
hook's answer, CDP's `Browser.cancelDownload` or a closed opener) or `failed`; a cancelled or
failed one has `error` `{category, code, name, message, resumable}`, its category `cancelled`,
`network`, `server`, `file` (no space, no permission, a path too long), `blocked`, `shutdown` or
`crash`, its code and name Chromium's `DownloadInterruptReason` (20 `NETWORK_FAILED`, 2
`FILE_ACCESS_DENIED`, 40 `USER_CANCELED`, ...) and its message Chrome's words ("Network error",
"Disk full"). Each report has the `guid` CDP's `Browser.downloadWillBegin` names it by. CDP's own
`Browser.downloadProgress` says `canceled` for a failure as well, as Chrome's does; a CDP client
gets these reports instead as `Zebkit.downloadProgress` after `Zebkit.setDownloadEvents
{enabled: true}`, on a page's session for that page's downloads, on the browser's for all.
The network hook's `response` and `headersReceived` stages have `remoteAddress`, the IP address
the response came from (absent when the network gives none, as through some proxies).
Rules of the network hook at the `sendHeaders`, `sent` and `headersReceived` stages see what the
network adds, Cookie and Set-Cookie, and at `sent` Authorization and Host, as DevTools does; those
that hold fail closed (`<zebkit/hooks.h>`).

Client certificates come from the application, or, with the engine setting
`clientCertificateStores`, from the operating system's store: the request's `identities` (subject,
issuer, validity, SHA-256, the PEM certificate) of the user's NSS database on Linux, Keychain on
macOS and the user's `MY` store on Windows, of which `choose(index)` answers with one; its key stays
in the store, which signs. Limits are in `docs/HOOKS.md`.

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
| LevelDB | `leveldb.h` | see `leveldb/c.h` | Key-value storage through LevelDB's own C API, with or without the engine running: a database, batches, snapshots, iterators. Compactions run on threads of their own until the engine starts; paths are their bytes |
| FreeType | `freetype.h` | 189 | The engine's 2.14.3 build; fonts, glyphs and outlines; create an independent `FT_Library`. Legacy Carbon APIs are not built |
| libxml2 | `libxml.h` | 1,008 | XML and HTML parsing, the tree, XPath, the reader and writer |
| libwebp | `libwebp.h` | 105 | Decoding, encoding, demuxing, muxing |
| libjpeg-turbo | `libjpeg.h` | 63 | The libjpeg API |
| FFmpeg | `ffmpeg.h` | 611 | zebkit's configuration (`src/ffmpeg`, `scripts/ffmpeg-config.sh`): Chrome's demuxers and decoders, and for applications libswresample, libswscale, the file protocol, the MP4, MOV, Matroska and WebM muxers, VP8 and VP9 through the engine's libvpx; no encoders |
| dav1d | `dav1d.h` | 19 | AV1 decoding |
| Opus | `opus.h` | 71 | Encoding and decoding |
| libvpx | `libvpx.h` | 37 | VP8 and VP9, decoding and encoding |
| PDFium | `pdfium.h` | 468 | Chromium's, without XFA forms and its JavaScript; call `FPDF_InitLibrary()` first, or use `<zebkit/pdf.h>` |

7,421 functions across 16 libraries in the current Linux slim build (counts
vary with platform/configuration), and the engine exports nothing else but zebkit's
own API (`scripts/engine-test.sh` checks both). Only the headers the
library's header reaches are shipped, not the library's internals.

What is there is what the engine is built with: a function is exposed when
the library's public headers declare it and the library's compiled objects
define it. `src/engine/vendor/gen_vendor.py` finds them in the build, at
the end of it, and writes the entry points and the headers; nothing is
listed by hand. Chromium renames some libraries' functions (zlib's
`Cr_z_`, SQLite's `chrome_sqlite3_`, ICU's version suffix,
libjpeg-turbo's `chromium_`) with macros in their headers; the generator
follows them. FreeType's macro header names use package-qualified paths,
so `#include FT_OUTLINE_H` works with the package's include directory alone.
The shipped configuration headers match the engine's build.

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

| `zebkit_fetch_in`, `zebkit_preconnect`, `zebkit_clear_http_cache` | A request in a context's network; connections opened ahead; the HTTP cache emptied ([Connection reuse](#connection-reuse)) | The network service |

All but `zebkit_fetch()` and the connection functions work without the engine running.
For a process that also starts the engine, call ICU-backed URL services
from the ready callback. Chromium registers and locks its URL schemes
during startup; parsing URLs first prevents that initialization. Standalone
service programs can use these functions without starting the engine.

### Connection reuse

What makes pages and requests load sooner by reusing what the network has
already done; measured in [CONNECTION_REUSE.md](CONNECTION_REUSE.md).

```c
bool zebkit_clear_http_cache(zebkit_context *context, zebkit_task_func done, void *data);
bool zebkit_preconnect(zebkit_context *context, const char *url, int count);
void zebkit_fetch_in(zebkit_context *context, const char *method, const char *url, const char *headers,
                     const void *body, size_t body_size, zebkit_response_func func, void *data);
```

- **What a profile keeps.** A profile on disk keeps its HTTP cache in
  `<profile>/Cache`, and in `<profile>/Network` what the network learnt of
  servers: HTTP/2 and alternative services (`Network Persistent State`) and
  HSTS (`TransportSecurity`), as Chrome does. A restart then revalidates what
  it cached instead of downloading it again. The startup setting `httpCache`
  chooses `disk` (the default), `memory` or `off`, and `maxSize`. The cache
  holds what the profile's pages loaded, so it says which sites they were:
  an application that must not keep that chooses `memory` or clears it. A
  `zebkit_context` keeps all of it in memory, whatever the setting.
- **`zebkit_clear_http_cache()`** empties the cache of a context (NULL: the
  engine's profile) and forgets what was learnt of servers; `done` is called
  on the engine's thread when both are done. Cookies and storage are not
  touched.
- **`zebkit_preconnect()`** opens `count` (1 to 6) connections to an origin,
  TLS included, in a context's network (NULL: the engine's profile), for a load
  or request soon after; nothing is sent, and an unused connection is closed
  after about 60 s. The startup setting `preconnect` does it for the engine's
  profile as it starts. Pages' own `<link rel=preconnect>` works as in Chrome.
- **`zebkit_fetch_in()`** is `zebkit_fetch()` in a context's network: its
  cookies, its cache and the connections its pages opened. `zebkit_fetch()` is
  always the engine's profile's.
- **Contexts start cold.** Each context has its own connections and cache;
  Chromium has no way to share them between profiles. Views that load the
  same sites belong in one context.

| | Rust | Python | Go | JavaScript (zebkit-js) |
|---|---|---|---|---|
| Clear the cache | `services::clear_http_cache(done)`, `Context::clear_http_cache(done)` | `clear_http_cache(function, context=None)` | `ClearHTTPCache(done)`, `Context.ClearHTTPCache(done)` | `zebkit.clearHttpCache({context})` (a promise) |
| Preconnect | `services::preconnect(url, count)`, `Context::preconnect(url, count)` | `preconnect(url, count=1, context=None)` | `Preconnect(url, count)`, `Context.Preconnect(url, count)` | `zebkit.preconnect(url, {count, context})` |
| Fetch in a context | `Context::fetch(method, url, headers, body, done)` | `fetch_in(context, url, function, ...)` | `Context.Fetch(method, url, headers, body, done)` | `zebkit.fetch(url, {context, ...})` |

### PDF

`<zebkit/pdf.h>` is documents on Chromium's own PDFium, which is in the
engine (without XFA forms and without PDFium's JavaScript). What it covers
is measured against EmbedPDF's viewer (`docs/ROADMAP.md`).

```c
zebkit_pdf *pdf = zebkit_pdf_open_file("report.pdf", NULL, &error);
char *info = zebkit_pdf_info(pdf);                      /* JSON: pages, metadata, permissions */
zebkit_pdf_render_options tile = {.scale = 2, .background = 0xffffffff,
                                  .x = 0, .y = 0, .width = 512, .height = 512};
uint8_t *rgba = zebkit_pdf_render(pdf, 0, &tile, &width, &height, &error);
char *found = zebkit_pdf_search(pdf, "revenue", 0, 0, 100, &error);
int index = zebkit_pdf_annotation_add(pdf, 0, "{\"type\": \"highlight\", \"rect\": [...], \"quads\": [[...]]}", &error);
uint8_t *bytes = zebkit_pdf_save(pdf, ZEBKIT_PDF_SAVE_INCREMENTAL, &size, &error);
```

| Functions | What |
|---|---|
| `zebkit_pdf_open`, `_open_file`, `_close`, `_save` | Bytes or a file, a password; saved whole or incrementally, with or without its encryption |
| `zebkit_pdf_info`, `_set_metadata` | Pages, version, encryption, permissions, form type, language, the document information |
| `zebkit_pdf_page_count`, `_page` | A page's size as shown, rotation, label and boxes |
| `zebkit_pdf_render`, `_render_annotation` | RGBA at a scale and rotation, the whole page or a tile, with annotations and form fields, on a background; one annotation's appearance alone, for a layer over the page |
| `zebkit_pdf_text`, `_text_layout`, `_text_lines`, `_text_rects`, `_char_at` | The text; each character's box, font, size and angle; lines with their boxes and unmapped characters; selection rectangles; hit testing |
| `zebkit_pdf_images`, `_image_data` | The images a page draws, with their pixel sizes and places (a page that is one image is a scan); one's bytes, its JPEG as stored or its pixels as PNG |
| `zebkit_pdf_search` | Every match in the document, with its rectangles and the text around it |
| `zebkit_pdf_signatures`, `_signature_contents` | The digital signatures, nested form fields too ([`patches/1020`](../patches/1020-pdf-nested-signature-fields.patch)): reason, time, byte range, permissions, the signer's certificate, and whether each is intact: the bytes it covers are those signed, by the signer's key, and it covers the whole file; the PKCS#7 itself. Trust in the certificate is the application's to judge |
| `zebkit_pdf_outline`, `_set_outline`, `_links` | Read/replace bookmarks (hierarchy, page positions, URLs), link annotations and addresses written in the text |
| `zebkit_pdf_annotations`, `_annotation_add`, `_update`, `_remove`, `_set_image`, `zebkit_pdf_flatten` | Every kind read; highlights, underlines, squiggles, strikeouts, ink, lines, polygons, polylines, squares, circles, notes, free text, links and redaction marks made, changed and removed, with their appearances generated; an image as a stamp's appearance |
| `zebkit_pdf_redact` | What is under the page's redaction marks, or areas given, taken out for good: text character by character, image pixels blacked out, paths and annotations that reach in; boxes drawn over |
| `zebkit_pdf_form_fields`, `_form_set`, `_form_reset` | Every field with its type, value, options and widgets; a value set, check boxes and radio buttons by export value, with the appearance redrawn; the form reset |
| `zebkit_pdf_page_rotate`, `_pages_delete`, `_pages_move`, `_page_insert`, `_pages_import`, `zebkit_pdf_new` | Pages turned, deleted, moved, inserted blank, copied in from another document; a new document to build from others' pages (merge, extract) |
| `zebkit_pdf_attachments`, `_attachment`, `_attach`, `_detach` | Embedded files |

Structured answers are JSON. Coordinates are PDF page space: points, the
origin at the bottom left, before the page's rotation. PDFium is not
thread-safe; the functions take one lock, so any thread may call them.
Where PDFium had nothing for it, zebkit patches PDFium (`patches/1014`,
`1015`, `1020`). Still to come: trust in a
signer's certificate (`docs/ROADMAP.md`).

In JavaScript the same is `zebkit.pdf` (layer 4), for pages granted
`ZEBKIT_ALLOW_PDF` and for the application's script. A page's documents are
worked on in the browser process, one call at a time on the thread pool;
a page reaches only the documents it opened, and they close with its view.
A PDF is untrusted input, and PDFium in the browser process is outside the
renderer's sandbox: a sandboxed utility process for it is on the roadmap.

### A PDF in a page

A view that navigates to a PDF (`application/pdf`, `text/pdf`), or an `<embed>`, `<object>` or
`<iframe>` of one, shows it as Chrome does, with nothing to call (2026-11-01). Chromium's own plugin
(`//pdf`) parses and draws it, always in a sandboxed renderer of the PDF's site (`--pdf-renderer`, no
JIT, no storage, cookies or workers of any origin), as Chrome's; neither the browser process nor a page's
process parses it. So a same-origin parent does not reach the frame's document (`contentDocument` is
null), where Chrome's parent reaches a stub document of its own process. A small viewer
(`src/engine/pdf_viewer.js`) in a closed shadow root is what Chrome's extension is:

- Continuous vertical scrolling: wheel, trackpad, scrollbar, Arrow keys, Page Up, Page Down, Home, End,
  Space. Only pages in sight are loaded and painted.
- Zoom (the toolbar, Control with `+`, `-`, `0`, Control and the wheel, a pinch), the page number, find
  (Control F), selecting and copying text, links, forms, download, print, `Page.printToPDF`, `#page=`,
  `#zoom=`, `#toolbar=0`.
- To the page, as Chrome: `navigator.pdfViewerEnabled`, `navigator.plugins`, `mimeTypes`, the document's
  HTML to the byte, one request for the file. Differences: `embed.postMessage` (Chrome's scripting API)
  is not there; one frame, not Chrome's stub, extension frame and inner frame; forms' JavaScript is not
  run; no thumbnails or outline sidebar.
- `--disable-pdf-viewer`: PDFs are downloads and `pdfViewerEnabled` is false, as Chrome's "Download PDFs".

### Annotations

`zebkit_pdf_annotation_add()` and `_update()` take JSON; the header lists every
key and its range. What fails, fails with an error and changes nothing.

```c
zebkit_pdf_annotation_add(pdf, 0, "{\"type\":\"line\",\"line\":[50,100,250,100],"
    "\"lineEndings\":[\"none\",\"closedArrow\"],\"leader\":{\"length\":10},"
    "\"measure\":{\"unit\":\"mm\",\"scale\":0.3528}}", &error);
```

| What | Keys and calls |
|---|---|
| Appearance | `lineEndings`, `leader`, `caption`, `borderStyle` (dashes), `borderEffect` (cloudy), `opacity`, `blend`, `rotation` |
| Free text | `font`, `fontSize`, `textColor`, `borderColor`, `color` (background), `align`, `richText` (`/RC`), `defaultStyle` (`/DS`), `callout`, `calloutEnding`; fonts by `zebkit_pdf_font_add()` or a hook |
| Stamps and the rest | `stamp` (the 14 standard ones), `stampText`; `zebkit_pdf_annotation_stamp_page()`: another document's page as vectors; `caretSymbol`; `icon` on notes and attachments; `zebkit_pdf_annotation_attach_file()` and `_file()` |
| Discussion | `inReplyTo` (a cycle fails), `replyType`, `state`, `stateModel`, `popup`; reading gives `replies`. Removing takes the popup and the replies too |
| Measurement | `measure` and `intent`: distance, perimeter, area, the value as `contents` and the caption, and reading gives `measure.value` and `.text` |
| Viewers | `zebkit_pdf_annotation_at()`, `textRange`, `zebkit_pdf_annotation_move()`, `_copy()` (with appearance and an application's entries, to any document), `_flatten()`, `zebkit_pdf_annotation_action()` |
| Interchange | `zebkit_pdf_annotations_export()` and `_import()`: XFDF and FDF. Read with no network, no entities and no DTD; all or nothing; at most 64 MiB and 100000 annotations. Appearances and embedded files are not carried |

Generated appearances are form XObjects in the file, drawn by zebkit
(`src/engine/pdf_appearance.cc`), with no patch to PDFium: highlights, underlines
and ink stay PDFium's, with `blend` written into their state. A `rect` grows to
what is drawn; its unturned box stays in the entry `/ZebkitBox`, and an appearance
the application made is marked `/ZebkitOwnAppearance`, which only
`"regenerate": true` replaces.

### Hooks

`zebkit_pdf_hook_*` (NULL for all documents), in C and the same in Rust
(`zebkit::pdf::hook_*`), Python (`Pdf.hooks`, `zebkit.pdf_hooks`) and JavaScript
(`doc.use(plugin)`, `zebkit.pdf.use(plugin)`). `examples/pdf-plugin/` is one
plugin in C and in JavaScript: a kind with its own appearance, an undo history, OCR.

| Hook | What it is for |
|---|---|
| `before`, `after` | Events: open, save, render, annotation add, update, remove, move, flatten; form set, reset; redact, flatten; page insert, move, delete, rotate, import. A `before` may refuse or give another operation. An `after` sees the operation, with `inverse` when it can be undone, which `zebkit_pdf_apply()` runs |
| `kind` | A kind of annotation: name, prefix, schema, check, `draw`, hit test. Data under `/<prefix>Data`, kept through save, copy, XFDF and FDF, and into `/PieceInfo` by flatten (`zebkit_pdf_page_data()`) |
| `appearance` | Any type's appearance, or `"*"`'s; false leaves it to the generator |
| `layer` | Above or below a page's content when rendered, on its bitmap, in page space |
| `ocr` | Text for pages without any: `zebkit_pdf_text()`, `_text_lines()`, `_search()` |
| `font`, `trust`, `action` | Fonts for free text; `"trusted"` in `zebkit_pdf_signatures()`; what `zebkit_pdf_annotation_action()` means |
| `input` | Filters of a drawing tool's points, after the built-in `smooth`, `simplify`, `snapGrid`, `snapAngle` and `constrain` (`zebkit_pdf_filter_input()`) |

Callbacks run on the thread of the call that needs them, outside the lock around
PDFium, and may call the API. A hook may remove itself; `destroy` waits for the end
of its call. A hook may close the document: the call fails or finishes, and then
it closes. Nesting stops at 16.

An appearance is drawn on a canvas with `<zebkit/canvas.h>`'s calls that writes PDF
operators, not Skia's pixels: Skia has no way to write one form XObject, and the
appearance stays vectors. Text is outlines there, gradients are shadings without
their stops' alpha, images are XObjects, and pixels and PNG fail on it.

In JavaScript a plugin is an object:

```js
const remove = await doc.use({
  kinds: {App_Mark: {prefix: 'App_', schema: {mood: {type: 'string'}}, check(data) {},
                     draw(canvas, annotation, {width, height}) {}, hitTest(annotation, x, y) {}}},
  before: {'annotation.add': (operation) => operation /* or false, or {refuse: why} */},
  after: {'*': (operation) => history.push(operation.inverse)},
  appearance: {stamp: (canvas, annotation, size) => {}},
  layers: [{position: 'above', draw: (canvas, {page, scale}) => {}}],
  ocr: (page, image) => [{text: '...', box: [1, 2, 3, 4]}],
  font: (family) => bytes, trust: (signature) => true, action: (action) => true,
  filters: {ink: (points) => points},
});
```

They run in the script, around the calls it makes on that document, which may be
awaited: the engine's PDF work is on another thread, where a script could not be
waited for.

### Canvas and text

`<zebkit/canvas.h>`: a canvas on the engine's Skia over a bitmap of its own or the
caller's RGBA pixels: a matrix, paths, fill and stroke, colours, blend modes,
gradients, images, text in a font given as bytes (shaped by HarfBuzz, drawn as
outlines), PNG out, and `zebkit_canvas_run()` for a JSON list of the same
operations. `<zebkit/text.h>`: `zebkit_text_shape()`, a line of text to glyph ids,
advances, offsets and clusters as JSON. Rust: `zebkit::canvas`. Python:
`zebkit.Canvas`, `zebkit.Font`, `zebkit.shape_text`. JavaScript, in pages granted
`image` and in the application's script:

```js
const canvas = zebkit.canvas.create(100, 100);
canvas.fillColor(0xff0000ff).rect(10, 10, 20, 20).fill().font(fontBytes, 20).fillText('Hi', 10, 60);
const png = await canvas.png();
const shaped = await zebkit.text.shape(fontBytes, 20, 'Hi', {language: 'en', direction: 'ltr'});
```

Together they cost 0.4 MiB of the package; `docs/ROADMAP.md`'s Done row has the figures.

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

It finds the engine in `$ZEBKIT_DIR`.

`bindings/rust/` is the same for Rust: `zebkit-rs` declares the C API, and the
`zebkit` crate is safe Rust over it, with closures for callbacks and the
engine's thread enforced by the types (`bindings/rust/README.md`):

```rust
std::process::exit(zebkit::run(|| {
    let view = zebkit::View::new(Some("https://example.com/"), 1024, 768);
    view.grant("https://example.com", zebkit::Allow::APP, None);
    view.bind("greet", |call| {
        // The arguments are a JSON array, the answer a JSON value (serde_json here).
        let args: Vec<String> = serde_json::from_str(call.args_json()).unwrap_or_default();
        let name = args.first().map(String::as_str).unwrap_or("");
        call.resolve(&serde_json::to_string(&format!("hello, {name}")).unwrap());
    });
    view.on_close(zebkit::quit);
}));
```

`bindings/golang` is a Go binding over cgo: all of the C API, on Linux,
macOS and Windows (programs for Windows cross-built with its `cmd/zebkit-cc`;
`bindings/golang/README.md`). The tables in this document name C, Python, Rust
and host JavaScript; Go's names come from C's by these rules: the prefix
`zebkit_` goes; a function of an object is a method of its Go type
(`zebkit_view_on_load(view, ...)` is `view.OnLoad(...)`); a function of the
package puts its verb first (`zebkit_app_set_badge(text)` is
`zebkit.SetAppBadge(text)`, and so `SetAppMenu`, `UpdateAppMenuItem`,
`RegisterScheme`, `HandleScheme`, `ListenCDP`, `StopCDP`), one that makes an
object is `New...` (`NewView`, `NewTray`), one that reads is the noun
(`AppMenu`, `EngineSettings`, `CDPPort`) and one that sets a callback starts
with `On` (`OnAppReopen`); the rest is in Go's case, initialisms in capitals
(`CDP`, `PDF`, `URL`).
Playwright's typed API, as Rust's `zebkit::automation` and Python's
`zebkit.automation` have it, is the package
`github.com/ikey4u/zebkit/bindings/golang/automation` (`automation.NewPage(view)`,
`page.On(kind, fn)`, `GetByRole(role, automation.RoleOptions{Name: ...})`,
`automation.Expect(locator)`; `docs/AUTOMATION.md`); the root package keeps
the low level, `CallAutomation` and `View.OnAutomationEvent`.

`scripts/check-bindings.py` checks that Rust, Python, Go and host JavaScript
each reach every function the headers export.

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
| `zebkit.crypto` | `digest(algorithm, data)`, `hmac(algorithm, key, data)`, `randomBytes(count)`, `aesGcmSeal(key, nonce, data, ad)` and `aesGcmOpen(key, nonce, sealed, ad)` (AES-256/128-GCM: the ciphertext then its tag) |
| `zebkit.image` | `decode(bytes)`, `encode('png' \| 'jpeg' \| 'webp', {width, height, data}, quality)` |
| `zebkit.url` | `parse(url, base)`, `registrableDomain(host)` |
| `zebkit.pdf` | `open(bytes, {password})`, a document with `info`, `page`, `pages` (every page's `page`, in one call), `render(i, {scale, rotation, x, y, width, height})` (RGBA), `renderAnnotation(i, a, {mode, scale, rotation})` and `renderAnnotations(i, {scale, rotation})` (each annotation's appearance alone, for a layer over the page), `text`, `textLayout`, `textLines` (lines with boxes), `textRects`, `charAt`, `search`, `outline`, `setOutline(items)` (atomic bookmark-tree replacement), `signatures`, `links`, `images` (each image's pixel size and place), `imageData` (one's JPEG as stored, or PNG), `annotations`, `annotationsOfPages` (several pages' in one call), `addAnnotation`, `updateAnnotation`, `removeAnnotation`, `setAnnotationImage` (an image as a stamp's appearance) (annotations carry their dictionary's `entries`, which add and update also set), `flatten`, `redact(i, areas, fill)`, `formFields`, `setField`, `resetForm`, `rotatePage`, `deletePages`, `movePages`, `insertPage`, `importPages`, `attachments`, `attach`, `detach`, `setMetadata`, `save`, `close`; `zebkit.pdf.create()` for a new one: `<zebkit/pdf.h>` |
| `zebkit.pathForFile(file)` | A dropped `File`'s path, when the application asked for drops (`zebkit_view_on_drop()`); otherwise `''`. Not a promise |

Every function but `pathForFile` returns a promise; bytes are `Uint8Array`s, and a string is
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

How it works: the view's internal DevTools session adds a script that runs
before the page's own (`Page.addScriptToEvaluateOnNewDocument`); the script
makes the `zebkit` object (a load asked for right after the first grant
waits until the browser has the script, so that the new document cannot
miss it). A call goes over the frame's own channel to the browser, through
a native function the renderer gives that script alone (any other script
of the page never sees it), so the browser knows the calling frame and its
origin from the channel, not from the call. The call and its answer are
each JSON with their bytes beside it as they are, through shared memory
when they are large: no base64, and many calls under way at once. The
renderer stays sandboxed; the work is done in the browser process, PDFs,
files and compression on Chromium's thread pool.
`zebkit.app` hands the application JSON, with bytes as `{"$bytes":
base64}` both ways.

## Script outside any page

An application can be JavaScript, as Electron's main process is, without
Node: `zebkit_script_new()` makes a V8 context in the application's own
process, on the engine's thread, and `zebkit-js` runs a module with it.

```js
// app.mjs: zebkit-js app.mjs
import {render} from './ui.mjs';                       // ES modules, from files

const view = new zebkit.View('https://app.example/', {width: 1024, height: 768});
view.grant('https://app.example', ['app', 'fetch']);
view.bind('save', async function (name, text) {      // the page calls zebkit.app.save()
  await zebkit.fs.writeFile(`/home/me/notes/${name}`, text);
  return {saved: true, from: this.origin};
});
view.on('load', (event, url) => event === 'finished' && console.log('loaded', url));
view.on('close', () => zebkit.quit());

const packed = await zebkit.compress('zstd', new TextEncoder().encode('...'), 19);
```

| In the script | What |
|---|---|
| `zebkit.View(url, {width, height})` | `load`, `reload`, `back`, `forward`, `resize`, `close`, `url`, `title`, `evaluate(script)`, `grant(origin, capabilities, filesRoot)`, `bind(name, fn)`, `on('load' \| 'title' \| 'close', fn)` |
| `view.on('navigation-result' \| 'certificate-result' \| 'http-auth-result' \| 'permission-result' \| 'popup-blocked' \| 'console' \| 'window-state', fn)` | `fn(event, view)`: what the hook tells, and the View it is of, which is a popup's View while its opener decides for it |
| `zebkit.fetch`, `fs`, `compress`, `decompress`, `crypto`, `image`, `url`, `pdf` | As in a page (layer 4), with no grant: the script is the application; `fs` takes any path, and `pdf.open` a path as well as bytes |
| `zebkit.args`, `zebkit.quit()`, `zebkit.version`, `zebkit.engineVersion`, `zebkit.isHeadless()` | |
| `console`, `setTimeout`, `setInterval`, `TextEncoder`, `TextDecoder` | |
| `new zebkit.Context()`, `new zebkit.View(url, {context})`, `context.close()` | `zebkit_context_new()`, `zebkit_view_new_in()`, `zebkit_context_close()`: cookies, storage and cache of its own |
| `view.targetId`, `setSizeLimits`, `attach(parent, x, y, w, h)`, `setBounds`, `setVisible`, `attachNative(handle, ...)`, `detachNative()`, `setTitle`, `addScript`, `stop()`, `isLoading`, `setUserAgent`, `capture()` (a PNG), `setNavigationFailClosed`, `popupPolicy`, `on('new-window' \| 'drop', fn)` | the rest of `<zebkit/zebkit.h>`'s view |
| `view.on('dialog' \| 'dialog-closed', fn)`, `setDialogPolicy`, `dialogPolicy`, `respondDialog(id, accept, text)`, `dialogs`, `requestClose()`, `printToPdf(options)`, `dragEnter`, `dragOver`, `dragLeave`, `drop`, `dragSourceEnd`, `isDragging`, `on('drag-start' \| 'drag-end', fn)`, `zebkit.DRAG` | `<zebkit/page.h>` |
| `zebkit.View.offscreen(url, {width, height, scale, context})`, `on('frame', frame => ...)` (`frame.pixels()`, `keep()`, `release()`: released after the handlers unless kept), `setSize`, `setFrameRate`, `setMaxFramesInFlight`, `setFrameBuffers`, `requestFrame`, `sendMouse`, `sendWheel`, `sendKey`, `sendKeyWithCommands`, `imeSetComposition`, `imeCommit`, `imeCancel`, `setFocus`, `on('caret' \| 'cursor' \| 'popup', fn)`, `zebkit.MODIFIERS` | `<zebkit/offscreen.h>` |
| `zebkit.automation.call(view, command, params)`, `screenshotPixels`, `page(id)`, `pageId(view)`, `listen(port)`, `commands()`, `view.on('automation', fn)` | `<zebkit/automation.h>` |
| `view.mediaList()`, `mediaAt(x, y)`, `mediaCapture(id, path, options, {onProgress, onDone})`, `mediaTap(id, options, fn)`, `zebkit.media.keepHistory`, `probe`, `ffprobe(args)`, `ffmpeg(args, onProgress)` (on a worker thread; `false` from `onProgress` cancels) | `<zebkit/media.h>`, `media_tap.h`, `media_tools.h` |
| `zebkit.scheme.handle(name, request => request.respond(status, headers, body))`, `zebkit.cdp.listen(address, port)`, `.port`, `.url`, `.stop()` | schemes are registered with `--zebkit-scheme=NAME,standard,secure,fetch`; `<zebkit/zebkit.h>`'s CDP |
| `view.on('authentication', fn)`, `authenticationAction(action)`, `setAuthenticationUi`, `zebkit.app.onOpenFiles`, `onReopen`, `onQuitRequest` | WebAuthn's host UI; the application's events |
| `zebkit.crypto.aesGcmSeal(key, nonce, data, ad)`, `aesGcmOpen`; `canvas.measureText(text)`, `canvas.pixels(over)` | AES-GCM; text's advance and drawing over given pixels (`zebkit_canvas_wrap()`) |

`zebkit-js --zebkit-cookie-key-file=PATH` encrypts cookies with the
application's own 32-byte key, read from PATH (never an argument);
`--zebkit-cookie-storage` and the other startup settings are the engine's
switches. `scripts/check-bindings.py` checks that zebkit-js reaches every
function of the C API, listing what it reaches otherwise than by name.

Bytes are `Uint8Array`s both ways, without base64: the calls go to the
same code as a page's, in the same process. A function bound with
`bind()` is called with the page's arguments and `this.origin`, and may
return a promise. `zebkit-js` ends when the script calls `zebkit.quit()`,
or once nothing of it is pending (a timer, a call, the module's
evaluation) and no view is open, as Node ends.

Not yet: `import` of anything but files (no packages, no `import()` of a
computed name), and the web's other globals (`URL`, `structuredClone`).

## JavaScript libraries: `<zebkit/library.h>`

A bundled JavaScript library (an npm package made one ES module by
`tools/js-bundle.mjs`) loaded once and called from C, Rust, Python, Go or
host JavaScript, as any library is (`docs/JS_LIBRARIES.md`). It runs in a V8
isolate of its own, on a thread of its own: no `zebkit` object, no files, no
network but the origins given at load, imports only inside its folder; it has
`console`, timers, `queueMicrotask`, `TextEncoder`/`TextDecoder`,
`atob`/`btoa`, `URL`/`URLSearchParams`, `structuredClone`,
`crypto.getRandomValues`/`randomUUID`, `performance.now`, `Event`,
`EventTarget`, `AbortController`, `DOMException`, `WebAssembly`, `Intl`, and
`fetch()` with `Headers` and `Response`, without cookies.

```c
static void told(zebkit_library *library, zebkit_library_code code, const char *json,
                 const zebkit_part *parts, size_t part_count, void *data) {
    if (code == ZEBKIT_LIBRARY_OK) puts(json);           /* "\"<h1>Title</h1>\\n\"" */
    else fprintf(stderr, "%d: %s\n", code, json);        /* {"message", "name", "stack"} */
}

char *error = NULL;
zebkit_library *md = zebkit_library_load("libs/markdown", "{\"timeoutMs\": 5000}", NULL, NULL, &error);
zebkit_library_call(md, "render", "[\"# Title\"]", NULL, 0, 0, told, NULL);
/* ... */
zebkit_library_close(md);
```

| Function | What |
|---|---|
| `zebkit_library_load(path, options_json, ready, data, &error)` | A folder with `index.mjs`, or one `.mjs` file; returns at once, `ready` told once (`{"functions": [...]}`, LOAD_FAILED, or GONE if closed first). Options: `timeoutMs` (30000), `queue` (64), `heapLimitMB` (128), `maxBytes` (64 MiB), `network` (origins for `fetch()`), `backend` (`"isolate"`). NULL and `error` for bad options or path. The engine's thread |
| `zebkit_library_call(library, function, args_json, parts, part_count, timeout_ms, func, data)` | A JSON array of arguments (NULL is `[]`) and parts (`{"$part": n}` is `parts[n]`), copied before it returns; `timeout_ms` 0 the library's, < 0 none. The call's number, and `func` told exactly once after it returns; 0 when refused (NULL library or function, closed), never told. Any thread |
| `zebkit_library_cancel(library, call)` | The call told CANCELLED unless told already. Any thread |
| `zebkit_library_close(library)` | Calls not told yet told GONE after it returns; the handle freed after the last. The engine's thread |

Codes: OK, THROWN (with the library's `name`, `message`, `stack`),
LOAD_FAILED, NO_FUNCTION, BAD_ARGUMENTS, BAD_RESULT, TIMEOUT, CANCELLED,
QUEUE_FULL, OUT_OF_MEMORY, GONE. Calls run one at a time per library, in
order. At its deadline a running script is stopped (TIMEOUT; the library
ends, the other calls are GONE); a call waiting on a timer or `fetch()` is
TIMEOUT and the next call runs. A library whose heap passes `heapLimitMB` ends
with OUT_OF_MEMORY rather than ending the process.

| Binding | Load | Call | Values |
|---|---|---|---|
| Rust | `zebkit::js::Library::load(path, &Value)` (`ready()` a `Call`) | `call(name, args)`, `call_with(name, &args, parts, timeout)`: a `Call`, a `Future` of `Reply {value, parts}` or `LibraryError {code, message, name, stack}`; `.then(f)`, `.cancel()` | `json::Value` + `Vec<Vec<u8>>` |
| Python | `zebkit.library.load(path, options, ready=None)` (`.ready` a Future) | `call(name, *args, timeout_ms=0, function=None)`: a `zebkit.automation.Future`, awaitable in `zebkit.automation.run()`, or `function(value, error)` and the call's number; `cancel(future or number)` | values with `bytes` in place; `LibraryError` |
| Go | `zebkit.LoadLibrary(path, options, ready)` | `Call(name, args, parts, timeout, done)` (a callback), `CallChan(...)` (a channel); `Cancel(id)` | `json.RawMessage` + `[][]byte`; `*LibraryError` |
| Host JavaScript | `zebkit.Library.load(path, options)` (`.ready` a promise) | `call(name, ...args)`, `callWith(name, args, {timeoutMs})`: a promise; `cancel(promise)` | values with `Uint8Array` in place; errors with `.code` |

`node tools/js-bundle.mjs <entry.mjs | package[@version]> <folder> [--install pkg@ver] [--empty module] [--minify]`
makes the folder (`index.mjs`, `LICENSES.txt`) at build time;
`examples/js-libraries` bundles markdown-it and ECharts with it.

## Native interface: `zebkit.ui`

A script outside any page can make windows of native widgets instead of
web pages: Chromium's `ui/views`, the toolkit Chrome draws its own
interface with on Linux, Windows and macOS, already in the engine. The
model is Native Shell's and Solid's: components run once, signals update
the properties that read them, and the changes go to the native side as
one batch of mount operations per turn of the loop. No DOM, no GTK.

```js
// zebkit-js todo.mjs
const {h, signal, For, window} = zebkit.ui;

const [todos, setTodos] = signal([]);
const [draft, setDraft] = signal('');

window({title: 'To do', width: 420, height: 440}, () =>
  h('view', {class: 'flex-col gap-3 p-6 bg-gray-50'},
    h('text', {class: 'text-2xl font-bold'}, 'Things to do'),
    h('view', {class: 'flex-row gap-2 items-center'},
      h('input', {class: 'grow', placeholder: 'What needs doing?', value: draft,
                  onInput: setDraft, onSubmit: () => { setTodos((t) => [...t, draft()]); setDraft(''); }})),
    h(For, {each: todos}, (todo) => h('text', {}, todo)),
    h('text', {class: 'text-sm text-gray-500'}, () => `${todos().length} things`)));
```

![examples/ui/todo.mjs, captured with zebkit.ui.capture](images/zebkit-ui-todo.png)

`examples/ui/todo.mjs` is the whole example above, and the picture is it
captured headless with `zebkit.ui.capture()`.

| | |
|---|---|
| Elements | `view` (a flexbox), `text`, `button`, `input`, `image` (PNG, JPEG or WebP bytes as `src`), `scroll`, `web` (a web page, `url`) |
| Properties | `direction`, `gap`, `padding`, `align`, `justify`, `grow`, `width`, `height`, `background`, `radius`, `border`, `color`, `fontSize`, `fontWeight`, `textAlign`, `wrap`, `visible`, `enabled`, `text`, `value`, `placeholder`, `password`, `tooltip`, and for a window `title`, `width`, `height` |
| Classes | A subset of Tailwind: `flex-row`, `flex-col`, `grow`, `gap-*`, `p-*`, `px-*` and the like, `items-*`, `justify-*`, `bg-*`, `text-*` (size, colour, alignment), `font-*`, `rounded-*`, `border`, `w-*`, `h-*`, `hidden`; an unknown class is an error |
| Events | `onClick`, `onInput` (the text), `onSubmit`, and a window's `onClose` |
| Signals | `signal`, `computed`, `effect`, `batch`, `untracked`, `onCleanup`, `root` |
| Control flow | `Show({when, fallback}, ...)`, `For({each}, (item) => ...)`, keyed: an item's widgets are made once and moved |
| JSX | `h` and `Fragment`: compile with `--jsx-factory=zebkit.ui.h --jsx-fragment=zebkit.ui.Fragment` (esbuild); a property given as a function is reactive |
| Automation | `dump(node)` (the laid-out tree, with bounds), `click`, `type`, `submit`, `capture(node)` (PNG bytes, as the compositor shows it): for tests, and for agents, which can drive native interface as they drive pages |

A property given as a function is read in an effect: `text: () =>
\`${count()}\`` sets the text again when `count` changes, and nothing
else. Layout is views' box layout (one direction, gap, padding,
alignment, grow); a window's content fills it.

## Off-screen views

`<zebkit/offscreen.h>`: a view without a window, whose frames the
application draws itself and whose input it sends: a page inside a window
of another toolkit, or of another process. Electron's `offscreen: true`
with its shared texture, CEF's off-screen rendering.

```c
#include <zebkit/offscreen.h>

static void on_frame(zebkit_view *view, const zebkit_frame *frame, void *data) {
    /* frame->width x frame->height BGRA pixels at frame->scale, the whole
     * picture; frame->damage[0 .. damage_count) changed since the last. */
    upload(frame->pixels, frame->stride, frame->damage, frame->damage_count);  /* Linux */
    zebkit_frame_release(frame);                                               /* any thread */
}

static void ready(void *data) {
    zebkit_view *view = zebkit_view_new_offscreen("https://example.com/", 800, 600, 2);
    zebkit_view_on_frame(view, on_frame, NULL);
    zebkit_view_set_focus(view, true);
    zebkit_view_send_mouse(view, ZEBKIT_MOUSE_DOWN, 40, 30, ZEBKIT_BUTTON_LEFT, 1, 0);
}

int main(int argc, char **argv) { return zebkit_main(argc, argv, ready, NULL); }  /* run with --headless */
```

- **Colours.** Frames are sRGB on every system, whatever the display's colour
  space (an off-screen view's screen is sRGB; on a Mac the display's own would be
  Display P3), and untagged, as the page's CSS colours are the bytes.
- **Its screen.** `screen.*` and `devicePixelRatio` are the view's own: at its scale,
  and larger than it. A change of the system's scale does not reach it.
- **Where.** On Linux and macOS in any process, beside views with a window: no
  window of the system's is made for an off-screen view or for what its page
  opens, its scale is its own, and input reaches it from the application alone.
  Also in a process without windows (`--headless`, or Linux without a display), as
  an application's helper process that draws pages for it
  (`examples/offscreen-host`). On Windows only there; elsewhere
  `zebkit_view_new_offscreen()` returns NULL. `zebkit_view_new_offscreen_ex(context,
  url, width, height, scale, &error)` says why: another thread than the engine's, a
  closed context, the engine stopping, a process with windows on Windows, a Wayland
  process; the other forms log the reason.
- **Frames are shared surfaces, never encoded.** The engine captures the
  view's compositor frames with viz's `FrameSinkVideoCapturer`, as Chrome's
  tab capture and Electron's shared texture do: the GPU process copies a
  frame into a buffer of a pool, off the browser's main thread, and the
  application gets that buffer. On macOS the frame is read back from the
  GPU and the part that changed is copied into an IOSurface of a small
  pool of the engine's (viz's own IOSurfaces, `ZEBKIT_BUFFERS_GPU`, are
  not drawn into for a page in no window yet: `docs/MACOS.md`).

  | | Buffer | For another process |
  |---|---|---|
  | macOS | an IOSurface (`ZEBKIT_FRAME_IOSURFACE`) | `zebkit_frame_iosurface_mach_port()`: a send right, for a mach message; `IOSurfaceLookupFromMachPort()` there |
  | Linux | BGRA in shared memory (`ZEBKIT_FRAME_SHARED_MEMORY`): `pixels` here, `fd` (a memfd) to map | the fd, with `SCM_RIGHTS` |
  | Linux, `ZEBKIT_BUFFERS_GPU` | a dmabuf (`ZEBKIT_FRAME_DMABUF`): planes' fds, strides, offsets, the modifier, where the GPU makes them; shared memory where it does not | the fds, with `SCM_RIGHTS` |

  `buffer_id` names a buffer for as long as it lives, so a host maps it
  (or makes its texture of it) once; a pool is a few buffers, made again
  when the size changes.
- **What changed.** Every frame is the whole picture, with its sequence
  number, its size in pixels, its scale, and the rectangles that changed
  since the layer's previous frame: a small change of the page is a small
  copy for a host that keeps its own texture.
- **Given back.** Each frame is released once (`zebkit_frame_release()`,
  any thread, also after the view closed). While the application holds as
  many as `zebkit_view_set_max_frames_in_flight()` allows (2), the engine
  keeps only the newest of the rest, with the damage of those it dropped.
- **Only when something changes.** An idle page sends no frames;
  `zebkit_view_set_frame_rate()` limits a busy one;
  `zebkit_view_request_frame()` asks for one again.
- **Size and scale.** `zebkit_view_set_size(view, width, height, scale)`
  lays the page out again and the next frame has the new size, and the
  page's `devicePixelRatio` is the scale. The views are on the headless
  screen's display, which takes their scale: the off-screen views of a
  process share one, the last one given (a host window that moves to a
  screen of another scale sets it for all its views). On macOS it is
  whole (1, 2, 3), as a Mac's screens are.
- **Input** goes to the page's widgets as a window's events would:
  `zebkit_view_send_mouse()` (moves, presses with their click count,
  leaving), `zebkit_view_send_wheel()` (a trackpad's precise deltas with
  their phases, or a wheel's notches), `zebkit_view_send_key()` (down,
  the character typed, up, with the DOM's `code` and `key`),
  `zebkit_view_set_focus()`. An input method composes with
  `zebkit_view_ime_set_composition()`, `_commit()` and `_cancel()`, and
  places its window by `zebkit_view_on_caret()`.
  `zebkit_view_on_cursor()` gives the cursor the page asks for, by its CSS
  name, or its image.
- **Frames in the page** take their input as in a window, whatever their
  origin, transform or scrolling: the mouse and the wheel go through the
  page's input event router (Chromium's, as a window's events do), which
  finds the frame under the mouse, a frame of another site or a sandboxed
  one in a process of its own included, keeps a pressed button's frame
  until it is released (a selection drag), and keeps a scroll on the frame
  it began in; keys and an input method's text go to the focused frame,
  and a drag from outside (`zebkit_view_drag_enter()` and the rest) to the
  frame under it. The cursor is the one of the frame under the mouse
  (`tests/engine/frames_input.c`). Open on macOS: moves sent in the same
  task as the press that starts a selection in a frame of another process
  select only to the first of them; moves as a mouse sends them, even 2 ms
  apart, select as in Chrome.
- **Popups.** A `<select>`'s list, a date or colour picker is a widget of
  its own: `zebkit_view_on_popup()` says where it is in the view, its
  frames are `ZEBKIT_LAYER_POPUP` with that place in `bounds`, the
  application draws them over the page's, and input over that place (and
  the keys while it is open) goes to it. A press outside closes it. On
  macOS a `<select>` uses this widget rather than a menu of the system's.

| Functions | What |
|---|---|
| `zebkit_view_new_offscreen`, `_is_offscreen`, `_set_size` | A view without a window, its size and scale |
| `zebkit_view_on_frame`, `zebkit_frame_release`, `zebkit_frame_iosurface_mach_port` | Frames, given back; an IOSurface for another process |
| `zebkit_view_set_frame_rate`, `_set_max_frames_in_flight`, `_set_frame_buffers`, `_request_frame` | How many, how often, in what |
| `zebkit_view_send_mouse`, `_send_wheel`, `_send_key`, `_set_focus` | Input |
| `zebkit_view_ime_set_composition`, `_ime_commit`, `_ime_cancel`, `_on_caret` | An input method |
| `zebkit_view_on_cursor`, `_on_popup` | The cursor; popup widgets |

In Rust, `View::new_offscreen()` and the `zebkit::offscreen` module: a
`Frame` is `Send` and gives its buffer back when it is dropped.

### Another process

The process that draws needs the engine; the one that shows needs only a
socket. `examples/offscreen-host` is both, in C, over a reference protocol
(`protocol.h`): the host starts the renderer with one end of a
`socketpair()`; each frame is a message of 232 bytes that names its
buffer, with the buffer's fd (`SCM_RIGHTS`) the first time on Linux, and
on macOS the IOSurface's send right in a mach message to a port the host
registered (`bootstrap_check_in()`) the first time; the host answers
`RELEASE` for each frame and sends input and resizes the other way.
`scripts/engine-test.sh` runs it headless and checks the pixels the host
gets.

### In a host window

What a window of another toolkit does with an off-screen view, in the
renderer's process or another:

- **macOS.** Wrap the IOSurface in a `CVPixelBuffer`
  (`CVPixelBufferCreateWithIOSurface()`, once per `buffer_id`) and draw
  it: gpui's `paint_surface`, a `CALayer`'s `contents`, a Metal texture
  (`newTextureWithDescriptor:iosurface:plane:`). Draw it in `bounds`
  (CSS pixels, the frame `scale` times as many pixels). Release the frame
  once the GPU has drawn it (for a layer's `contents`, when the next frame
  replaces it).
- **Linux.** Map the fd once per `buffer_id` (`mmap(size, PROT_READ,
  MAP_SHARED)`, the pixels at `offset`, `stride` a row), copy the damage
  rectangles into the window's texture (BGRA, premultiplied), release the
  frame; or import the dmabuf as an EGLImage
  (`EGL_LINUX_DMA_BUF_EXT` with the modifier) and draw it.
- **Both.** Send the window's events in the view's CSS pixels; the focus
  when the window gains and loses it; the input method's composition and
  commit, its window placed at the caret; set the cursor the view asks
  for; draw a popup's layer over the page while it is shown; on a change
  of size or of the window's scale, `zebkit_view_set_size()`.

For a toolkit-owned window and loop, see `examples/native-host/`: GTK, Qt,
AppKit and winit each display web frames and a native PDF pane. Linux
examples have real X11 input regressions; the AppKit host draws both panes and
shuts down on a Mac (`docs/MACOS.md`), its input not driven.

Native key bindings can send their resolved editing commands with one DOWN
using `zebkit_view_send_key_with_commands(view, &key, commands, count)`.
Each `zebkit_edit_command` has a UTF-8 name and optional value (for example
`SelectAll`, then `InsertText` with Unicode text). Chromium dispatches one
DOM keydown, then executes its commands if the page did not prevent it.
Commands on UP/CHAR and more than 128 commands are rejected. Ordinary
`zebkit_view_send_key()` is unchanged. Rust provides `EditCommand` and
`View::send_key_with_commands`; the AppKit example collects selectors and
text-input actions before forwarding the event.

## Dialogs, PDF, drag and drop, find in page

`<zebkit/page.h>`: what a page asks of its window, for the application
(or a CDP client) to answer. Any view, with a window or off-screen.

```c
#include <zebkit/page.h>

static void on_dialog(zebkit_view *view, const zebkit_dialog *dialog, void *data) {
    /* alert, confirm, prompt, beforeunload, print: show it, answer later */
    if (dialog->held)
        zebkit_view_dialog_respond(view, dialog->id, true, "an answer");
}

static void printed(zebkit_view *view, const uint8_t *pdf, size_t size, const char *error, void *data) {
    /* the PDF's bytes, or NULL and the error */
}

zebkit_view_on_dialog(view, on_dialog, NULL);
zebkit_view_print_to_pdf(view, "{\"landscape\": true, \"printBackground\": true}", printed, NULL);
zebkit_view_drag_enter(view, "{\"files\": [\"/home/me/report.pdf\"]}", 120, 80, ZEBKIT_DRAG_COPY, 0);
zebkit_view_drop(view, 120, 80, 0, NULL, NULL);
```

- **Dialogs are held, not dismissed.** `alert()`, `confirm()`,
  `prompt()`, a beforeunload handler's "leave this page?" (on a
  navigation, a reload, or `zebkit_view_request_close()`, which closes a
  view as a tab is closed) and `window.print()` are reported to
  `zebkit_view_on_dialog()`'s function, with their type, message, a
  prompt's default text and the frame's URL, and the page waits until
  `zebkit_view_dialog_respond()` answers (but for print, which waits for
  nothing: answering only closes it). `zebkit_view_dialogs()` lists the
  open ones as JSON; `zebkit_view_on_dialog_closed()` hears how each
  closed.
- **CDP answers them too.** Every session with the Page domain gets
  `Page.javascriptDialogOpening` and `Page.javascriptDialogClosed`, and may
  answer with `Page.handleJavaScriptDialog`; the application hears of it
  as closed. Whoever answers first does.
- **Native Playwright actions.** A dialog that a native action opens
  (`zebkit_automation_call()`'s `Frame.click` whose handler calls
  `confirm()`) is told to the automation client as `BrowserContext.dialog`
  (`type`, `message`, `defaultValue`, `held`), and its close as
  `BrowserContext.dialogClosed` (`accepted`, `userInput`). Held for the
  application's function, a CDP client or `ZEBKIT_DIALOG_POLICY_HOLD`, it
  stays open for them even when the automation client has no listener; held
  for the automation client alone and without its listener
  (`Page.updateSubscription {event: "dialog"}`), it is dismissed (`"auto":
  true`), as Playwright does. The action itself is done once the dialog is
  answered, as in Chrome, where the page is held until then.
- **The policy is the view's, and may change at any time**
  (`zebkit_view_set_dialog_policy()`):

  | Policy | A dialog is |
  |---|---|
  | `ZEBKIT_DIALOG_POLICY_DEFAULT` | held for the application when it set a function; else held for a CDP client attached to the page, as headless Chrome holds it; else what the view's window has, as in Chrome: a sheet on macOS, Chromium's views dialog on Linux and Windows (prompt() with its text field), and the page waits for the person to answer it; an automation client listening for dialogs has them instead (dismissed at once without a handler, as Playwright does); in every off-screen view or `--headless` process nothing, dismissed at once (confirm false, prompt null, beforeunload leaves) |
  | `ZEBKIT_DIALOG_POLICY_HOLD` | always held until answered |
  | `ZEBKIT_DIALOG_POLICY_ACCEPT` | answered OK at once (confirm true, prompt its default, beforeunload leaves), and reported |
  | `ZEBKIT_DIALOG_POLICY_DISMISS` | answered Cancel at once (confirm false, prompt null, beforeunload stays), and reported |

- **Printing to PDF** is Chrome's: `zebkit_view_print_to_pdf()` and CDP's
  `Page.printToPDF` take the same parameters (landscape,
  displayHeaderFooter, printBackground, scale, paperWidth and paperHeight
  in inches, the margins, pageRanges, headerTemplate and footerTemplate,
  preferCSSPageSize, generateTaggedPDF, generateDocumentOutline; over CDP
  also `transferMode: "ReturnAsStream"`, read with `IO.read`). The page is
  laid out for print (`@media print`) and drawn into a PDF by Skia in the
  renderer, as headless Chrome does it, without a print compositor: frames
  of other sites in the page are left blank.
- **A page's drags.** `zebkit_view_on_drag_start()` gets each drag the
  page starts: what it carries as JSON (`text`, `html`, `urls`, `files`,
  `custom` types, an image's `fileContents`), the operations it allows,
  where it started, and its picture (BGRA) with the cursor's place in it.
  Return false and the engine runs it: in an off-screen view the mouse
  events the application sends are then the drag's, over the page
  (dragenter, dragover, drop, dragend), until the button is released or
  Escape is pressed, so that a sortable list or a drop zone works as in a
  window. Return true to run it yourself (the system's drag and drop) and
  end it with `zebkit_view_drag_source_end()`. A window's drags are the
  system's unless the application takes them.
- **Drags into the page.** `zebkit_view_drag_enter()`, `_drag_over()`
  (whose function gets the operation the page chose), `_drag_leave()` and
  `_drop()` bring what the application drags (files from the system's file
  manager by path, text, links, custom types) over the page with the
  keyboard's modifiers. A dropped file is the page's to read, as from the
  system's drag and drop; `zebkit.pathForFile()` knows its path.
- **Over CDP**, content's own: `Input.dispatchDragEvent`,
  `Input.setInterceptDrags` and `Input.dragIntercepted`, and drags of the
  mouse events `Input.dispatchMouseEvent` sends, which come before the
  application's.
- **Find in page** is Chrome's own (its find bar, `FindTabHelper` over
  content's `FindRequestManager`): `zebkit_view_find(view, "price", flags)`
  looks in the main document, in every frame (other sites' too) and in
  shadow trees, open or closed. Every match is highlighted, the active one
  distinctly, and the page scrolls to it. The same text again goes to the
  next match, with `ZEBKIT_FIND_BACKWARD` the previous, around the end;
  `ZEBKIT_FIND_MATCH_CASE` matches case and `ZEBKIT_FIND_HIGHLIGHT_ONLY`
  counts and highlights without an active match. As in Chrome, a new search
  starts in the focused frame. `zebkit_view_on_find()`'s function gets each
  result as JSON (`requestId`, `text`, `matches`, `activeMatch` from 1,
  `final`, `rect` of the active match, `stopped`), the count growing as
  frames answer and again when the page changes; `zebkit_view_find_result()`
  is the last. `zebkit_view_stop_finding()` clears the highlights, leaving
  the active match selected (`ZEBKIT_FIND_STOP_KEEP`) or activating it
  (`ZEBKIT_FIND_STOP_ACTIVATE`); navigating to another document stops a
  search. Over CDP, on a page's session: `Zebkit.find {text, matchCase,
  backward, highlightOnly}` → `{requestId}`, `Zebkit.stopFinding {action:
  "clear" | "keep" | "activate"}`, `Zebkit.getFindResult` → `{result}`, and
  `Zebkit.findResult` events, the same JSON, to the sessions that searched.

| Functions | What |
|---|---|
| `zebkit_view_on_dialog`, `_on_dialog_closed`, `_set_dialog_policy`, `_get_dialog_policy`, `_dialog_respond`, `zebkit_view_dialogs`, `zebkit_view_request_close` | JavaScript dialogs; closing as a tab |
| `zebkit_view_print_to_pdf` | The page as PDF |
| `zebkit_view_on_drag_start`, `_on_drag_end`, `_is_dragging`, `_drag_source_end` | The page's drags |
| `zebkit_view_drag_enter`, `_drag_over`, `_drag_leave`, `_drop` | Drags into the page |
| `zebkit_view_find`, `_stop_finding`, `_on_find`, `_find_result` | Find in page |

In Rust, `zebkit::page` and the `View` methods it adds (`on_dialog`,
`set_dialog_policy`, `respond_dialog`, `print_to_pdf`, `on_drag_start`,
`drag_enter`, `drop_data`, `find` (answering `Result<i32, Error>`),
`stop_finding`, `on_find` with a `FindResult`, ...); in Python `view.find()`
(raising `zebkit.Error` for a NUL, text that is not UTF-8 or the engine's
refusal), `stop_finding()`, `on_find()`, `find_result()`, with a
`zebkit.FindResult`; in Go
`View.Find`, `StopFinding`, `OnFind`, `FindResult`; in host JavaScript
`view.find(text, {matchCase, backward, highlightOnly})`, `stopFinding()`,
`findResult` and the `'find'` event.

## Media: what a page plays, saved as it plays

For real-time encoded packets, sampled RGBA frames and PCM, use
[`<zebkit/media_tap.h>`](MEDIA_TAP.md). A native tap can also record the same
encoded stream to a file. C, Rust `View::media_tap_start`, and Python
`zebkit.media_tap.MediaTap` expose it; MediaStream supports decoded output,
while original demuxed packets and lossless remuxing remain unavailable.

`<zebkit/media.h>`: a page's `<audio>` and `<video>` elements, and what
one of them plays copied into a file while it plays, as an image is
saved: the encoded audio and video the player's demuxer gives its
decoders, put into MP4, WebM or Matroska by the engine's FFmpeg without
decoding or encoding anything. Any view, with a window or off-screen.

```c
#include <zebkit/media.h>

static void on_progress(zebkit_media_capture *capture, const char *json, void *data) {
    /* {"state": "capturing", "bytes": ..., "duration": 12.0, "ranges": [[10, 22]], ...} */
}

static void on_done(zebkit_media_capture *capture, const char *json, void *data) {
    /* {"reason": "stopped", "files": [{"path": "/home/me/movie.mp4", "format": "mp4",
     *   "start": 10.0, "duration": 12.0, "gaps": [], "tracks": [...]}], "ranges": [...]} */
    zebkit_media_capture_free(capture);
}

static void listed(zebkit_view *view, const char *json, const char *error, void *data) {
    /* [{"id": "4.12.3", "kind": "mse", "codecs": {"video": "h264", "audio": "aac"}, ...}] */
    char *problem = NULL;
    zebkit_media_capture *capture =
        zebkit_media_capture_start(view, "4.12.3", "/home/me/movie.mp4", NULL, &problem);
    zebkit_media_capture_on_progress(capture, on_progress, NULL);
    zebkit_media_capture_on_done(capture, on_done, NULL);
    /* ... zebkit_media_capture_stop(capture) when the person says so */
}

zebkit_view_media_list(view, listed, NULL);
zebkit_view_media_at(view, 400, 300, at_point, NULL); /* the element under a right click */
```

- **Whatever the page delivers.** The copy is taken where the decoders
  read (`media::DemuxerTap`, `patches/1031`, `1032`), so it is the same
  for a file the element loads itself (`src=`, FFmpeg's demuxer), for what
  a DASH or HLS player script appends through Media Source Extensions
  (Chromium's `ChunkDemuxer`, the stream's own segments as they are
  parsed), and for HLS the engine plays itself. `kind` in the list says
  which: `file`, `mse`, `hls`; `stream` for cameras and WebRTC, which are
  not demuxed and cannot be captured.
- **What the player has, from the playhead on.** A capture starts with
  what the player holds already from the keyframe before the playhead
  (an MSE player's buffer ahead, FFmpeg's queue), then what it loads while
  it runs; `"includeBuffered": true` takes what it holds behind the
  playhead too. It never loads anything itself: the progress and the end
  report the ranges of the media captured, so that an application can say
  what was not played.
- **The stream's times, put in order when it stops.** Packets are kept in
  a file beside the destination as they come and written when the capture
  stops: each track in groups from a keyframe to the next, sorted by time,
  the same time appended twice (an adaptive player's new quality) kept
  once; a seek forward leaves a gap, closed up in every track at once when
  it is longer than `closeGaps` (1 s), and reported. Decode times are the
  stream's, or made from the presentation times as a muxer makes them
  (FFmpeg's demuxer gives the decoders none).
- **Without re-encoding.** H.264, HEVC, AV1, VP8, VP9, AAC, Opus, Vorbis,
  MP3 and FLAC as they are: H.264's and HEVC's parameter sets and AV1's
  sequence header taken from the stream where MSE has them in band, AAC
  without the ADTS headers Chromium adds. The container is the
  options' `format`, else the path's extension if it holds the codecs,
  else WebM for WebM's codecs, MP4 for MP4's, Matroska for the rest; the
  extension is made to match. A track whose configuration changes during
  a capture (another resolution) goes on in another file,
  `movie-2.mp4`, since a track of a file has one.
- **Encrypted media is refused**: zebkit has no CDM, and its copy would
  be of encrypted samples. The list says `"encrypted": true`.
- **Ends by itself** when the page goes, or the element loads other
  media; the files are written with what was captured. `maxBytes` (16 GiB
  by default) and `maxDuration` end it with an error, and the files of
  what it has. What a page sends is not trusted: more than 8 tracks, a
  track that changes kind or its configuration more than 32 times, and
  times beyond any media's end the capture or are dropped.
- **Nothing left behind, nothing replaced but the path**: the packets
  wait in a new file of its own while the capture runs (without a name
  outside Windows); the names a capture
  makes besides the path (`movie-2.mp4`) take `movie-2 (1).mp4` rather
  than replace a file. A capture running when the engine quits is not
  written, and does not hold it up.
- **No cost until media is looked for**: players keep the video since
  their last keyframe, for a capture to start at what is shown, only once
  `zebkit_view_media_list()` or `zebkit_view_media_at()` ran in their
  page's process. `zebkit_media_keep_history(true)` before
  `zebkit_main()` (or `--zebkit-media-history`; in Rust
  `zebkit::media::keep_history(true)` before `zebkit::run`) has them keep
  it from the start, so that a capture starts at the frame on screen
  whatever was asked before: up to about 16 MiB for each video stream
  played, held only while the application turns it on.
- **Over CDP too**, for any page target (the engine's views or pages a
  client made with `Target.createTarget`), in a domain of zebkit's own:
  `Zebkit.mediaList` → `{media}`, `Zebkit.mediaAt {x, y}` → `{media}`,
  `Zebkit.mediaCaptureStart {mediaId, path, options}` → `{captureId}`,
  `Zebkit.mediaCaptureStop {captureId}`, `Zebkit.mediaCaptureStatus
  {captureId}` → `{status}`, with the events `Zebkit.mediaCaptureProgress
  {captureId, state}` and `Zebkit.mediaCaptureDone {captureId, result}`
  (the JSON of the C API). A capture is the session's that started it:
  only it stops it and reads its status, and its captures stop (their
  files written) when it detaches. `Zebkit.mediaAt`'s point is in the
  view's coordinates, as `Input.dispatchMouseEvent`'s.

| Functions | What |
|---|---|
| `zebkit_view_media_list`, `zebkit_view_media_at` | The page's media elements, in every frame, as JSON; the one at a point |
| `zebkit_media_capture_start`, `_stop`, `_free` | A capture of what one plays |
| `zebkit_media_capture_on_progress`, `_on_done`, `_status` | Its state and its files |
| `zebkit_media_keep_history` | Before `zebkit_main()`: players keep what a capture starts from from the start |

In Rust, `zebkit::media`: `keep_history`, `View::media_list`, `View::media_at`,
`View::media_capture_start` and `MediaCapture` (`on_progress`, `on_done`,
`stop`, `status`; dropped, it is let go).

### Native media processing tools

For application-level FFmpeg/ffprobe functions (probe, remux, PCM extraction and
RGBA frame export), see [MEDIA_TOOLS.md](MEDIA_TOOLS.md). These synchronous
C/Rust/Python functions use the built-in FFmpeg without an external executable
or engine loop. The lower-level vendor API remains available.

## Automation: a screenshot's pixels

Playwright's commands run natively through `<zebkit/automation.h>`
([AUTOMATION.md](AUTOMATION.md)); `Page.screenshot` answers with a PNG,
JPEG or WebP in base64. When the application wants the pixels themselves,
`zebkit_automation_screenshot_pixels(view, "Page.screenshot", params, func,
data)` takes the same options (`clip`, `fullPage`, `scale`, `animations`,
`caret`, `mask`, `maskColor`, `style`, `omitBackground`, `timeout`; not
`type`, `quality` or `path`) and calls `func(view, pixels, width, height,
stride, error, data)` with the picture unencoded: `height` rows of `stride`
(= `width * 4`) bytes, each pixel R, G, B, A, 8 bits each, not
premultiplied: the pixels the PNG decodes to. They are the engine's for the
call only. `"ElementHandle.screenshot"` with the element's `"handle"` takes
an element's. Python: `await page.screenshot_pixels(full_page=True)` (also
`locator.screenshot_pixels()`, `element.screenshot_pixels()`) returns
`Pixels(width, height, stride, data)`; Rust:
`Page::screenshot_pixels(options_json, done)` and
`automation::screenshot_pixels(view, method, params, done)` give a
`Pixels { width, height, stride, data }`. This is zebkit's own, not
Playwright's API, which is unchanged.

## Loading the library

An application is linked with `libzebkit.so.1`, or loads it with `dlopen()`
(Python's `ctypes` does). The engine applies its own packed relocations
when it is loaded (`patches/1005`). Loading after host threads already
exist is supported on Linux: patch 1045 removes initialized TLS pointers
that could be copied before those relocations, including the thread-type
manager's vtable. The ELF audit and C/Python regressions cover main-thread
and pre-existing worker-thread engine loops; C runtime checks also pass on
glibc 2.17. On macOS late loading on the main thread passes on a Mac
(`docs/MACOS.md`). Keep the library loaded until process exit; unloading
Chromium and starting it again is not supported.

On Linux and macOS every child process is `zebkit-helper` (`Zebkit
Helper.app`), beside the library, whatever the application's executable is.
On Windows the children are the application's own executable, which calls
`zebkit_win_startup()` first (`<zebkit/win_sandbox.h>`, [WINDOWS.md](WINDOWS.md#the-sandbox));
`zebkit-helper.exe` only with `--no-sandbox`.

## Switches that are not an application's

`--use-fake-device-for-media-stream` and `--use-fake-ui-for-media-stream` are
Chromium's test switches: with them a page gets a fake camera and microphone with no
permission asked. `--isolated-context-origins` gives the origins it lists Chromium's
Isolated Context. An application should not pass on any of them from outside.
