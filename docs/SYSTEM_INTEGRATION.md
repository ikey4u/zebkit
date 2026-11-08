# System integration

Every place where the engine meets the operating system or shows something
to a person: a window, a menu, a dialog, a permission, a device, a
notification, the system's theme. For each: what decides there, what
happens when the application says nothing, how the application takes it
over, and what is missing. This document is the design those decisions
follow, and the inventory against which "complete" is measured.

Status: a design, 2026-11-08. Statements about today's behaviour are
checked against `main` (zebkit 0.2.1, Chromium 155) and the Chromium 157
branch, and name the file and symbol that show it; line numbers drift,
symbols do not. Everything under "Design" is proposed and not yet built.

Related: `docs/ROADMAP_HOOKS.md` (how a decision is handed to the
application; this document does not replace it), `docs/UI.md` (`zebkit.ui`,
the engine's own small UI), `docs/API.md` (the reference),
`docs/ROADMAP.md` (the engine's roadmap, where the phases in section 17 go).

## 1. The goal, and what it is not

An application that embeds zebkit can decide everything the engine does in
front of the system and the person, without patching the engine, and gets
a sensible behaviour where it decides nothing. "Decide" has a precise
meaning here. For each surface, the application can:

1. **leave it**: the default, documented and tested;
2. **configure it**: change the default's parameters (a window's frame, a
   download directory);
3. **decide it**: answer the decision itself, per request (a hook);
4. **present it**: show its own UI for it, and give the engine the result;
5. **turn it off**, where it is optional;
6. **observe it**: be told what happened, including what the system refused;
7. **go back**: restore the default.

Not every surface has all seven; the inventory (section 5) says which it
has. A callback that only reports is not "decide", and a Chromium switch
is not an API: switches stay an escape hatch with no stability promise.

What this is not:

- **Not Chrome's product UI.** No tab strip, omnibox, settings pages,
  account sync or extension store. Where Chrome has UI the content layer
  lacks (a screen-share picker, a device chooser, print preview), zebkit
  gets a hook and, where a default is worth its size, a small one of its
  own drawn with `zebkit.ui` or the system's own dialog.
- **Not a way around the system.** Where macOS asks the person for the
  camera, or Wayland forbids a window placing itself, the application can
  choose when to ask and what to say first, not the answer. The system's
  refusal is reported, never hidden.
- **Not a framework for building applications on.** zebkit is a library
  applications integrate (`docs/UI.md`, "What it is"). The host keeps its
  loop, windows and toolkit; this design makes sure the engine never takes
  them from it.

## 2. Principles

These are fixed; the rest of the document follows from them.

1. **Today's behaviour is the default.** An application built against the
   current API sees no change: no hidden migration of menus, permissions,
   profiles or windows. A new entry point may default to Chrome's
   behaviour where today's differs (section 7.6 is the one case so far);
   the old entry point keeps today's.
2. **One decision, one owner.** As for hooks: a decision has one function
   that answers it, per view or for the engine; others observe the result.
   No chains of handlers competing for one answer.
3. **The content layer's own mechanism first.** Where Chromium has a way
   to do something (draggable regions, Window Controls Overlay,
   `SelectFileDialog`, the XDG portals), zebkit uses it, and patches
   Chromium where it falls short, rather than emulating it beside Chromium.
   Chrome's behaviour is the reference for what is correct.
4. **The same function on every system.** A surface exists on Linux,
   macOS and Windows with the same meaning. Where a system cannot do it,
   the capability says so and the call fails with a reason; it never
   succeeds silently, and never falls back silently to something else.
5. **The page has nothing by default.** Every surface here is for the
   application's trusted code (C, Rust, Python, host JavaScript). A page
   gets a piece of it only through a grant the application gives an
   origin, as `zebkit_view_grant()` gives `zebkit.*` today.
6. **Settings, decisions, presentation and execution are separate.** A
   setting is a preference (a window's frame). A decision is allow, deny
   or delegate (may this page have the camera). Presentation is how it is
   shown (the system's dialog, the application's, none). Execution is the
   system call that does it. Replacing the presentation of a file chooser
   does not change who may read files, and a menu item does not grant the
   command it runs.
7. **Size and speed are kept.** Every surface added follows the project's
   rule: it may not cost more size, in percent, than it gains in speed,
   unless it is a feature the engine requires. A surface that needs a
   large dependency is a hook with no default.

## 3. Terms

| Term | Meaning |
|---|---|
| Surface | One place where the engine meets the system or the person: "the file chooser", "a window's title bar", "web notifications". Each has a stable identifier, such as `dialog.file-chooser`. |
| Default | What happens with nothing configured. Documented per surface, held by a test. |
| Presenter | What shows a surface to the person: `system` (the system's own UI), `engine` (`zebkit.ui` or Chromium's views), `application` (the host, through a hook), or `none`. |
| Capability | Whether this build, on this system and display server, can do something at all: `overlay` frames on Wayland, a notification service on this desktop. Asked, never guessed from the OS name. |
| Requested / effective | What the application asked for and what holds. The engine's runtime settings report both already (`zebkit_get_engine_settings()`: `explicit` and `effective`); every surface's getter does the same. |
| Owner | Who owns the application object or a window: the engine or the host (section 4). |

## 4. Who owns what

zebkit runs in two very different hosts, and every surface is defined for
both.

**The application.** On macOS, zebkit makes the `NSApplication` delegate,
the menu bar and the Dock's behaviour only when asked
(`applicationIntegration` in `zebkit_init_with_settings()`,
`startup_settings.cc`); `zebkit_main()` uses its own. A host with its own
`NSApplication` keeps it. Elsewhere there is no application object; the
menu's keys work in the engine's windows (`zebkit_app_set_menu()`).

**The window.** A view is in one of five places:

| Placement | Made by | Frame, title bar, buttons | Window actions (minimize, fullscreen, close) |
|---|---|---|---|
| Engine window | `zebkit_view_new[_in]()` | the engine's, configurable (section 7) | the engine's, asked by the application or the person |
| Attached to a view | `zebkit_view_attach()` | the parent view's window | the parent's; refused for the child |
| In the host's window | `zebkit_view_attach_native()` | the host's | the host's; the engine never touches the parent |
| Off screen | `<zebkit/offscreen.h>` | the host's, which draws the frames | the host's |
| No window | `--headless`, or no display | none | reported unsupported |

Rules that follow:

- A surface that needs a window (a native dialog's parent, a context menu)
  uses the view's top-level window: `zebkit_view_get_native_window()` and,
  for portals, `zebkit_view_export_native_window()`, both of which exist.
  With none, the default is the one written for "no window", never a
  dialog parented nowhere.
- The engine never changes a window it does not own: no style, no
  fullscreen, no focus stealing in the host's window.
- Window options given to a view that is later attached are kept and
  applied again when it is detached (`zebkit_view_detach_native()`); if
  that is impossible on a system, detaching restores the system frame and
  `effective` says so.

## 5. The surfaces today

Status: **done** (a default and a way for the application to decide or
present), **partial** (some of it), **default only** (works, nothing to
configure), **gap** (missing or refused for lack of an implementation).
"Hook" names are of `<zebkit/hooks.h>` and `<zebkit/page.h>`.

### 5.1 Application

| Surface | Today | Status |
|---|---|---|
| `app.lifecycle`: ready, quit request, reopen, files opened | `zebkit_main()` ready callback, `zebkit_app_on_quit_request`, `zebkit_app_on_reopen` (macOS), `zebkit_app_on_open_files` (macOS from Finder and Dock; elsewhere program arguments) | done on macOS; partial elsewhere (no single-instance hand-over of files: by design the host's) |
| `app.open-url`: a URL the system gives the application (`myapp://`) | not received | gap |
| `app.menu`: the application's menu | `zebkit_app_set_menu()` with roles; the macOS menu bar; elsewhere the items' keys only, no menu bar | partial: no checked/enabled state, no update by id, no menu bar on Windows and Linux |
| `app.dock`: Dock menu, badge, progress; taskbar on Windows | none | gap |
| `app.identity`: name, icon, app id (`wm_class`, AppUserModelID) | `wm_class` from the program name (`window_views.cc`); bundle metadata on macOS | partial |

### 5.2 Windows

| Surface | Today | Status |
|---|---|---|
| `window.frame` | `system`, `overlay`, `none` by `zebkit_view_new_with_options()` (`<zebkit/window.h>`): macOS a full-size content view under a transparent title bar, the system's buttons; Views a frame of zebkit's with engine-drawn buttons (`window_mac.mm`, `window_views.cc`) | done |
| `window.show` | shown once its page is in it, on macOS too; `show: false` makes it hidden until the `show` action | done |
| `window.geometry` | size, place (`x`, `y`; refused on Wayland), limits by the options; `zebkit_view_get_window()` state, actions, `zebkit_view_on_window_state()` events | done |
| `window.title` | `zebkit_view_set_title`, the page's by default | done |
| `window.focus` | `zebkit_view_focus`; the page's own focus request (`ActivateContents`) | done |
| `window.fullscreen` | a page's fullscreen fills the content area, or with `pageFullscreen: "window"` makes the window fullscreen as Chrome does; the `fullscreen` actions; the policy hook still decides | done |
| `window.drag-regions`, `window.controls-overlay` | the page's `app-region` while its main frame is of a granted origin, or `zebkit_view_set_drag_regions()`; an overlay's title bar area sent with `UpdateWindowControlsOverlay` (`navigator.windowControlsOverlay`, `env(titlebar-area-*)`, `display-mode: window-controls-overlay`) | done |
| `window.close` | the close box, `window.close()`, `zebkit_view_close`, `zebkit_view_request_close` (beforeunload first) | done |
| `window.popup`: `window.open`, `target=_blank` | blocked without a gesture, as Chrome; `zebkit_view_on_new_window`, `zebkit_view_on_window_open`, the popup policy | done |
| `window.web-controls`: the page's `window.minimize()`, `maximize()`, `restore()`, `setResizable()` (Additional Windowing Controls) and `moveTo`/`resizeTo` | Chromium's defaults: refused or ignored (`MinimizeFromWebAPI` and the others not overridden) | gap |
| `window.picture-in-picture`: video and document picture-in-picture | `EnterPictureInPicture` returns not supported | gap |

### 5.3 Input

| Surface | Today | Status |
|---|---|---|
| `input.reserved-keys` | `zebkit_view_set_reserved_keys`, `zebkit_view_on_reserved_key` | done |
| `input.context-menu` | `zebkit_view_on_context_menu` takes it with its context; without one, `ViewDelegate::ShowContextMenu` has a null inner delegate and shows nothing (`view.cc`, `engine.cc`) | partial: no default menu, no way to run a menu's command |
| `input.drop` | `zebkit_view_on_drop` (paths before the page has the drop), the drag API of `<zebkit/page.h>` | done |
| `input.pointer-lock` | the `pointer-lock` permission: granted by default where there is a window, as before; refused without one (`--headless`, off screen), where there is no system pointer | done |
| `input.keyboard-lock` | the `keyboard-lock` permission, denied by default (`Window::RequestKeyboardLock`) | done |
| `input.global-shortcuts` | none | gap; host's toolkit by preference |

### 5.4 Dialogs and choosers

| Surface | Today | Status |
|---|---|---|
| `dialog.javascript`: alert, confirm, prompt, beforeunload | `zebkit_view_on_dialog`, a policy per view; default presenter: `NSAlert` on macOS, `MessageBoxW` on Windows (where `prompt()` returns null), none on Linux, none without a display (`js_dialog_manager.cc`, `dialog.cc`) | done; the defaults differ by system |
| `dialog.file-chooser`: `<input type=file>` | `zebkit_view_on_file_chooser`; default `ui::SelectFileDialog` (NSOpenPanel, the Windows dialog, the XDG portal on Linux) | done |
| `dialog.file-system-access`: `showOpenFilePicker()`, `showSaveFilePicker()`, `showDirectoryPicker()` | through `zebkit_view_on_file_chooser` (`"api": "file-system-access"`, modes open, save, directory; `patches/1084`); the page reads what was picked and writes where it saves; writing to an opened file, or a handle beyond what was picked, is the `file-system-write` or `file-system-read` permission, denied without a hook (`file_system_access.cc`) | done |
| `dialog.color`: `<input type=color>` | Blink's in-page popup (`ColorChooserPopupUIController`) | default only |
| `dialog.eye-dropper`: `EyeDropper.open()` | feature on, `OpenEyeDropper` not overridden: the promise rejects; the colour popup's eyedropper button does nothing | gap |
| `dialog.date-time` | Blink's in-page chooser | default only |
| `dialog.select`: `<select>` popups | macOS windows: a native `NSMenu`; macOS off-screen views and Views: Blink's in-page popup (`offscreen.cc` forbids external popups) | default only |
| `dialog.http-auth` | `zebkit_view_on_http_auth`; cancelled without one | done (no default presenter) |
| `dialog.client-certificate` | `zebkit_view_on_client_certificate`; none sent without one | done (no default presenter) |
| `dialog.webauthn` | `zebkit_view_on_authentication`, or the engine's UI (Views, AppKit); system passkeys on macOS and Windows (`webauthn.cc`) | done |
| `dialog.print`: `window.print()` | no dialog and no printer: reported to the application as a dialog, the page continues; `zebkit_view_print_to_pdf` for output (`print.cc`) | partial: no physical printing, no preview |
| `dialog.repost`: "send the form again?" on reloading a POST | `ShowRepostFormWarningDialog` not overridden: Chromium's default, the form is not sent again | default only |
| `dialog.fedcm`: FedCM sign-in (`navigator.credentials.get({identity})`) | no dialog controller and no permission contexts: not available | gap |

### 5.5 Permissions, devices and capture

| Surface | Today | Status |
|---|---|---|
| `permission.*` (geolocation, clipboard, sensors, MIDI, ...) | `zebkit_view_on_permission`, a table per view; default denied, but a screen wake lock (`permission_manager.cc`) | done |
| `permission.geolocation.system` | Apple's location source on macOS; none elsewhere (`client_defaults.cc`) | partial |
| `media.camera-microphone` | `zebkit_view_on_media_access`, else the permission table | done |
| `media.display-capture`: `getDisplayMedia()` | refused: no chooser (`window.cc`, `permission_manager.cc`) | gap |
| `device.usb`, `device.hid`, `device.serial`, `device.bluetooth` | no delegate: the services refuse or report not supported (`web_usb_service_impl.cc`, `hid_service.cc`, `serial_service.cc`, `bluetooth_device_chooser_controller.cc`) | gap |
| `device.smart-card` | no delegate: not available | gap |
| `media.audio-output`: `selectAudioOutput()` | Chromium's default | default only |
| `media.system-controls`: the system's now-playing and media keys | Chromium's default | default only |
| `media.speech`: speech recognition and synthesis | zebkit's recognition delegate; Chromium's synthesis | default only |
| `clipboard`: reads and writes | the permission table for reads; Chromium's defaults for policy | default only |
| `protocol-handler`: `registerProtocolHandler()` | the `protocol-handler` permission, with the scheme and the handler's URL, denied without a hook; a handler granted handles its links in the profile; nothing is registered with the system, and nothing says it is (`protocol_handler_delegate.cc`) | done |

### 5.6 Notifications and the desktop

| Surface | Today | Status |
|---|---|---|
| `notification.web`: `new Notification()`, a service worker's `showNotification()` | denied: `Profile::GetPlatformNotificationService` is null and `Notification.permission` reads "denied" (`permission_manager.cc`, `profile.cc`) | gap |
| `notification.push` | `GetPushMessagingService` null | gap (needs a push service; likely out of scope) |
| `desktop.tray`: status item, tray icon | none | gap |
| `desktop.share`: `navigator.share()` | not exposed (Blink's `WebShare` flag is for tests, no `ShareService`) | gap |

### 5.7 Appearance, locale, accessibility

| Surface | Today | Status |
|---|---|---|
| `appearance.color-scheme` | light by default, as before; with the startup setting `appearance.follow` the system's colour scheme, contrast and forced colours and their changes (`NativeTheme`, on Linux the XDG portal's `color-scheme`, `appearance.cc`); per view `colorScheme` and `contrast` in `zebkit_view_set_settings` still win; `zebkit_get_environment()` and `zebkit_on_environment_changed()` | done |
| `appearance.reduced-motion`, forced colours | through automation's media emulation only | partial |
| `locale` | the system's, from Chromium 157 on (`patches/1083`); `Accept-Language` from it | done (157 branch) |
| `accessibility.platform` | the trees when an assistive technology asks: on macOS VoiceOver's `AXEnhancedUserInterface`, `AXManualAccessibility` and a read of the application's role (`application_mac.mm`, as Chrome's), elsewhere content's detection; the environment's `accessibility` says which; no client reads AT-SPI, NSAccessibility or UI Automation on the test machines | partial: the trees' reading is not tested |

### 5.8 Files, other programs, network

| Surface | Today | Status |
|---|---|---|
| `download` | `zebkit_view_on_download` and progress; default `<profile>/Downloads`, no dialog, never opened (`download_manager.cc`) | done |
| `external-protocol`: `mailto:` and others | `zebkit_view_on_external_protocol` tells; nothing opened | done (the application opens it) |
| `network.proxy`, `network.certificates`, `network.client-certificates` | startup `proxy`; certificate and client-certificate hooks; the system's trust store | done |
| `storage.cookie-keys` | the OS key store, a key file, or a host key (`zebkit_set_cookie_storage`, `zebkit_set_cookie_encryption_key`) | done |
| `devtools`, `cdp` | off unless asked (`zebkit_cdp_listen`, the switch); 127.0.0.1 by default | done |

### 5.9 Power, session, displays

| Surface | Today | Status |
|---|---|---|
| `power.wake-lock` | the screen wake lock granted; system wake lock via Chromium's device service | default only |
| `power.events`: suspend, resume, lock, battery | Chromium's power monitor inside the engine; nothing told to the application | gap |
| `display.list`: screens, scale, work area, changes | Chromium's `display::Screen` inside; nothing told | gap |

## 6. The common contract

What every surface follows, so that an application learns it once.

### 6.1 Defaults

Each surface's default is written in its row of the registry (6.6), with
the test that holds it. A default may depend on the placement (section 4)
and on capabilities, never on anything else: not on the build, not on an
environment variable the application cannot see.

Where a default presenter exists, it is the system's own UI if the system
has one (a file dialog, an alert), else the engine's (`zebkit.ui`, Chromium
views) if small, else none, and the request gets its "no presenter"
answer: denied for a permission, cancelled for a chooser, refused for an
external action. That answer is part of the surface's definition; "wait
forever" is never one.

### 6.2 Decisions: the hook model, unchanged

Decisions use `docs/ROADMAP_HOOKS.md` as it is: asked and answered later
where Chromium waits, a setting where Chromium decides on the spot, an
event where nothing is decided; a deadline per request; requests of a page
that went away answered for the application. Additions:

- **"Use the default" as an answer.** A hook that wants to handle some
  requests and not others answers the rest with `default`, which runs the
  default presenter for that request once. The application's function is
  not called again for it.
- **Reasons of the end.** Every request's result event names how it ended:
  `answered`, `default`, `timed-out`, `cancelled` (the page or view went
  away, or the handler was removed), `unsupported`, `system-denied` (the
  system refused, as macOS's camera prompt can).
- **Handler replaced.** Removing or replacing a function cancels its
  pending requests with `cancelled`; it never leaves them hanging. This is
  already the rule for WebAuthn (`zebkit_view_on_authentication`).

### 6.3 Configuration

Configuration is JSON at the C boundary, as the settings functions are
now: validated whole, applied whole or not at all, unknown keys and bad
values an error naming the key. Inside the engine it is typed structures;
platform code never parses JSON.

Every field belongs to one of four lifetimes, written beside it:

| Lifetime | Set | Changed later |
|---|---|---|
| startup | `zebkit_init_with_settings()` | never: needs a restart |
| context | `zebkit_context_new()` (later with options) | never: needs a new context |
| creation | the view's options (section 7.3) | `requires-recreate` error |
| runtime | the surface's setter | yes; the field says whether it changes what exists or only what comes |

Scopes, from wide to narrow: engine, context, view, request. A field
declares which scopes it has; the narrowest set wins, and a popup starts
with its opener's, as the runtime settings do today. No deep merging of
arrays: an array replaces the one before.

Every surface's getter returns `{"explicit": ..., "effective": ...}`, and
`effective` says where each value came from when it differs from what was
asked (`"source": "capability"`, `"default"`, `"opener"`).

### 6.4 Capabilities

```c
/* What this build can do on this system, display server and display, as
 * JSON: {"window": {"frames": ["system", "overlay", "none"], "position":
 * true, ...}, "notification": {"service": "dbus"}, ...}. Caller frees. */
char *zebkit_get_capabilities(void);
typedef void (*zebkit_capabilities_func)(const char *json, void *data);
void zebkit_on_capabilities_changed(zebkit_capabilities_func func, void *data);
```

A capability is what the engine found, not what an OS normally has: on
Linux, X11 and Wayland differ, and a compositor without xdg-foreign or a
desktop without a notification daemon is a different answer. Some answers
are only known once a window exists, so a window's own state (7.4) has its
capabilities too, and wins.

### 6.5 Threads, lifetimes and bindings

As everywhere in zebkit: every call and callback on the engine's thread;
strings returned are the caller's to free with `zebkit_free()`; strings
given to a callback are valid during it; a callback may close its view,
and the close happens after it returns. Each C function has its Rust,
Python and host JavaScript form with the same names, values and errors,
and a test that runs the same cases through all four, as
`tests/engine/hook_policies.py` does for hooks. A binding never gives more
than C does.

### 6.6 The registry, and how "complete" is checked

The tables of section 5 move into `src/engine/surfaces.json`, one entry
per surface:

```json
{
  "id": "dialog.file-system-access",
  "chromium": ["FileSystemAccessPermissionContext", "FileSystemChooser"],
  "zebkit": ["BrowserContext::GetFileSystemAccessPermissionContext"],
  "placements": ["window", "attached", "native", "offscreen", "none"],
  "default": "the system's picker; write access refused",
  "control": ["decide", "present", "off"],
  "platforms": {"linux": "done", "macos": "done", "windows": "done"},
  "tests": ["tests/engine/file_system_access.py"],
  "status": "gap"
}
```

`scripts/check-surfaces.py` keeps the list honest, and runs with the engine
tests where Chromium's tree is (`engine-test.sh`, section `surfaces`). For
each delegate class the engine implements (`WebContentsDelegate`,
`ContentBrowserClient`, `BrowserContext`, `WebContentsViewDelegate`,
`PermissionControllerDelegate`), it reads every virtual method from
Chromium's header and fails when one is in neither list of the registry: the
methods that reach the system or the person, each with its surface and
whether zebkit implements it, and the plumbing. A Chromium update that adds
a method fails it until the method is classified. It also fails when a
method the registry says zebkit implements is not overridden in
`src/engine`, or one it leaves to Chromium is; when a surface's test is not
there; and when a surface is missing from section 5. On 157: 564 methods,
112 of them reaching the system or the person, in 67 surfaces.

Done means: no entry point to the system that is not in the registry, and
no entry whose status the tests do not hold. A release's notes list the
gaps that remain.

## 7. Windows

The first surface to build, because it is the one an application sees
first, and because it needs what every other surface needs: options before
the native object exists, capabilities, state and events.

### 7.1 What it must achieve

- A page that extends to the top of its window, with the system's window
  buttons over it where the system has them, and the page laying out
  around them: a tab strip or a toolbar in the title bar.
- A window with no system frame, the page drawing its own buttons, still
  resizing, snapping and maximizing as the system does.
- The first frame the person sees is the window as configured: no system
  title bar that disappears, no white rectangle before the page.
- Dragging the window from where the page says, and only there: a search
  field, a tab's close button or a link in the title bar stay usable.
- The application can ask for the window's state and act on it (minimize,
  maximize, fullscreen), and is told when it changes, by whomever.
- A page cannot take any of this unless the application lets it.

### 7.2 Frames

| `frame` | The window | The page |
|---|---|---|
| `system` (the default) | the system's title bar and buttons | below the title bar, as today |
| `overlay` | no title bar; the system's buttons over the page where the system draws them (macOS), else drawn by the engine as Chrome draws them for installed web apps (Windows, Linux) | from the window's top; the title bar area and the buttons' rectangles known to it |
| `none` | no title bar and no buttons; resize edges kept | from the window's top; the page draws its controls |

`overlay` is Chrome's **Window Controls Overlay**: the mode an installed
web app with `"display_override": ["window-controls-overlay"]` runs in,
and what Electron's `titleBarOverlay` gives. The content layer has it:
`WebContents::UpdateWindowControlsOverlay(rect)` sends the title bar area
not covered by buttons to the renderer, where the page reads it as the
CSS environment variables `env(titlebar-area-x)`, `-y`, `-width`,
`-height` and as `navigator.windowControlsOverlay.getTitlebarAreaRect()`
with its `geometrychange` event, and `@media (display-mode:
window-controls-overlay)` matches through
`WebContentsDelegate::GetDisplayMode` (which `Window` implements already,
for fullscreen). A page written for Chrome's installed apps or for
Electron's overlay works unchanged, and no zebkit-specific layout API is
needed for the page.

A frame is fixed at creation: changing it at runtime returns
`requires-recreate`. Recreating an `NSWindow`'s style or a Views frame
under a live page changes focus, size and attachment in ways not worth
the risk now; it can be designed later on its own.

A frame the system cannot give is an error at creation, unless the
options say `"fallback": "system"`; then the window has the system frame
and `effective.frame` says `system`, with the reason. A window is never
left without a way to close it.

### 7.3 Creating a window with options

The window must be configured before its native object exists, so options
go to creation: `NewView` calls `Window::Create` before anything else, and
`zebkit_view_set_*` after `zebkit_view_new()` is too late for a frame.

```c
/* <zebkit/window.h> */

/* zebkit_view_new_in() with options, as JSON (NULL: {} as
 * zebkit_view_new_in()). Invalid options: NULL, with @error, and nothing
 * made. */
zebkit_view *zebkit_view_new_with_options(zebkit_context *context, const char *url,
                                          const char *options_json, char **error);
```

```json
{
  "width": 1200, "height": 800,
  "minWidth": 640, "minHeight": 400, "resizable": true,
  "x": 100, "y": 80,
  "title": "Example",
  "frame": "overlay",
  "controls": {"height": 40, "color": "#20232a", "symbolColor": "#ffffff"},
  "background": "#20232a",
  "appearance": "system",
  "show": true,
  "pageFullscreen": "window",
  "dragRegions": {"source": "css", "origins": ["app://main"]},
  "fallback": "error"
}
```

| Field | Lifetime | Default | Meaning |
|---|---|---|---|
| `width`, `height` | creation, then `zebkit_view_resize` | the engine's default size | the content area, DIPs |
| `minWidth`, `minHeight`, `resizable` | runtime | none, true | as `zebkit_view_set_size_limits` |
| `x`, `y` | runtime | the system places it | screen DIPs; `unsupported` on Wayland, where the compositor places windows |
| `title` | runtime | the page's | as `zebkit_view_set_title` |
| `frame` | creation | `system` | 7.2 |
| `controls` | runtime | the system's | `height` of the title bar area, at least the system's; `color` and `symbolColor` of buttons the engine draws (Windows, Linux); ignored where the system draws them, and reported so |
| `background` | runtime | white | the window's colour before the page's first frame and where the page is transparent; avoids a white flash in a dark application |
| `appearance` | runtime | `system` | `system`, `light`, `dark`: the frame's and the buttons' look. Does not change the page's `prefers-color-scheme` (11) |
| `show` | creation | true | false: made hidden, not activated; `zebkit_view_window_action(view, "show")` later |
| `pageFullscreen` | runtime | `window` (new entry point), `content` (old) | 7.6 |
| `dragRegions` | runtime | `none` | 7.5 |
| `fallback` | creation | `error` | 7.2 |

`zebkit_view_new()` and `zebkit_view_new_in()` stay as they are: `{}` with
`pageFullscreen: "content"`, today's behaviour. Popups made by a page
(`zebkit_view_on_window_open`) have the system frame; an application that
wants another frame for a popup decides before it is made (7.8).

### 7.4 State, actions and events

```c
/* The window's state as JSON (below); NULL for a closed view. Caller frees. */
char *zebkit_view_get_window(zebkit_view *view);

/* Runtime fields of the options above, merged; creation fields are
 * requires-recreate. Validated whole, applied whole. */
bool zebkit_view_set_window(zebkit_view *view, const char *json, char **error);

/* "show", "hide", "minimize", "maximize", "restore", "fullscreen",
 * "exit-fullscreen", "close". True: asked of the system; the result
 * comes as a state event. False with @error: unsupported here, not the
 * engine's window, or a closed view. "close" runs beforeunload first, as
 * zebkit_view_request_close() does. */
bool zebkit_view_window_action(zebkit_view *view, const char *action, char **error);

/* Every change of the state below, in order, whatever caused it. */
typedef void (*zebkit_window_state_func)(zebkit_view *view, const char *json, void *data);
void zebkit_view_on_window_state(zebkit_view *view, zebkit_window_state_func func, void *data);
```

```json
{
  "revision": 12,
  "owner": "engine",
  "state": "normal",
  "visible": true, "focused": true,
  "bounds": {"x": 100, "y": 80, "width": 1200, "height": 840},
  "content": {"width": 1200, "height": 800},
  "scale": 2,
  "frame": {"requested": "overlay", "effective": "overlay"},
  "titlebarArea": {"x": 78, "y": 0, "width": 1122, "height": 40},
  "controls": [{"x": 12, "y": 13, "width": 54, "height": 14}],
  "capabilities": {"actions": ["minimize", "maximize", "fullscreen"], "position": true}
}
```

- `state` is `normal`, `minimized`, `maximized` or `fullscreen`. macOS's
  green button is fullscreen, its option-click "zoom" is `maximized`;
  they are reported as what they are.
- `titlebarArea` is what the engine gives `UpdateWindowControlsOverlay`,
  so the C side and the page agree. `controls` are the buttons'
  rectangles, which a host drawing its own UI (off screen, say) needs. All
  in the content area's DIPs, top left the origin; the page sees the same
  in CSS pixels at its zoom, converted by Blink.
- An action's `true` means asked, not done: a minimize can be refused, a
  fullscreen takes an animation. The state event says what happened.
- Events are ordered by `revision`. Resize events during a live resize are
  coalesced to one per frame; a change of `state`, `visible` or `frame`
  is never coalesced away. A closed view gets no event after its
  `on_close`.
- For a view in a window the engine does not own (section 4), `owner` is
  `application` or `none`, the state is what the engine can see, and
  actions fail with `not the engine's window`.

### 7.5 Dragging the window

Two sources, one per window, chosen in `dragRegions.source`:

- **`css`**: the page's `app-region: drag` and `no-drag` (`-webkit-app-region`
  too), the standard way, as Chrome's installed apps and Electron use. The
  engine calls `WebContents::SetSupportsDraggableRegions(true)` and takes
  `WebContentsDelegate::DraggableRegionsChanged`. What the 157 source
  shows: the property is stable (`CSSWindowDrag`); Blink collects regions
  only when the page is told it supports them; it collects them from the
  **main frame's document only** (`WebViewImpl::DraggableRegionsChanged`
  reads `MainFrameImpl()->GetDocument()`), so no iframe, same-site or not,
  can contribute a region; and it sends them in DIPs
  (`BlinkSpaceToEnclosedDIPs`). Content applies the setting again when the
  primary page changes.
- **`host`**: rectangles the application gives, for UI drawn without CSS:

  ```c
  /* [{"x":0,"y":0,"width":800,"height":40,"drag":true}, ...] in content
   * DIPs, later entries over earlier ones; at most 1024. [] clears. */
  bool zebkit_view_set_drag_regions(zebkit_view *view, const char *json, char **error);
  ```

**Who may set regions.** `css` regions are honoured only while the main
frame's document is of an origin in `dragRegions.origins` (the
application's own pages, as for a grant). When the main frame commits a
document of another origin, its regions are dropped at commit, before it
can paint, and it gets none; a renderer crash drops them too. Regions are
never a capability a page can ask for.

**Hit testing**, in this order: the system's resize edges and the window
buttons; then the regions, `no-drag` over `drag` and later over earlier;
then the page. A drag starts only from a real mouse press the system
delivered, never from a call: there is no `startDrag(x, y)` for a page,
and none for the application, because Wayland moves a window only from a
press's serial. A double click in a drag region does what the title bar
does on that system (maximize, or on macOS what the person's setting says).

**Cost.** Regions are a browser-side snapshot updated when Blink sends a
change (after layout), not per mouse move; hit testing reads the snapshot.
No IPC and no script per mouse move.

### 7.6 Fullscreen

Two things are called fullscreen, and are kept apart:

- **The page's** (`element.requestFullscreen()`): today it fills the
  content area only. Chrome makes the browser window fullscreen for it.
  `pageFullscreen: "window"` does what Chrome does in an engine window:
  the window goes fullscreen with the page and comes back when the page
  leaves; `"content"` keeps today's. The fullscreen policy hook still
  decides whether the page may.
- **The window's** (`zebkit_view_window_action(view, "fullscreen")`, the
  green button, F11 by the application's menu): the window, with the page
  in its normal state.

The state reports both (`state: "fullscreen"` and the page's flag), and
`GetDisplayMode` answers `fullscreen` for either, as Chrome does.

### 7.7 Per system

**macOS** (`window_mac.mm`):

- Create the window hidden; show it after the page is attached and the
  first frame is ready (or after a short limit, so a slow page does not
  keep a window from showing), unless `show` is false. This removes the
  first frame without its page that `Create()` shows today, for every
  frame, not only the new ones.
- `overlay`: keep `Titled | Closable | Miniaturizable | Resizable`, add
  `FullSizeContentView`, set `titlebarAppearsTransparent` and
  `titleVisibility = hidden`. The system keeps its buttons, shadow, rounded
  corners, fullscreen and tiling. `controls` and `titlebarArea` come from
  the buttons' real frames (`standardWindowButton:`), converted to the
  content view's coordinates, recomputed on resize, screen change, and
  entering and leaving fullscreen (where the buttons hide). `controls.height`
  taller than the system's is done with an `NSTitlebarAccessoryViewController`
  of that height, which moves the buttons as the system lays them out; no
  button is placed by coordinates.
- `none`: `Borderless` is not used: it loses key-window behaviour, the
  menu's window commands and the resize edges. `none` is `overlay` with
  the three buttons hidden.
- Dragging: the content view, on a mouse down in a drag region, calls
  `-[NSWindow performWindowDragWithEvent:]` with that event, so the system
  moves the window, with its snapping and Stage Manager. Not
  `movableByWindowBackground`, which would take the whole page.
- Fullscreen: `toggleFullScreen:`; state from the window's
  `windowDidEnterFullScreen:` and `windowDidExitFullScreen:`, not assumed.

**Windows and Linux** (`window_views.cc`, one implementation on Views):

- Views shows the window in `SetContents()` already; the `show` option and
  the background colour are added there.
- `overlay` and `none`: a `views::FrameView` of zebkit's, installed with
  `WidgetDelegate::SetFrameViewFactory` (the 157 names; there is no
  `CreateNonClientFrameView` any more). Its `NonClientHitTest` returns the
  resize edges (`HTLEFT` ... `HTBOTTOMRIGHT`), the buttons (`HTMINBUTTON`,
  `HTMAXBUTTON`, `HTCLOSE`), `HTCAPTION` in drag regions, and `HTCLIENT`
  elsewhere. Returning these, not moving the window from mouse events, is
  what keeps the system's behaviour: on Windows, Aero Snap, the system
  menu (Alt+Space, a right click on a drag region), and Windows 11's snap
  layouts on hover over the maximize button, which need `HTMAXBUTTON`; on
  Linux, Ozone turns `HTCAPTION` into the window manager's move, which on
  Wayland is `xdg_toplevel.move` with the press's serial.
- The buttons for `overlay` are drawn by the engine with Views, as Chrome
  draws its own caption buttons for Window Controls Overlay, in
  `controls.color` and `symbolColor`, following the system's sizes and
  its light or dark look. They are the same code on Windows and Linux.
- Windows: client-area extension over the frame with DWM so the system's
  shadow and rounded corners stay; DPI changes (`WM_DPICHANGED` through
  Views) recompute everything. Linux: client-side decoration on Wayland,
  where the frame is the application's in any case; on X11 the window
  manager's frame removed with the Motif hints, as Chrome does.

**No window**: every action `unsupported`; `zebkit_view_get_window()`
reports `owner: "none"`. Options are validated all the same, so a test of
an application's options runs headless.

### 7.8 Popups

A page's popup (`window.open`, `target=_blank`) gets the system frame and
no drag regions: it does not inherit its opener's. An application that
wants a frame for its own popups (a sign-in window in the application's
look) decides before the window is made, with a setting of the opener:

```c
/* Options for windows pages of @view open from now on: the runtime and
 * creation fields of 7.3 but dragRegions, which a popup never has from
 * its opener. NULL: the system frame. */
bool zebkit_view_set_popup_window_options(zebkit_view *view, const char *json, char **error);
```

A setting, not a callback, because Chromium makes the popup's contents
before asking the delegate where to put them, and a popup must keep its
opener, POST data and `window.closed` (the reason
`zebkit_view_on_window_open` gives the made view, not its URL).

### 7.9 Files

| Area | Files |
|---|---|
| C contract | new `src/engine/include/zebkit/window.h`; `zebkit.ver`, `mac/exports.txt` and the Windows export list |
| Options and state | new `window_options.cc/.h` (parsing, validation, typed `WindowOptions`), `view.cc` (`NewView` passes them to `Window::Create`), `window.cc/.h` (state, revision, regions snapshot, `DraggableRegionsChanged`, `GetDisplayMode`) |
| Platform interface | `window_platform.h`: `Create(window, options)`, `Show`, `Hide`, `GetState`, `PerformAction`, `SetAppearance`, `SetDragRegions`, a capabilities query; typed values, no JSON |
| macOS | `window_mac.mm`; an `NSView` subclass for the drag hit test if the content view cannot take it |
| Views | `window_views.cc`, new `window_frame_views.cc` (the `FrameView` and its buttons) |
| Bindings | `rust/zebkit-rs` (raw), `rust/zebkit` (`ViewBuilder`, `WindowOptions`, `WindowState`, `WindowAction`), `bindings/python/zebkit`, `script.cc` (`new zebkit.View({window: ...})`) |
| Tests | `tests/engine/window_options.c` (validation, headless), `tests/engine/window_frame.py` (each frame, state, actions, events, regions; real windows), the same cases in Rust and host JavaScript |
| Example | `examples/custom-titlebar/`: a page with tabs in an `overlay` title bar, CSS regions, in C and JavaScript |
| Docs | `docs/API.md` (the reference), `docs/MACOS.md`, `docs/WINDOWS.md` |

## 8. Menus, commands and keys

**Today**: `zebkit_app_set_menu()` takes the whole menu as JSON, with
roles for the standard commands; on macOS it is the menu bar, elsewhere
only its keys work. `zebkit_view_on_context_menu` takes a page's context
menu with its context, and without it nothing is shown.

**Design:**

- **One command model.** An item is `{id, title, role, key, modifiers,
  enabled, checked, radioGroup, items}`. The same `id` may appear in the
  application menu, the Dock menu, a context menu and a tray menu; all
  call the same function with the `id`. Roles (`undo`, `copy`, `paste`,
  `selectAll`, `minimize`, `fullscreen`, `quit`, ...) are executed by the
  engine on the focused frame of the focused view, not always the main
  frame.
- **State changes without rebuilding.** `zebkit_app_update_menu_item(id,
  json)` sets `enabled`, `checked` and `title` of every item with that id.
  A function to fill a menu just before it opens (macOS
  `menuNeedsUpdate:`) is not offered: it would run the application's code
  while the system waits on the UI thread. The application updates state
  as it changes.
- **Menu bars on Windows and Linux** are not the engine's. An application
  that wants one draws it, in its toolkit or in its page (the overlay
  title bar is where most do now); the keys keep working through the
  engine as today. This is a decision, not a gap: on macOS the menu bar is
  the system's and belongs to the application object, elsewhere it is a
  part of the window's content.
- **Context menus.** Without a function, the engine shows a default menu
  with the system's look where there is a window (Back, Forward, Reload;
  Cut, Copy, Paste, Select All in editable fields; Copy Link, Open Link,
  Copy Image, Save Image As), drawn with Chromium's views menu
  (`views::MenuRunner`) on Windows and Linux and `NSMenu` on macOS, the
  same items on each. With a function, it gets the context and the
  default items, and answers with nothing (no menu), the default items, or
  its own list mixing default ids and its own. The engine runs a default
  item itself, against the context it captured: a page cannot make the
  engine "Save Image As" a URL it chose later.
- **Keys**, in this order: the system's own (which no application gets),
  the view's reserved keys (`zebkit_view_set_reserved_keys`), the page and
  the input method, then the menu's keys when the page did not take them
  (today's rule). Global shortcuts (keys that work while the application
  is in the background) are for the host's toolkit and are not planned in
  the engine.

## 9. Dialogs and choosers

Each chooser is a hook, with the system's dialog or the engine's as the
default presenter, and none without a window.

- **File System Access bypasses the file chooser hook today.** Its picker
  must go through the same decision as `<input type=file>`:
  `zebkit_view_on_file_chooser` gets a request with `"kind": "open" |
  "save" | "directory"`, `"api": "file-system-access"`, the suggested name
  and the types; and `Profile` provides a
  `FileSystemAccessPermissionContext` whose read and write grants follow
  the permission hook (`file-system-read`, `file-system-write`), denied by
  default as other permissions are. Until then the picker reaches the
  system with no decision of the application's, which section 16 does not
  allow: this is the first fix of this document.
- **JavaScript dialogs on Linux** have no default presenter; Windows'
  `prompt()` returns null. With a window, the default becomes a
  `zebkit.ui` dialog, the same on each system where the system has none
  (Linux, Windows' prompt). macOS keeps `NSAlert`.
- **EyeDropper** gets a default: Chromium's views eye dropper, which
  content can host (`OpenEyeDropper`); none off screen, where the promise
  rejects as now.
- **Colour, date and `<select>`** stay Blink's in-page popups: they work
  the same everywhere, need no window of the system's, and are drawn by
  the page's renderer. Not hooks.
- **HTTP authentication, client certificates**: default presenters in
  `zebkit.ui` where there is a window (a sign-in form, a list of
  certificates), still cancelled or none without a function and without a
  window.
- **Print**: `window.print()` with a window shows the system's print
  dialog and prints (Chromium's `PrintViewManagerBase` path with the
  platform's printing backend), when the build includes printing; the
  application may take it with a hook and keeps `zebkit_view_print_to_pdf`
  for output without a dialog. A build without the printing backend
  reports `print` unsupported. Print preview is not planned (it is
  Chrome's WebUI).

## 10. Permissions, devices and capture

The permission hook decides; what is added is the parts that today have
no implementation behind the decision.

- **Screen capture** (`getDisplayMedia()`): a request to the media access
  hook with `"type": "display"`, answered with a chosen source (a screen,
  a window, the view itself). Default presenter: the system's picker
  where it has one (the XDG portal's ScreenCast on Linux, with the
  window's exported handle; `SCContentSharingPicker` on macOS 14 and
  later), else none and refused. The system's own consent stays the
  system's.
- **USB, HID, Serial, Bluetooth**: a `UsbDelegate`, `HidDelegate`,
  `SerialDelegate` and `BluetoothDelegate` whose choosers are one hook,
  `zebkit_view_on_device_chooser`, with the device list updated as devices
  come and go, answered with one device or none. No default presenter
  first (refused, as now); a `zebkit.ui` list later. Permission to use a
  chosen device again goes through the permission table.
- **Pointer lock and keyboard lock** become permissions with a policy per
  view, pointer lock allowed by default as now, keyboard lock denied.
- **`registerProtocolHandler()`** stops reporting a registration it did
  not make: the request goes to the application (a permission
  `protocol-handler`, denied by default), and registering the scheme with
  the system is the application's (section 15).
- **Geolocation** on Linux and Windows: the system's providers
  (GeoClue, the Windows location API) through Chromium's
  `GeolocationSystemPermissionManager`, behind the permission hook, as
  macOS has today.

## 11. Appearance, locale, accessibility

**Appearance.** Two things, kept apart: the window's look (its frame and
buttons, `appearance` in 7.3) and the page's `prefers-color-scheme`. Today
the page is light unless told otherwise, and the system's theme is not
read. Design:

- The engine follows the system's theme by default for both, through
  `ui::NativeTheme` (which reads the system on macOS and Windows, and the
  portal's `org.freedesktop.appearance.color-scheme` on Linux), and its
  changes while running. This is a change of default for the page, so it
  is a startup setting, `"appearance": {"follow": true}`, false by
  default for existing applications and true in the new examples.
- `zebkit_view_set_settings` `colorScheme` and `contrast` keep winning
  for their view.
- `zebkit_get_environment()` returns what the system says (`colorScheme`,
  `contrast`, `reducedMotion`, `accentColor`, `locale`, `scale` per
  screen) and `zebkit_on_environment_changed` tells changes. An
  application never changes the system's settings through zebkit.

**Locale.** From Chromium 157 the page's default locale is the system's
(`patches/1083`), en-US without one; `Accept-Language` follows it. An
application overrides it with the startup setting `locale`, as Chrome's
`--lang`.

**Accessibility** is a requirement for every window the engine shows, not
an option, and is today untested:

- Turn on the platform tree when an assistive technology asks, as Chrome
  does: on macOS by giving `ZebkitApplication` Chrome's
  `accessibilitySetValue:forAttribute:` handling of
  `AXEnhancedUserInterface` and `AXManualAccessibility`; on Windows and
  Linux content's detection already does it, to be verified.
- An attached or off-screen view's accessibility belongs in the host's
  tree; for off-screen views that needs a bridge the host builds on
  `Accessibility.getFullAXTree`, documented, not built here.
- Tests that read the real tree: AT-SPI on Linux (in the Xvfb runs),
  `NSAccessibility` from the Mac test host, UI Automation on Windows; each
  finds a page's button by its name and presses it.
- Whatever replaces a system UI (an engine-drawn button in an overlay, a
  `zebkit.ui` dialog) has an accessible name and works from the keyboard.

## 12. Notifications and the desktop

- **Web notifications**: a `PlatformNotificationService` in `Profile`, so
  that `Notification.permission` follows the permission hook (denied by
  default, as now) instead of "denied" always. Showing one is a hook,
  `zebkit_on_notification`, with title, body, icon, actions, tag and the
  origin; the application answers with `shown`, or `default` for the
  system's notification: `UNUserNotificationCenter` on macOS (needs the
  application's bundle identifier and the person's consent, reported as a
  capability), the toast API on Windows (needs an AppUserModelID),
  `org.freedesktop.Notifications` over D-Bus on Linux. Clicks and closes
  come back to the page's `onclick` and `notificationclick` handlers.
  Notifications of a service worker with no page open need the engine
  running; they are shown only while it runs (no background launch).
- **Push** needs a push service, which is Google's for Chrome; not
  planned. A hook could carry an application's own later.
- **Tray and status item**: `zebkit_tray_new(icon, tooltip)`, a menu of
  the command model (8), click events. `NSStatusItem` on macOS,
  `Shell_NotifyIcon` on Windows, StatusNotifierItem over D-Bus on Linux,
  where a desktop without a host for it is a capability `false`. Small and
  common enough for the engine; nothing appears unless asked.
- **Dock and taskbar**: badge text, progress, the Dock menu, attention
  requests. `NSDockTile` on macOS, `ITaskbarList3` on Windows, the Unity
  launcher API where it exists on Linux, else capability `false`.
- **Web Share**: `navigator.share()` exposed only with a hook set
  (`zebkit_view_on_share`); the default presenter is the system's share
  sheet on macOS (`NSSharingServicePicker`) and Windows
  (`DataTransferManager`), none on Linux.

## 13. Files, other programs, printing

- **Downloads** stay as they are (`docs/ROADMAP_HOOKS.md`): saved to the
  profile's `Downloads` without a dialog and never opened. A
  `"downloads": "ask"` runtime value shows the system's save dialog by
  default.
- **Other programs.** `mailto:` and other external schemes stay told and
  not opened. For the application, `zebkit_open_external(url)` and
  `zebkit_show_in_folder(path)` call the system's handler (`NSWorkspace`,
  `ShellExecuteW`, the portal's OpenURI and `org.freedesktop.FileManager1`
  on Linux) with the URL or path as one argument, never through a shell.
  Pages never reach them except through the application.
- **Printing**: section 9.

## 14. Power, session, displays

- `zebkit_on_power_event`: suspend, resume, screen locked and unlocked,
  on battery, from Chromium's `base::PowerMonitor`, which the engine runs
  already.
- `zebkit_get_displays()` and a change event: bounds, work area, scale,
  primary, from `display::Screen`.
- Keeping the system awake for the application (not for a page, which has
  the wake lock API) is `zebkit_prevent_sleep(reason)` returning a token,
  released by the token or by the engine's end. Session end (logout)
  arrives as a quit request where the system gives one, as on macOS
  today.

## 15. What the engine does not do

These change the system for longer than the process, and are the
installer's or the application's, not a library's:

- registering URL schemes and file types with the system, login items,
  default-browser status, Start menu and desktop entries, the Dock's
  permanent icon;
- writing the system's settings (theme, proxy, keychain entries the
  application does not own).

The engine receives what they deliver at runtime: `app.open-url` (5.1)
and opened files. `docs/API.md` shows how each system's packaging
declares them (`Info.plist`, a `.desktop` file, the Windows registry).

## 16. Security and privacy rules

- A page gets nothing of this document without a grant to its origin;
  grants are per view and never inherited by a popup.
- No surface reaches the system without a decision the application can
  take: the File System Access picker (9) and protocol handlers (10) are
  today's exceptions, and are fixed first.
- The engine never asks the system for a sensitive capability (camera,
  location, notifications, screen) without a request from a page or the
  application that the decision allowed.
- Commands run from menus run against the context the engine captured.
- Window regions and actions are dropped at a cross-origin navigation and
  a renderer crash.
- Events that carry personal data (a notification's body, a credential, a
  pairing code) are never logged by the engine, as for WebAuthn now.
- CDP stays off unless asked, on 127.0.0.1 by default.

## 17. Order of work

Each phase ships when its exit test passes on Linux, macOS and Windows,
with a release note; unsupported cases are listed, never counted as
passed.

| Phase | Content | Exit |
|---|---|---|
| 1. Registry | `surfaces.json` from section 5; `scripts/check-surfaces.py`; the generated tables | the check passes on 157 and names every unlisted method; section 5 generated |
| 2. Holes | File System Access through the file chooser hook and the permission table; protocol handlers asked, not faked; pointer and keyboard lock as permissions | tests show each denied by default, decided by a hook, and through all four languages |
| 3. Windows | section 7: options at creation, the macOS first frame, `system`, `overlay`, `none`, Window Controls Overlay, CSS and host regions, state, actions, events, popups' options, page fullscreen | the custom title bar example, in real windows on macOS, Windows, X11 and Wayland: first frame as configured; buttons' rectangles within 1 DIP of the system's; drag only in regions; snap and system menu on Windows; resize everywhere; cross-origin navigation drops regions |
| 4. Appearance and accessibility | following the system's theme and its changes; the environment query; the platform tree on demand; real-tree tests | a page follows the system's dark mode live; a screen reader's API finds and presses a button on each system |
| 5. Menus and dialogs | menu item state by id; the default context menu and running its commands; `zebkit.ui` defaults for JavaScript dialogs, sign-in and certificates on Linux and Windows; the eye dropper | each item of 8 and 9 in tests, with the default, a hook, and no window |
| 6. Notifications and desktop | web notifications; tray; Dock and taskbar; Web Share; `open_external`, `show_in_folder` | a notification shown by the system and clicked back to the page on each system; capability `false` where the desktop lacks a service |
| 7. Capture, devices, power | the screen-capture picker; device choosers; displays; power events; print dialog | `getDisplayMedia()` through the system's picker; a USB device chosen in a test with a virtual device where the system has one |

Phase 1 and 2 come before anything new, because they make the rest
measurable and close what reaches the system today without the
application. Phase 3 is the first user-visible one and the template for
the API shape of the others. The size of each phase is measured as for
any change (`docs/SIZE_AND_SPEED.md`); phase 6's services are D-Bus and
system frameworks, already linked, not new libraries.

## 18. Testing

- **Each surface**: the default; each answer; an answer later; the
  deadline; the page or view going away while it waits; the handler
  replaced; no window; through C, Rust, Python and host JavaScript with
  the same expectations.
- **Real windows, not screenshots of pages.** CDP's screenshot is the
  page; a frame's correctness is in the system's window. On macOS the
  test host reads the buttons' frames and the window's state with AppKit
  and sends real events; on Windows `GetWindowRect`, `WM_NCHITTEST` sent
  to the window, and `SendInput`; on X11 under a window manager in Xvfb
  with XTEST; on Wayland under labwc, as `native-window-wayland` runs now.
  A captured image of the whole window (with its frame) shows the first
  frame.
- **No silent skip.** Each test reports passed, failed, unsupported (with
  the capability that said so) or not run; the release notes count each.
- **Security cases**: a page without a grant; an iframe's `app-region`;
  a navigation to another origin while regions are set; a popup; a
  renderer crash.
- **Cost**: a page animating a region and a live resize, measured for
  frame time against the same page with `frame: "system"`; like the
  network hook, a surface costs nothing until it is set, as
  `tests/bench/web/` checks now.

## 19. Decisions, and what is open

Decided by this document:

| Decision | Reason |
|---|---|
| Today's behaviour stays the default of today's entry points | no application changes without asking |
| No presets ("desktop", "embedded", "headless" as bundles of defaults) | they hide what a default is and multiply what must be tested; ownership (section 4) and capabilities explain the same differences directly |
| `overlay` is Window Controls Overlay | content has it; pages written for Chrome and Electron work; no layout API of zebkit's for pages |
| Frames are fixed at creation | changing a live window's frame risks focus, size and attachment; later, separately |
| Drag regions from the main frame only, for granted origins | Blink collects them from the main frame's document only; a grant keeps other origins out |
| No menu bar on Windows and Linux from the engine | it is window content there, the application's to draw |
| No global shortcuts, no install-level registration in the engine | the host's toolkit and the installer do them better, and they outlive the process |
| The system's consent is never replaced | the system's to give |

Open, to decide when the phase starts:

- Whether `controls.height` taller than the system's is possible on
  macOS without moving buttons by coordinates (7.7); if not, the height is
  the system's on macOS and `effective` says so.
- Whether Windows' snap layouts appear with an engine-drawn maximize
  button returning `HTMAXBUTTON`, as Chrome's do; to be confirmed on
  Windows 11.
- The exact first-frame wait on macOS (7.7): the page's first frame or a
  limit, and the limit's value.
- Whether printing's backend fits the size rule (9), which decides whether
  the release build has the print dialog or reports it unsupported.
- The accessibility bridge for off-screen views (11): what the host
  needs, and whether CDP's tree is enough.
