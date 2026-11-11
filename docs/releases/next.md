# Unreleased: on `main` since 0.4.0

What `main` has that the 0.4.0 release does not (`git log v0.4.0..main`).
It goes into the next release's notes; until then nothing here is in a
release archive, on crates.io or on PyPI.

## Changes

**A license, and the notices in every archive.** `LICENSE` holds the GNU
LGPL 2.1 (`LGPL-2.1-or-later`), which the crates and the Python package
already declared; each crate, the Python package, the Go module and
`zebkit-mcp` carry it. From the next release on, every package and release
archive has `LICENSE`, `NOTICE` (the version, Chromium's version, the tag its
source is at) and `THIRD_PARTY_NOTICES.txt`, made by
`scripts/third-party-notices.py` (`docs/LICENSING.md`). 0.4.0's archives have
none of them.

**A Go binding.** `bindings/golang`, on cgo: all of the C API, on Linux,
macOS and Windows; Windows programs are cross-built with `cmd/zebkit-cc`, the
only route tested. Its checks (`internal/check*`) pass on all three, on macOS
and Windows with the sandbox on (`bindings/golang/README.md`).

**Every binding has the whole C API.** Python and host JavaScript
(`zebkit-js`) gained what they lacked: contexts, off-screen views with their
frames and input, `<zebkit/page.h>`, media capture and taps, the rest of PDF,
Playwright's API inside the engine and the rest of a view; Rust gained
contexts (`zebkit::Context`). `scripts/check-bindings.py` checks that each
binding reaches every exported function, and that each package's `LICENSE`
is a copy of `LICENSE`. `--zebkit-cookie-key-file` gives `zebkit-js` a cookie
key from a file.

**X11 windows: a real input method.** The engine is a client of IBus itself
(`src/engine/ime_ibus_linux.cc`): ibus-daemon, or fcitx5's IBus frontend,
found by `IBUS_ADDRESS` or the file ibus-daemon writes; composition drawn in
the page, candidates at the caret, password fields left alone. Before, X11
windows had none, and Chinese, Japanese or Korean could not be typed.
`tests/engine/input_method.py`.

**Chrome's focus ring.** A focused field had Blink's placeholder default, an
orange ring; zebkit sets Chrome's.

**Clipboard writes granted.** `navigator.clipboard.writeText()` and `write()`
after a click (`clipboard-write`) are granted by default, as Chrome grants
them. Reading the clipboard (`clipboard-read`) stays denied unless the
application permits it. This is a default that changed.

**Fixes.** A closing view's automation events no longer reach data an
application freed; `zebkit-js` keeps a view's hook until the task that closed
it has ended, and reports a promise rejected without a handler only once the
promise jobs have run. `zebkit-mcp` writes its address and its generated
token in one write.

**Behaviour of the C API that changed.** No function was added or removed;
an application built against 0.4.0's headers needs these looked at:

- `zebkit_post_task()`, `zebkit_post_delayed_task()` and `zebkit_view_attach()`
  return `bool`: false, and the task never run, for a NULL function, before
  the engine has started and once it has stopped; false for an attach to a
  NULL or closed view, to the view itself or to a parent with no window.
- Every `zebkit_view_*()` function is safe on a closed view, on NULL and off
  the engine's thread: it does nothing and returns its failure value. A
  callback the engine owes a view (an evaluation, a capture, a PDF, a media
  list, a drag's answer) is called exactly once, with an error if the view
  closes first, before its `on_close`.
- `zebkit_context_close()` posts the closing, as `zebkit_view_close()` does,
  so it may be called from any callback; the profile is freed when its pages'
  processes have gone.
- With no home directory to keep a profile in, the engine uses a private
  temporary one, deleted as it stops; a `--user-data-dir` that cannot be made
  fails the start (exit code 1).
- `zebkit_call_resolve(call, NULL)` resolves the page's promise with
  `undefined`; a call may be answered from any thread.
- A tray icon's clicks and its menu's choices reach the application in a
  task of their own, where it may close the tray or change the menu.
- The application menu's JSON is plain JSON, as the tray's and the Dock's:
  no comments or trailing commas; its functions are for the engine's thread
  and refuse others with an error.
- A page's PDF documents (`zebkit.pdf`) belong to the document that opened
  them (or to the script): another document, even of the same origin, cannot
  use them.
- `zebkit.fs`'s root, the directory granted, is resolved (its links followed)
  for each call, not once.
- The network hook's `headersReceived` metadata has `remoteAddress`, the IP
  address the response came from (`docs/ROADMAP_HOOKS.md`).
- A service worker's `clients.openWindow()` opens a view only through a page
  of the worker's own origin in the same context, with that view's popup
  options, handler and popup policy; none without such a page.

**The bindings' API.** Rust: `FrameBuffer` is `#[non_exhaustive]`, has
`D3D11(handle)` for a Windows texture, a `handle` in `SharedMemory` (the
file mapping on Windows), and `Dmabuf`'s planes as slices the frame lends
(`&[i32]`, `&[u32]`), with `Frame::format()` (`PixelFormat`); calls that can
be refused have `try_*` forms returning `Result` (`View::try_new`,
`try_attach`, the hook requests' `try_respond` and the like), and handlers
have `clear_*` methods. Python: a call off the engine's thread raises
`zebkit.Error` instead of reaching the engine; `zebkit.UNDEFINED`, returned
from a bound function, resolves the page's promise with `undefined`; network
events have `header_list`, every header in order; `Frame.memory()` gives a
shared-memory frame's pixels without a copy. Go: the binding was reworked to
Go's conventions (`bindings/golang/README.md`): package functions put the verb
first (`SetAppBadge`, `RegisterScheme`, `StopCDP`) and objects come from
`New...`; what the engine can refuse returns an `error` (`ErrRefused`,
`ErrClosed`, `ErrNotStarted`), constructors `(value, error)`; JSON goes in as
text or any value `encoding/json` encodes; a call off the engine's thread
panics, except what C lets any thread call.

**Tests and releases.** `mise run check` (`scripts/check.sh`) runs what a
release is published after: the static checks (also alone, `mise run
check:static`), `engine-test.sh --fetch`, the suite in strict mode,
`glibc-test.sh` and `web-bench.sh --check`, and records the pass; `mise run
r:github` publishes only after a strict pass on the same sources and package
(`ZEBKIT_RELEASE_UNCHECKED=1` publishes without and says so).
`engine-test.sh` has `--fetch` (the downloads some tests need, apart from the
run), `--strict` (`ENGINE_TEST_STRICT=1`: a skipped test or part of one
fails, as a test that exits 0 after printing `FAIL` lines does; the parts a
test's log names as skipped are collected and listed) with
`ENGINE_TEST_EXPECTED_SKIPS`, the skips it accepts (check.sh's short list),
and `--only` that fails when it matches no test; each test has a profile of
its own and the page server takes the port the system gives it. The drag
regions of `window_frame.py` are checked under the suite's Xvfb with real
drags (the window's move request seen at the root window).
`release.sh --publish-only` publishes what was built without building; a
version whose tag exists is never built or packed again (bump `VERSION`), an
interrupted upload's draft is named, and a release missing an archive's
`.sha256` is checked by GitHub's digest. `docker/publish.sh` lays out both
SDKs with `scripts/zsdk-create.sh all`, builds, checks, and publishes once the
checksums are committed. Heavy steps share one lock, `ZEBKIT_HEAVY_LOCK`.

## Not changed

Chromium is still 157.0.8103.38. Nothing was measured again against
Electron.
