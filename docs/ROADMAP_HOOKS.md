# Roadmap: hooks

How an application decides what the engine does, from native code, without
patching zebkit: a hook at each point where Chromium itself decides
(whether a page may have a permission, open a window, load a URL, send a
request, download a file, go past a bad certificate), with zebkit's
default when the application sets none (owner, 2026-10-24). The engine's
roadmap is `docs/ROADMAP.md`; this is its item "every setting changeable at
runtime", made general.

## What is a hook, and what is not

- **Hooks are for policy and decisions**: what an application may want
  differently from another application, or from one view to the next.
  Every hook has a default that holds without it, and the defaults are the
  ones zebkit has today: permissions denied, third-party cookies blocked,
  Chrome's user agent (with `--headless` too), popups blocked without a
  click, as Chrome blocks them.
- **Engine patches are for correctness**, which no hook can reach and no
  application should have to supply: V8's inspector reading a page's error
  without running the page's script, the screen of a headless view as big
  as the view, `window.chrome` as Chrome has it, `navigator.plugins` with
  the PDF viewer. These stay in `patches/series`.
- **One hook per decision.** A hook replaces the default for its view; it
  does not stack. An application that wants several parties to decide
  (its own code and an agent, say) decides among them in its own hook.

## How a hook answers

Chromium makes some decisions on the spot (synchronously, on the UI
thread, which is the engine's thread) and waits for others (it hands over
a callback). The C API follows Chromium, not the other way round:

- **Asked and answered later** where Chromium waits: the function gets a
  request (`zebkit_permission_request *`, as `zebkit_request *` for the
  application's scheme) and answers it now or later, from any thread
  through `zebkit_post_task()`. A request has a deadline (by default 30 s,
  per view): unanswered, it gets the default answer, and the request is
  reported as timed out. A request whose page went away is answered for
  the application, and answering it afterwards does nothing.
- **Set beforehand** where Chromium decides on the spot: the answer is a
  setting of the view (a table by origin, a value, a flag), which the
  application changes at any time. A callback would block the engine's
  thread, so there is none; there is an event after the fact.
- **Told** where nothing is decided: an event (a console message, a
  finished download, a response).

Every hook's function runs on the engine's thread, as every zebkit
callback does. Hooks are per view (`zebkit_view_on_*`, `zebkit_view_set_*`),
and those that exist before any view, such as certificate errors on a
service worker's fetch, are for the engine (`zebkit_on_*`).

## The hooks

| Hook | Chromium's decision point | Chromium | zebkit today | Kind |
|---|---|---|---|---|
| Permissions | `PermissionControllerDelegate::RequestPermissionsFromCurrentDocument`, `GetPermissionStatus` (`ShellPermissionManager`, `patches/1023`) | request waits; status on the spot | denied, but a screen wake lock | asked; status set |
| Popups and new windows | `ContentBrowserClient::CanCreateWindow` (with `user_gesture`), `WebContentsDelegate::IsWebContentsCreationOverridden`, `AddNewContents` | on the spot | blocked without a click, as Chrome blocks them; `zebkit_view_on_new_window` takes the URL of one that opens, `zebkit_view_on_window_open` the window as a view with its opener (`patches/1027`) | set, then told |
| Navigation and redirects | `ContentBrowserClient::CreateThrottlesForNavigation` (`NavigationThrottleRegistry`): a `NavigationThrottle`'s `WillStartRequest`, `WillRedirectRequest`, `WillProcessResponse` | waits (`DEFER`, then `Resume` or `CancelDeferredNavigation`) | allowed | asked |
| Network requests and responses | `ContentBrowserClient::WillCreateURLLoaderFactory` (a factory in front of the network's, as Chrome's webRequest has), `CreateWebSocket` | waits | `zebkit_view_set_network_filter`, `zebkit_view_on_network`, `zebkit_view_on_network_event`, `zebkit_view_on_network_body`: nothing held or told without a filter; WebSocket handshakes and, by `zebkit_view_set_network_filter_for_workers`, shared and service workers covered | asked; told |
| User agent and client hints | `ContentBrowserClient::GetUserAgent`, `GetUserAgentMetadata`; per view `WebContents::SetUserAgentOverride` | on the spot | Chrome's for 155, `--headless` too; `zebkit_view_set_user_agent` sets the string | set |
| Downloads | `DownloadManagerDelegate::DetermineDownloadTarget`, `InterceptDownloadIfApplicable`; `WebContentsDelegate::CanDownload` | waits | `zebkit_view_on_download`, `zebkit_view_on_download_progress`; without a function saved in `Downloads` in the profile's directory | asked; told |
| JavaScript dialogs, beforeunload | `JavaScriptDialogManager::RunJavaScriptDialog`, `RunBeforeUnloadDialog` | waits | done: `zebkit_view_on_dialog`, policy per view (`<zebkit/page.h>`) | asked |
| Certificate errors | `ContentBrowserClient::AllowCertificateError`; client certificates `SelectClientCertificate` | waits | refused; client certificates `zebkit_view_on_client_certificate`, none sent by default | asked |
| Sign-in (HTTP authentication) | `ContentBrowserClient::CreateLoginDelegate` | waits | cancelled | asked |
| Links to other programs (`mailto:`, `zoommtg:`) | `ContentBrowserClient::HandleExternalProtocol` | on the spot | not opened; `zebkit_view_on_external_protocol` tells | told |
| Console messages | `WebContentsDelegate::DidAddMessageToConsole` | told | `zebkit_view_on_console` | told |
| Scripts at document start | `ContentRendererClient::RunScriptsAtDocumentStart`; isolated worlds as CDP's `Page.addScriptToEvaluateOnNewDocument` with `worldName` | in the renderer, before the page's script | `zebkit_view_add_script`, `zebkit_view_add_world_script` (named world, all frames or main frame) | set |
| Settings of a view | `ContentBrowserClient::OverrideWebPreferences`, `WebContents::NotifyPreferencesChanged` | on the spot | Chromium's defaults; `zebkit_view_set_settings` | set |
| File choosers | `WebContentsDelegate::RunFileChooser`, and File System Access's pickers through it (`UseFileChooserForFileSystemAccess`, `patches/1084`) | waits | `zebkit_view_on_file_chooser`, `"api"` `input` or `file-system-access`; without a function the engine's: cancelled where it has no dialog | asked |
| Camera, microphone, screen | `WebContentsDelegate::RequestMediaAccessPermission` | waits | `zebkit_view_on_media_access` for camera and microphone; without a function the permission controller; screen refused | asked |
| Keys before the page | `WebContentsDelegate::PreHandleKeyboardEvent` | on the spot | `zebkit_view_set_reserved_keys`, `zebkit_view_on_reserved_key` | set, then told |
| Context menu | `WebContentsDelegate::HandleContextMenu` | told | `zebkit_view_on_context_menu`; without a function the system's menu with a window (`docs/ROADMAP.md`) | told |
| Fullscreen | `WebContentsDelegate::CanEnterFullscreenModeForTab`, `EnterFullscreenModeForTab`, `ExitFullscreenModeForTab` | on the spot | allowed; `zebkit_view_set_fullscreen_policy`, `zebkit_view_on_fullscreen` | set, then told |

What exists in narrow form is made one of these, not kept beside them:
`zebkit_view_on_new_window` becomes the popup hook's event,
`zebkit_view_set_user_agent` the user agent hook's string, `zebkit_view_add_script`
the main world of the document-start hook, `zebkit_scheme_handle` the
network hook for the application's own scheme, and the dialog, print and
drag functions of `<zebkit/page.h>` (`patches/1024`, `1025`) and the
off-screen frames (`patches/1021`) stay as they are, as hooks already.

## Each hook

- **Implemented: document permissions.** `<zebkit/hooks.h>` provides
  origin/default view rules, a side-effect-free status query, asynchronous
  requests, replies and outcome events. Requests include permission,
  requesting/top origin, user gesture and deadline. Reply once with prompt,
  granted or denied; `remember` stores an origin rule. Navigation, destruction,
  handler replacement and deadlines settle outstanding requests. Late replies
  return false; tokens are never reused. The deadline uses a monotonic clock,
  including when the host blocks the engine thread.
  `navigator.permissions` subscriptions use Chromium's actual controller.
  Later context/CDP mutations supersede older view rules in Chromium's own
  permission scope; a later explicit view rule applies only to that view.
  There is no parallel CDP permission cache. Resetting view rules preserves
  context overrides; resetting context permissions supersedes older rules.
  Worker permissions use context/CDP policy: document view rules do not
  redefine the policy of shared or service workers. Document security,
  fenced-frame restrictions and Permissions Policy remain enforced.
  MIDI denial also denies SysEx; SysEx grants also grant MIDI. Approximate-only
  geolocation cannot be persisted separately. Opaque requesting origins cannot
  be remembered. Rules are bounded to 4096 per view and pending requests to
  4096 per engine; invalid batches do not partially update policy.
  Camera/microphone device capture now uses this controller and device IDs
  selected by Chromium. Display/tab capture still needs a host chooser.
  Linux regression coverage includes real location requests with synthetic
  coordinates, subscriptions, OOPIF, timeouts, blocked callbacks, once/remember,
  handler replacement, navigation, closure, context isolation and native/CDP
  ordering. Platform runtime and additional permission-specific checks remain.
- **In progress: popups and new windows.** `<zebkit/hooks.h>` now has a
  per-view popup policy and an asynchronous blocked-popup event, with Rust,
  Python and host JavaScript bindings. Linux runtime tests cover two views,
  runtime changes, invalid values, gestures and startup-switch precedence;
  Python, Rust and host JavaScript tests cover the bindings.
  Native decision/created metadata and creation snapshots are implemented and
  pass Linux/macOS compilation checks. C/Rust/Python/host JavaScript bindings
  and integration tests pass on Linux, including immediate closure, cross-origin
  source identity and adoption of the original child in host JavaScript.
  Platform runtime validation remains open.
  `CanCreateWindow` is answered from the
  view's popup policy: allowed with a click and blocked without, as
  Chrome's blocker does (`ZEBKIT_POPUP_POLICY_BLOCK_WITHOUT_GESTURE`, the
  default), always, or never. A window that is allowed goes to the
  application with its URL, the frame name, the features and whether it
  is a popup or a tab (`IsWebContentsCreationOverridden`), and the
  application opens it as a view of its own, keeping `window.opener` when
  it wants (`AddNewContents` with the new contents), or elsewhere; a
  blocked one is reported, so that it can be offered.
- **In progress: native document-console events.** The C observer is connected
  directly to Chromium's WebContentsObserver and reports real frame identity,
  origin, severity, message, source and an optional untrusted exception stack.
  Asynchronous queues and payloads are bounded; handler replacement and closure
  cancel pending deliveries. Linux/macOS compilation and Rust binding checks
  pass. Existing bindings pass Linux runtime tests for frame isolation,
  truncation, handler replacement and closure. Worker events and platform
  runtime validation remain open.
- **In progress: document certificate errors.** The native BrowserClient hook
  provides asynchronous single-use requests, metadata/PEM chains, deadlines and
  outcome events. Denial is the default; strict errors cannot be continued.
  Top-level navigation, stopping, closure and handler replacement invalidate
  requests. C/Rust/Python/host JavaScript interfaces and local TLS tests are
  implemented. Native compilation and real Linux TLS tests pass through C,
  Rust, Python and host JavaScript. Patch 1051 prevents an earlier per-view
  allow decision leaking through pooled HTTP/1.1 or HTTP/2 connections; tests
  verify denied requests send no HTTP headers or body to the server.
  Requests without a WebContents,
  finer subframe cancellation and platform validation remain open.
- **Implemented: document navigation throttle hooks.** C, Rust, Python and
  host JavaScript receive start, redirect, response, failure and loader-less
  commit stages. Requests have stable navigation/frame IDs, URL, method,
  gesture, referrer, redirect chain, response status and duplicate-preserving
  headers. Response header output is bounded and reports truncation.
  Reply once to continue, cancel, or cancel and start a GET of a replacement
  URL in the same frame. Unanswered stages continue after the configurable
  deadline (default 30 seconds); invalid replies preserve pending requests.
  Superseding, closing and cancellation invalidate old tokens. Replacing or
  removing a handler continues its pending stages. Native/Python real HTTP
  tests cover cancellation before network IO and before commit, redirect
  responses, POST, subframe replacements, headers, timeout, blocked callbacks,
  handler replacement, wrong-thread replies and closure. Rust's full suite
  passed 75 checks; host JavaScript's real navigation test passed.
  BFCache restoration and prerender activation use an additional commit-deferring
  `activation` stage with `activationType`. Real Linux tests verify continue,
  cancel, replacement, timeout, handler removal, closure and superseding
  navigation for both paths. Activation referrer metadata is unavailable before
  Chromium initializes it and is explicitly marked as such. Renderer-only
  same-document policy remains open. Internal inspection uses tab targets and
  preserves initialization-script ordering, Disposable removal and native UA
  state across prerender activation. Tests verify real request headers, the
  document's first script and worker UA values. Chromium's own content-layer
  throttles remain registered; full native UA metadata and cross-client override
  precedence are still separate work.
- **Partial: network requests and responses.** `network_hooks.cc`: a factory in
  front of the network's for a view's frames and their dedicated workers, installed
  once a view has a filter (`zebkit_view_set_network_filter`), and for frames and their running
  dedicated workers there already through `UpdateSubresourceLoaderFactories` (`patches/1063`).
  Rules choose by URL glob, resource type, frame and stage (request or response), and either hold
  or observe. Held requests are blocked, redirected, answered with bytes, a file or a stream, or
  continued with changed headers; held responses get changed headers, or are blocked or replaced.
  Unanswered or with the function replaced, they go on; with no policy left, refused. Observing
  holds nothing. CDP's Fetch sits behind the hook; a test has it see the hook's header.
  Added 2026-10-31 (`hook_policies.py`, every mode in C, Rust, Python and JavaScript):
  - **WebSockets** (`CreateWebSocket`): the handshake is a request of type `websocket`; a rule
    without types holds it too. Continue, with headers the network would not drop, or block;
    redirect and fulfil are refused. Frames are not filtered. A dedicated worker's socket looks like
    its frame's. A socket of a service or shared worker has no profile from content: it goes to the
    one profile with a policy, and with two it is refused.
  - **Shared and service workers:** `zebkit_view_set_network_filter_for_workers`. That view's filter
    decides for its profile's workers, including their scripts and those already running. Once on,
    when that view goes, the profile's worker requests are refused until another takes over or
    `off` is said. Their factories are always proxied, so without a policy each such request
    pays one more mojo hop.
  - **Response bodies:** a rule with `"body": true`, `zebkit_view_on_network_body`: chunks of up to
    64 KiB as they pass to the page, the next read only when the last is written, so never held
    whole; told up to 64 MiB a response, then `truncated` once while the page's stream goes on. One
    `end` per response, before its finished or failed event.
  - **Answers from a file or a stream:** `zebkit_network_respond_file`, read off the engine's
    thread, no range; `zebkit_network_respond_stream`, with a 1 MiB buffer, write false when full or
    gone (`zebkit_network_stream_is_open`), `zebkit_network_stream_on_writable`, no sniffing.
  - **`redirect: "manual"` and `"error"`** fetches now work when the application redirects them,
    as DevTools' does.
  - **The headers the network adds** (`header_client`, `kURLLoadOptionUseHeaderClient`, `patches/1069`,
    `tests/engine/network_stages.py`): three stages inside the network, each hop. `sendHeaders` has the
    Cookie, before the transaction; a rule holds it, and the application may change or remove any
    header or block. `sent` has the headers as sent, with what the transaction adds after that
    (`Authorization`, `Proxy-Authorization`, `Host`, `Connection`), and only observes, as DevTools'
    `Network.requestWillBeSentExtraInfo`; nothing can change those any more, in Chrome either.
    `headersReceived` has the raw response before the cookie store; a removed `Set-Cookie` stores
    nothing. Rules here fail closed: no function, no policy, too many held, no answer in time or the
    function replaced each end the request with `ERR_BLOCKED_BY_CLIENT`, where the page's stages go on.
    A preflight is a request of its own there (`preflight`, type `preflight`), as in DevTools' Network;
    CDP's Fetch does not hold one. Only with such a rule does a request use the client.
  - **WebSockets at these stages** (2026-11-02): a rule of `sendHeaders`, `sent` or `headersReceived`
    for type `websocket` gives the network's handshake a header client of the hooks', as extensions'
    webRequest does (`network::WebSocket` asks it as a loader's): the Cookie the network adds is seen
    and can be changed or the socket blocked at `sendHeaders`; the `101` and its headers are seen at
    `headersReceived`. The network does not tell a handshake's `sent` headers, so that stage has none
    for a socket. `network_stages.py` has a WebSocket server of its own for it.
  Not done: a rule of these stages set after a request started does not reach that request, nor one
  answered by the HTTP cache at `sent`, which sends nothing. Early hints,
  prefetch and background fetch factories do not pass the hook, so a 103's preload gets past a rule.
  Handshake outcomes of WebSockets are not told. A service worker's socket in a two-profile engine
  is refused rather than decided. As CDP's `Fetch.fulfillRequest`, an answered request skips CORS
  and does not store `Set-Cookie`, and a response's changed headers come after the network's CORS check.
  No backpressure bound on bodies can be observed from a page's fetch: Blink's reads ahead itself.
- **Implemented foundation: user agent and client hints, per view.**
  `zebkit_view_set_user_agent_override` sets the string, optional language,
  platform and `userAgentMetadata` together using Chromium's validated CDP
  parameter format. Only successful updates persist into future targets. C,
  Python, Rust and host JavaScript interfaces are present. Real Linux tests
  cover first-script values, brands, full versions, platform, architecture,
  model, bitness, WoW64, form factors, Accept-CH request headers and workers;
  rejected updates preserve prior values. External CDP sessions retain their
  own scopes. Platform runtime and wider target/context coverage remain open.
- **Implemented: downloads** (`patches/1057`, `download_hooks.cc`). Asked with URL,
  suggested name, type and size; saved at the absolute path given or cancelled;
  progress and the end told; `zebkit_view_cancel_download` ends a running one.
  Unanswered they are cancelled. Without a function they are saved in `Downloads`
  in the profile's directory on every system, without a prompt (`patches/1059`),
  a file already there kept as Chrome keeps it (`report (1).bin`). "Opened by the
  application from its bytes" is not implemented.
- **JavaScript dialogs and beforeunload: done** (0.0.1-alpha.6,
  `<zebkit/page.h>`), the model for the others: held for the application,
  answered by it or by a CDP client, a policy per view.
- **Partial: sign-in.** Per-view HTTP authentication (`CreateLoginDelegate`)
  exposes realm, challenger, proxy and navigation metadata through single-use
  requests with deadlines and result events. C, Rust, Python and host JavaScript
  pass real HTTP challenges; native tests also cover proxy 407. Global requests
  without a WebContents remain open. Chromium's context auth cache and earlier
  CDP interception retain their existing scopes.
- **Implemented: links to other programs.** `HandleExternalProtocol` opens nothing
  and tells the application, which opens the link or not; the page stays. As Chrome,
  fenced frames and sandboxed frames without the permission are not told. The table
  said "set": there is nothing to set, since the engine never opens one itself.
- **Console messages:** see the native document-console events above. Worker
  messages remain open.
- **Implemented: scripts at document start, in isolated worlds.**
  `zebkit_view_add_world_script` uses CDP's `worldName`, for every frame, those of
  other sites' processes too (their targets are attached once such a script exists),
  or the main frame only, by a guard around the script: its top-level `let`, `const`
  and `class` are then not global. Chromium gives these worlds universal access, so
  a world can read other origins, unlike a content script. Not done: `zebkit.*` for
  a world and not for its page (`zebkit_view_grant` with a world). The browser cannot
  tell which world a call to `zebkit.*` came from, so that needs more than a flag.
- **Settings of a view: implemented** (2026-10-29). `<zebkit/hooks.h>`
  exposes `zebkit_view_set_settings(view, json, error)` and
  `zebkit_view_get_settings`. Changes merge by top-level key; null restores
  Chromium's default, {} does nothing, and an invalid batch changes nothing.
  They are reapplied through `OverrideWebPreferences` on recomputation and
  sent to every renderer with `NotifyPreferencesChanged`.
  - Booleans: `javaScript`, `images`, `remoteFonts`, `plugins`,
    `spatialNavigation`, `smoothScrolling`, `textAutosizing`, `localStorage`,
    `hyperlinkAuditing`, `reducedMotion`, `reducedTransparency`, `hideScrollbars`.
  - Integer font sizes (up to 512): `defaultFontSize`, `defaultFixedFontSize`
    (at least 1), `minimumFontSize`, `minimumLogicalFontSize` (at least 0).
  - `fonts`: standard/fixed/serif/sansSerif/cursive/fantasy/math, each a family
    string for Common or a dictionary of four-letter ISO 15924 script codes
    to family strings. A family is nonempty, without NUL, at most 1024 bytes.
    Replacing `fonts` replaces that entire override dictionary.
  - `defaultEncoding` is validated and canonicalized; `autoplay` is allow,
    gesture or document-activation; `colorScheme` is light/dark; `contrast`
    is no-preference/more/less/custom. The JSON object is at most 64 KiB.
  - C, Rust `View::set_settings/settings`, Python `set_settings/settings`,
    and host JavaScript `setSettings/settings` are tested. The getter returns
    explicit native preferences, not temporary CDP emulation; Chromium's CDP
    emulation continues to apply separately. Parser scripts do not execute
    retroactively when JavaScript is re-enabled; applications reload when
    their content needs that.
  - Linux checks cover atomic rejection, two views, scripts, images including
    live re-enabling, proportional/fixed fonts, autoplay allow/deny, main and
    cross-site iframe media queries, runtime changes and reset. C/host JS pass,
    Rust passes 61 integration checks, and the complete engine suite passes.
    macOS framework/package and C hooks ARM64 cross-link pass; runtime remains.
  Engine-wide settings: `zebkit_set_engine_settings`, `zebkit_view_set_runtime_settings` (2026-10-31, `docs/API.md`).
- **Implemented: file choosers, camera and microphone, keys, context menu,
  fullscreen** (`patches/1058`, `page_hooks.cc`), as `<zebkit/hooks.h>` says. Asked
  ones end when their document goes. Permissions Policy still refuses a frame before
  the media function is asked. Not done: screen and tab capture, still refused. The
  page still gets the keyup of a reserved key, as with Chrome's accelerators.
- **Implemented: client certificates, the system's stores included.** Asked with
  host, port and the server's accepted authorities; answered with a PEM chain and key
  checked against each other, with none, or by failing the request. Unanswered,
  the request fails without Chromium remembering anything. Known limit: Chromium
  remembers an answer for that host and port for the whole context, so a second view
  is not asked, and a default of none holds even after a function is set. **The operating
  system's stores** (2026-11-03, built as designed below): with the engine setting
  `clientCertificateStores` (off by default), `CreateClientCertStore` gives Chromium's store of the
  platform, the request's `identities` describe what matches the server's authorities, and
  `zebkit_client_certificate_choose` (`choose` in the bindings) continues with the identity's key,
  acquired from the store. Tested by `tests/engine/hook_policies.py python:certstore` (an NSS
  database made in the test's home by `tests/engine/nss_import.c`; a keychain of the test's own on
  macOS; the user's store on Windows, removed after). Not offered: NSS keys behind the database's
  password. The design as it was written: Chromium's own stores are in the
  network library already built into the engine: `net::ClientCertStoreNSS` (Linux, the
  user's NSS database, `~/.pki/nssdb`), `ClientCertStoreMac` (Keychain identities) and
  `ClientCertStoreWin` (the user's `MY` store). `ContentBrowserClient::CreateClientCertStore`
  returning the platform's makes `SelectClientCertificate` arrive with the store's identities
  matching the server's authorities; the request's metadata would list each (subject, issuer,
  validity, SHA-256 fingerprint, the store's name) and a new answer, `identity` with an index,
  would call `ClientCertIdentity::AcquirePrivateKey` and continue with that key, which stays in
  the store (NSS, Keychain or CNG signs; the engine never sees it). Off by default, by an engine
  setting, so that nothing of the system's is offered to a page without the application asking.
  Cost: about 300 lines in `client_certificate.cc` and `engine.cc`, the answer in the four
  bindings, and tests per platform: on Linux an NSS database made in the test's HOME with
  `certutil`/`pk12util`; on macOS a test keychain (the suite already answers Keychain calls from
  files); on Windows a certificate imported into the test user's store. NSS may ask for a
  database password (`PasswordDelegateFactory`): the first version would offer only identities
  whose keys need none, and Keychain may show its own access prompt the first time, which a
  headless host cannot answer: both said in `<zebkit/hooks.h>`. Two to three days with the macOS
  and Windows test runs.

## In the bindings

The hooks of `page_hooks.cc`, `download_hooks.cc`, `client_certificate.cc` and
`network_hooks.cc` have the same shape in each language: Python `view.on_download`,
Rust `View::on_download`, host JavaScript `view.onDownload`, each with a request's
`metadata`, `info` and reply. `tests/engine/hook_policies.py` runs the same modes
through all four on Linux; popup inheritance, two views, a running download's
cancellation and the order with CDP's Fetch run through Python only.

- **C**: `zebkit_view_on_<hook>(view, func, data)` and
  `zebkit_view_set_<setting>(view, ...)` in `<zebkit/hooks.h>`; a request
  type per asked hook, with `zebkit_<hook>_respond()` and accessors, as
  `zebkit_request` and `zebkit_view_dialog_respond()` are now. JSON where
  a structure would change between Chromium versions.
- **Rust**: `View::on_permission`, `PermissionRequest::respond(state,
  remember)`, `info` and retained metadata; requests are engine-thread objects.
  `View::set_permission`, `permission`, `reset_permissions`, deadline and
  handler cleanup methods wrap the same policy. Async convenience remains open.
- **Python**: `view.on_permission(function)` receives `PermissionRequest`;
  `request.respond('granted', remember=True)` replies. `request.info` becomes
  `None` after settlement; `metadata` remains a snapshot. Passing `None`
  removes a handler; post worker replies onto the engine thread.
- **JavaScript** (`zebkit-js`): `view.onPermission(fn)` receives a request with
  `metadata`, `info` and `respond(state, remember)`; `onPermission(null)` removes
  it. `view.on('permission-result', fn)` observes outcomes. `setPermission`,
  `getPermission`, `resetPermissions`, `setPermissionTimeout` and `setPopupPolicy`
  configure host policy. Pages never see these hooks.
- **CDP and MCP**: what CDP already decides (Fetch, `Browser.setPermission`,
  `Emulation.setUserAgentOverride`, `Page.setDownloadBehavior`) keeps
  working in its own scope, as a CDP client's override does in Chrome: it
  does not write the settings a hook reads. The MCP server
  (`docs/ROADMAP.md`, "zebkit as an MCP server") sets the same policies
  for an agent, within the bounds whoever runs it sets.

Navigation bindings are `View::on_navigation` with
`NavigationRequest::respond(NavigationAction)` in Rust;
`view.on_navigation(fn)` and `request.respond('redirect', url)` in Python;
and `view.onNavigation(fn)` with `request.respond('redirect', url)` in host
JavaScript. Each exposes a metadata snapshot and pending `info`; the view
also exposes a navigation deadline and outcome observer. C and Rust return
native error details; Python/JavaScript replies return false for stale or
invalid native replies. All requests run on the engine thread.

## Tests

Every hook gets a test in `tests/engine/`, in C and through the Rust and
JavaScript bindings, against a local page that asks for what the hook
decides: the default without a function; each answer; an answer given
later; the deadline; the page closing while the request is held; two
views with two policies at once; and, for the settings, that a page and
its workers see the same. `scripts/web-bench.sh --check` holds the
defaults, as it holds the privacy defaults now. A hook set and answered
quickly costs nothing a page can measure, which `tests/bench/web/`
checks for the network hook.

## Order

1. Popups (Chrome's blocker as the default), permissions and certificate
   errors: they change what pages can do today.
2. Network requests and responses, and navigation: what agents and
   applications ask for most.
3. User agent metadata per view, settings of a view, downloads, sign-in,
   links to other programs.
4. Isolated worlds, console messages, file choosers, media, keys,
   fullscreen.
