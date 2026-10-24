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
| Popups and new windows | `ContentBrowserClient::CanCreateWindow` (with `user_gesture`), `WebContentsDelegate::IsWebContentsCreationOverridden`, `AddNewContents` | on the spot | blocked without a click, as Chrome blocks them; `zebkit_view_on_new_window` takes the URL of one that opens | set, then told |
| Navigation and redirects | `ContentBrowserClient::CreateThrottlesForNavigation` (`NavigationThrottleRegistry`): a `NavigationThrottle`'s `WillStartRequest`, `WillRedirectRequest`, `WillProcessResponse` | waits (`DEFER`, then `Resume` or `CancelDeferredNavigation`) | allowed | asked |
| Network requests and responses | `ContentBrowserClient::WillCreateURLLoaderFactory` (a factory in front of the network's, as Chrome's webRequest has), `CreateWebSocket` | waits | the application's scheme only (`zebkit_scheme_handle`) | asked; told |
| User agent and client hints | `ContentBrowserClient::GetUserAgent`, `GetUserAgentMetadata`; per view `WebContents::SetUserAgentOverride` | on the spot | Chrome's for 155, `--headless` too; `zebkit_view_set_user_agent` sets the string | set |
| Downloads | `DownloadManagerDelegate::DetermineDownloadTarget`, `InterceptDownloadIfApplicable`; `WebContentsDelegate::CanDownload` | waits | the content shell's: `Downloads` in the profile's directory | asked; told |
| JavaScript dialogs, beforeunload | `JavaScriptDialogManager::RunJavaScriptDialog`, `RunBeforeUnloadDialog` | waits | done: `zebkit_view_on_dialog`, policy per view (`<zebkit/page.h>`) | asked |
| Certificate errors | `ContentBrowserClient::AllowCertificateError`; client certificates `SelectClientCertificate` | waits | refused | asked |
| Sign-in (HTTP authentication) | `ContentBrowserClient::CreateLoginDelegate` | waits | cancelled | asked |
| Links to other programs (`mailto:`, `zoommtg:`) | `ContentBrowserClient::HandleExternalProtocol` | on the spot | not opened | set, then told |
| Console messages | `WebContentsDelegate::DidAddMessageToConsole` | told | not reported (`Shell::DidAddMessageToConsole`) | told |
| Scripts at document start | `ContentRendererClient::RunScriptsAtDocumentStart`; isolated worlds as CDP's `Page.addScriptToEvaluateOnNewDocument` with `worldName` | in the renderer, before the page's script | main world only: `zebkit_view_add_script` | set |
| Settings of a view | `ContentBrowserClient::OverrideWebPreferences`, `WebContents::NotifyPreferencesChanged` | on the spot | Chromium's defaults | set |
| File choosers | `WebContentsDelegate::RunFileChooser` | waits | the content shell's: cancelled where it has no dialog | asked |
| Camera, microphone, screen | `WebContentsDelegate::RequestMediaAccessPermission` | waits | refused (`NOT_SUPPORTED`) | asked |
| Keys before the page | `WebContentsDelegate::PreHandleKeyboardEvent` | on the spot | the menu's shortcuts | set, then told |
| Context menu | `WebContentsDelegate::HandleContextMenu` | told | the system's menu with a window (`docs/ROADMAP.md`) | told |
| Fullscreen | `WebContentsDelegate::EnterFullscreenModeForTab`, `ExitFullscreenModeForTab` | on the spot | the window's | set, then told |

What exists in narrow form is made one of these, not kept beside them:
`zebkit_view_on_new_window` becomes the popup hook's event,
`zebkit_view_set_user_agent` the user agent hook's string, `zebkit_view_add_script`
the main world of the document-start hook, `zebkit_scheme_handle` the
network hook for the application's own scheme, and the dialog, print and
drag functions of `<zebkit/page.h>` (`patches/1024`, `1025`) and the
off-screen frames (`patches/1021`) stay as they are, as hooks already.

## Each hook

- **TODO: permissions.** A request (`zebkit_view_on_permission`) carries
  the permission, the requesting origin, the top origin and whether a click
  came first, and is granted, denied or dismissed, once or for the origin.
  The status a page reads (`navigator.permissions.query()`) comes from the
  view's table (`zebkit_view_set_permission(view, origin, permission,
  state)`), which a granted request fills; what is not in it is
  `patches/1023`'s answer. CDP's `Browser.setPermission` writes the same
  table. Location, sensors, NFC, MIDI, payment handlers, idle detection and
  background work stay denied by default.
- **TODO: popups and new windows.** `CanCreateWindow` is answered from the
  view's popup policy: allowed with a click and blocked without, as
  Chrome's blocker does (`ZEBKIT_POPUP_POLICY_BLOCK_WITHOUT_GESTURE`, the
  default), always, or never. A window that is allowed goes to the
  application with its URL, the frame name, the features and whether it
  is a popup or a tab (`IsWebContentsCreationOverridden`), and the
  application opens it as a view of its own, keeping `window.opener` when
  it wants (`AddNewContents` with the new contents), or elsewhere; a
  blocked one is reported, so that it can be offered.
- **TODO: navigation.** A throttle on every navigation of a view's frames
  asks the application at the start, at each redirect and at the response
  (URL, method, frame, whether a click started it, the response's status
  and headers), and the answer is go on, cancel, or cancel and load
  another URL. Chrome's own throttles (safe browsing among them) are not
  in the content layer; this is where an application puts its own.
- **TODO: network requests and responses.** A loader factory in front of
  the network's gives the application each request of a view's frames and
  workers before it is sent and each response before the page has it:
  headers changed, the request blocked, redirected or answered by the
  application (bytes, a file, or a stream), the response's headers
  changed, and everything observed without being held (`observe` mode,
  which costs no round trip). Only what the application asks for is held:
  a filter by URL pattern, resource type and frame, as CDP's
  `Fetch.enable` takes. A request the application does not answer within
  the deadline goes on. CDP's Fetch and Network domains keep working
  beside it: the hook sees requests before them.
- **TODO: user agent and client hints, per view.** The string, and the
  client hints that go with it (brands, full versions, platform and its
  version, architecture, model, mobile), set together
  (`zebkit_view_set_user_agent_metadata` with JSON, as
  `Emulation.setUserAgentOverride` takes `userAgentMetadata`), so that the
  two never disagree. Workers of the view's pages send the same; checked
  as `tests/bench/web/privacy.html` checks the engine's.
- **TODO: downloads.** A download is asked about with its URL, the
  suggested name, the type and the size when known, and is saved where the
  application says, opened by the application from its bytes, or
  cancelled; progress and the end are told. Without a function, downloads
  go to `Downloads` in the profile's directory, as today.
- **JavaScript dialogs and beforeunload: done** (0.0.1-alpha.6,
  `<zebkit/page.h>`), the model for the others: held for the application,
  answered by it or by a CDP client, a policy per view.
- **TODO: certificate errors.** Asked with the error, the certificate's
  chain (PEM), the URL and whether it is the main frame, and answered
  proceed or refuse, for this request or for the host until the view
  closes. Refused by default. Client certificates are chosen the same way
  (`SelectClientCertificate`), none by default.
- **TODO: sign-in.** HTTP authentication (`CreateLoginDelegate`) asked
  with the realm, the host and whether it is a proxy, and answered with a
  user and password or cancelled, as Chrome's dialog does.
- **TODO: links to other programs.** A URL of a scheme the engine does not
  load goes to the application (`HandleExternalProtocol`), which opens it
  with the system or not; nothing is opened by default.
- **TODO: console messages.** Each message of a view's pages and workers:
  level, text, source URL, line. Told only, and taken from Chromium's
  console as it is (`DidAddMessageToConsole`), never by serializing the
  page's objects, so that listening changes nothing a page can see.
- **TODO: scripts at document start, in isolated worlds too.** A script
  run in every document of a view before the page's own, in the main world
  (`zebkit_view_add_script`, today) or in a world of its own name that
  shares the page's DOM and not its globals, as Electron's preload with
  context isolation does and Chrome's content scripts do; for every frame
  or the main frame only. `zebkit.*` can be given to an isolated world and
  not to the page (`zebkit_view_grant` with a world).
- **TODO: settings of a view.** What Chromium's `WebPreferences` holds and
  an application changes: JavaScript, images, fonts and font sizes,
  default encoding, autoplay, spatial navigation, smooth scrolling, the
  minimum font size, plugins, `prefers-color-scheme` and the other media
  features, text autosizing; set per view (`zebkit_view_set_settings` with
  JSON), applied at once (`NotifyPreferencesChanged`).
- **TODO: file choosers, camera and microphone, keys, context menu,
  fullscreen**: as the table says; the context menu is the one in
  `docs/ROADMAP.md` ("What a window does besides").

## In the bindings

- **C**: `zebkit_view_on_<hook>(view, func, data)` and
  `zebkit_view_set_<setting>(view, ...)` in `<zebkit/hooks.h>`; a request
  type per asked hook, with `zebkit_<hook>_respond()` and accessors, as
  `zebkit_request` and `zebkit_view_dialog_respond()` are now. JSON where
  a structure would change between Chromium versions.
- **Rust**: `View::on_permission(|request| ...)`, the request answered by
  `request.grant()`, `request.deny()` or kept and answered later; an
  `async` form beside it.
- **JavaScript** (`zebkit-js`): `view.on('permission', async (request) =>
  'grant')`, the promise being the answer, and `view.set('popups',
  'block-without-gesture')`. Pages never see hooks: they are the
  application's.
- **CDP and MCP**: what CDP already decides (Fetch, `Browser.setPermission`,
  `Emulation.setUserAgentOverride`, `Page.setDownloadBehavior`) keeps
  working and writes the same settings a hook reads, so that an agent and
  the application never disagree about what is in force. The MCP server
  (`docs/ROADMAP.md`, "zebkit as an MCP server") sets the same policies
  for an agent, within the bounds whoever runs it sets.

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
