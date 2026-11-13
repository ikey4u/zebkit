# System integration

Every place where the engine meets the operating system or shows something
to a person: a window, a menu, a dialog, a permission, a device, a
notification, the system's theme. For each: what decides there, what
happens when the application says nothing, how the application takes it
over, and what is missing. This document describes how zebkit does it and
why, and is the inventory against which "complete" is measured.

Status: implemented in 0.4.0 (Chromium 157), all seven phases of section
17 (`docs/releases/0.4.0.md`). The document was first written as the
design, on 2026-11-08; it now describes what exists, with the reasons for
each choice and what was declined. What is not done is in section 5 (each
surface's status) and section 19. Statements about the engine's behaviour
name the file and symbol that show it; line numbers drift, symbols do not.

Related: `docs/HOOKS.md` (how a decision is handed to the
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
  has a hook and, where a default is worth its size, a small one of its
  own, drawn with Chromium's views (as `zebkit.ui` is) or AppKit, or the
  system's own dialog.
- **Not a way around the system.** Where macOS asks the person for the
  camera, or Wayland forbids a window placing itself, the application can
  choose when to ask and what to say first, not the answer. The system's
  refusal is reported, never hidden.
- **Not a framework for building applications on.** zebkit is a library
  applications integrate (`docs/UI.md`, "What it is"). The host keeps its
  loop, windows and toolkit; the engine never takes them from it.

## 2. Principles

These are fixed; the rest of the document follows from them.

1. **The earlier behaviour is the default.** An application built against
   the API before 0.4.0 sees no change: no hidden migration of menus,
   permissions, profiles or windows. A new entry point keeps it too:
   `zebkit_view_new_with_options()` has `pageFullscreen: "content"` unless
   asked for `"window"` (section 7.6). The one default that changed is
   `Notification.permission`, `"default"` where it said `"denied"`
   (section 12); requests are still denied without a decision.
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
   application's trusted code (C, Rust, Python, Go, host JavaScript). A
   page gets a piece of it only through a grant the application gives an
   origin, as `zebkit_view_grant()` gives `zebkit.*`.
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
| Surface | One place where the engine meets the system or the person: "the file chooser", "a window's title bar", "web notifications". Each has a stable identifier, such as `dialog.file-chooser`, in the registry (6.6). |
| Default | What happens with nothing configured. Documented per surface, held by a test where the registry names one. |
| Presenter | What shows a surface to the person: `system` (the system's own UI), `engine` (Chromium's views or AppKit, drawn by the engine), `application` (the host, through a hook), or `none`. |
| Capability | Whether this build, on this system and display server, can do something at all: an `overlay` frame, placing a window (not on Wayland). Asked, never guessed from the OS name (`zebkit_get_capabilities()`, 6.4). |
| Requested / effective | What the application asked for and what holds. The runtime settings report both (`zebkit_get_engine_settings()`: `explicit` and `effective`), and so does a window's state (`frame.requested` and `frame.effective`, `options.explicit`). |
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
| Engine window | `zebkit_view_new[_in]()`, `zebkit_view_new_with_options()` | the engine's, configurable (section 7) | the engine's, asked by the application or the person |
| Attached to a view | `zebkit_view_attach()` | the parent view's window | the parent's; refused for the child |
| In the host's window | `zebkit_view_attach_native()` | the host's | the host's; the engine never touches the parent |
| Off screen | `<zebkit/offscreen.h>` | the host's, which draws the frames | the host's |
| No window | `--headless`, or no display | none | reported unsupported |

Rules that follow:

- A surface that needs a window (a native dialog's parent, a context menu)
  uses the view's top-level window: `zebkit_view_get_native_window()` and,
  for portals, `zebkit_view_export_native_window()`. With none, the
  default is the one written for "no window", never a dialog parented
  nowhere.
- The engine never changes a window it does not own: no style, no
  fullscreen, no focus stealing in the host's window.
- A view's window keeps its options while it is attached in the host's
  window; `zebkit_view_detach_native()` gives the window back the style it
  had before (`attach.cc`).

## 5. The surfaces

Status: **done** (a default and a way for the application to decide or
present), **partial** (some of it), **default only** (works, nothing to
configure), **gap** (missing or refused for lack of an implementation).
"Hook" names are of `<zebkit/hooks.h>` and `<zebkit/page.h>`. The status of
each row is the one in the registry (`src/engine/surfaces.json`, 6.6), which
holds one status per surface for every system; where a system differs, the
row says so.

### 5.1 Application

| Surface | Behaviour | Status |
|---|---|---|
| `app.lifecycle`: ready, quit request, reopen, files opened | `zebkit_main()` ready callback, `zebkit_app_on_quit_request`, `zebkit_app_on_reopen` (macOS), `zebkit_app_on_open_files` (macOS from Finder and Dock; elsewhere program arguments) | done on macOS; partial elsewhere (no single-instance hand-over of files: by design the host's) |
| `app.open-url`: a URL the system gives the application (`myapp://`) | not received | gap |
| `app.menu`: the application's menu | `zebkit_app_set_menu()` with roles, `enabled`, `checked`, `radioGroup`; `zebkit_app_update_menu_item()` by id, `zebkit_app_get_menu()`; the macOS menu bar; elsewhere the items' keys (a menu bar there is the application's, section 8) | done |
| `app.dock`: Dock menu, badge, progress; taskbar on Windows | `zebkit_app_set_badge()`, `zebkit_app_set_progress()`, `zebkit_app_request_attention()`: `NSDockTile` and the Dock's bounce on macOS, `ITaskbarList3` (progress, an overlay badge) and `FlashWindowEx` on Windows, the launcher API (`com.canonical.Unity.LauncherEntry`) on Linux docks that know it; `zebkit_app_set_dock_menu()` on macOS (`dock.cc`) | done |
| `app.identity`: name, icon, app id (`wm_class`, AppUserModelID) | `wm_class` from the program name (`window_views.cc`); bundle metadata on macOS | partial |

### 5.2 Windows

| Surface | Behaviour | Status |
|---|---|---|
| `window.frame` | `system`, `overlay`, `none` by `zebkit_view_new_with_options()` (`<zebkit/window.h>`): macOS a full-size content view under a transparent title bar, the system's buttons; Views a frame of zebkit's with engine-drawn buttons (`window_mac.mm`, `window_views.cc`) | done |
| `window.show` | shown once its page is in it, on macOS too; `show: false` makes it hidden until the `show` action | done |
| `window.geometry` | size, place (`x`, `y`; refused on Wayland), limits by the options; `zebkit_view_get_window()` state, actions, `zebkit_view_on_window_state()` events | done |
| `window.title` | `zebkit_view_set_title`, the page's by default | done |
| `window.focus` | `zebkit_view_focus`; the page's own focus request (`ActivateContents`) | done |
| `window.fullscreen` | a page's fullscreen fills the content area, or with `pageFullscreen: "window"` makes the window fullscreen as Chrome does; the `fullscreen` actions; the fullscreen policy still decides (`zebkit_view_set_fullscreen_policy`) | done |
| `window.drag-regions`, `window.controls-overlay` | the page's `app-region` while its main frame is of a granted origin, or `zebkit_view_set_drag_regions()`; an overlay's title bar area sent with `UpdateWindowControlsOverlay` (`navigator.windowControlsOverlay`, `env(titlebar-area-*)`, `display-mode: window-controls-overlay`) | done |
| `window.close` | the close box, `window.close()`, `zebkit_view_close`, `zebkit_view_request_close` (beforeunload first) | done |
| `window.popup`: `window.open`, `target=_blank` | blocked without a gesture, as Chrome; `zebkit_view_on_new_window`, `zebkit_view_on_window_open`, the popup policy | done |
| `window.web-controls`: the page's `window.minimize()`, `maximize()`, `restore()`, `setResizable()` (Additional Windowing Controls) and `moveTo`/`resizeTo` | Chromium's defaults: refused or ignored (`MinimizeFromWebAPI` and the others not overridden) | gap |
| `window.picture-in-picture`: video and document picture-in-picture | `EnterPictureInPicture` returns not supported | gap |

### 5.3 Input

| Surface | Behaviour | Status |
|---|---|---|
| `input.reserved-keys` | `zebkit_view_set_reserved_keys`, `zebkit_view_on_reserved_key` | done |
| `input.context-menu` | without a function the engine's default menu, `NSMenu` on macOS and Chromium's views menu elsewhere; `zebkit_view_on_context_menu` gets the context with `menuId` and the default items; `zebkit_view_context_menu_run()` runs a default command against the captured context, `zebkit_view_context_menu_show()` shows a menu mixing default and own items (`context_menu.cc`) | done |
| `input.drop` | `zebkit_view_on_drop` (paths before the page has the drop), the drag API of `<zebkit/page.h>` | done |
| `input.pointer-lock` | the `pointer-lock` permission: granted by default where there is a window, as before; refused without one (`--headless`, off screen), where there is no system pointer | done |
| `input.keyboard-lock` | the `keyboard-lock` permission, denied by default (`Window::RequestKeyboardLock`) | done |
| `input.global-shortcuts` | none | gap; host's toolkit by preference |

The input method is not a surface of its own: it is the system's, and the
engine passes it on. On macOS, Windows and Wayland Chromium has it from the
system. On X11, where Ozone gives none and zebkit has no toolkit, the engine
is a client of IBus itself (`ime_ibus_linux.cc`: ibus-daemon, or fcitx5's
IBus frontend), composition drawn in the page and password fields left out;
`tests/engine/input_method.py` checks it against a fake IBus.

### 5.4 Dialogs and choosers

| Surface | Behaviour | Status |
|---|---|---|
| `dialog.javascript`: alert, confirm, prompt, beforeunload | `zebkit_view_on_dialog`, a policy per view; default presenter: `NSAlert` on macOS, `MessageBoxW` on Windows, Chromium's views dialog on Linux and for Windows' `prompt()` (`js_dialog_views.cc`); suppressed without a display | done |
| `dialog.file-chooser`: `<input type=file>` | `zebkit_view_on_file_chooser`; default `ui::SelectFileDialog` (NSOpenPanel, the Windows dialog, the XDG portal on Linux) | done |
| `dialog.file-system-access`: `showOpenFilePicker()`, `showSaveFilePicker()`, `showDirectoryPicker()` | through `zebkit_view_on_file_chooser` (`"api": "file-system-access"`, modes open, save, directory; `patches/1084`); the page reads what was picked and writes where it saves; writing to an opened file, or a handle beyond what was picked, is the `file-system-write` or `file-system-read` permission, denied without a hook (`file_system_access.cc`) | done |
| `dialog.color`: `<input type=color>` | Blink's in-page popup (`ColorChooserPopupUIController`) | default only |
| `dialog.eye-dropper`: `EyeDropper.open()` | `zebkit_view_on_eye_dropper`: the application answers with a colour; without a function cancelled (Chrome's eye dropper is its browser's, capturing the screen) | done |
| `dialog.date-time` | Blink's in-page chooser | default only |
| `dialog.select`: `<select>` popups | macOS windows: a native `NSMenu`; macOS off-screen views and Views: Blink's in-page popup (`offscreen.cc` forbids external popups) | default only |
| `dialog.http-auth` | `zebkit_view_on_http_auth`; without one, the engine's sign-in dialog where there is a window, as Chrome asks (`sign_in_dialog_views.cc` on Linux and Windows, `sign_in_dialog_mac.mm`); cancelled without a window (`tests/engine/sign_in.py`) | done |
| `dialog.client-certificate` (in the registry under `network.client-certificates`) | `zebkit_view_on_client_certificate`; without one, when the engine setting `clientCertificateStores` is on and the system's stores offer identities, the engine's list where there is a window (`certificate_picker_views.cc`, `certificate_picker_mac.mm`); otherwise none sent | done |
| `dialog.webauthn` | `zebkit_view_on_authentication`, or the engine's UI (Views, AppKit); system passkeys on macOS and Windows (`webauthn.cc`) | done |
| `dialog.print`: `window.print()` | no dialog and no printer: reported to the application as a dialog, the page continues; `zebkit_view_print_to_pdf` for output (`print.cc`). Decided: no printing backend (its size), section 19 | partial: no physical printing, no preview |
| `dialog.repost`: "send the form again?" on reloading a POST | `ShowRepostFormWarningDialog` not overridden: Chromium's default, the form is not sent again | default only |
| `dialog.fedcm`: FedCM sign-in (`navigator.credentials.get({identity})`) | no dialog controller and no permission contexts: not available | gap |

### 5.5 Permissions, devices and capture

| Surface | Behaviour | Status |
|---|---|---|
| `permission.*` (geolocation, clipboard, sensors, MIDI, ...) | `zebkit_view_on_permission`, a table per view; default denied, but a screen wake lock, pointer lock and writing to the clipboard after a click, as Chrome grants them (`permission_manager.cc`) | done |
| `permission.geolocation.system` | Apple's location source on macOS; none elsewhere (`client_defaults.cc`) | partial |
| `media.camera-microphone` | `zebkit_view_on_media_access`, else the permission table | done |
| `media.display-capture`: `getDisplayMedia()` | refused without a function; `zebkit_view_on_media_access` gets `"type": "display"` and the sources, and answers with one: `view` (the view itself, with its sound when asked) or `screen` (the whole desktop, where WebRTC's capturer reaches one: X11, macOS, Windows) (`page_hooks.cc`) | partial: no system picker as the default; Wayland's PipeWire capture is off in this build |
| `device.usb`, `device.hid`, `device.serial` | `UsbDelegate`, `HidDelegate`, `SerialDelegate` whose chooser is `zebkit_view_on_device_chooser`: the system's matching devices, the list following them as they come and go, answered with one or none; a chosen device is the origin's for the engine's run, until `forget()`; HID devices of only protected reports (keyboards, mice) are not offered, as in Chrome; no device without a function (`device_choosers.cc`; `patches/1085` for serial ports without a platform Bluetooth) | done |
| `device.bluetooth` | no delegate: Web Bluetooth reports not supported | gap: its scanning and pairing prompts are a chooser of their own |
| `device.smart-card` | no delegate: not available | gap |
| `media.audio-output`: `selectAudioOutput()` | Chromium's default | default only |
| `media.system-controls`: the system's now-playing and media keys | Chromium's default | default only |
| `media.speech`: speech recognition and synthesis | zebkit's recognition delegate; Chromium's synthesis | default only |
| `clipboard`: reads and writes | keys, `execCommand('copy')` and the context menu copy and paste as in Chrome; `navigator.clipboard.writeText()` and `write()` after a click are the `clipboard-write` permission, granted by default as Chrome grants it; `readText()` and `read()` are the `clipboard-read` permission, denied without a decision (`permission_manager.cc`) | done (the registry still says default only) |
| `protocol-handler`: `registerProtocolHandler()` | the `protocol-handler` permission, with the scheme and the handler's URL, denied without a hook; a handler granted handles its links in the profile; nothing is registered with the system, and nothing says it is (`protocol_handler_delegate.cc`) | done |

### 5.6 Notifications and the desktop

| Surface | Behaviour | Status |
|---|---|---|
| `notification.web`: `new Notification()`, a service worker's `showNotification()` | the `notifications` permission, undecided ("default") and denied without a decision; once granted, `zebkit_on_notification` shows them, or the system does: `org.freedesktop.Notifications` on Linux, UserNotifications on macOS for an application with a bundle identifier, a notification area balloon on Windows (a toast on Windows 10 and later; one at a time) (`notifications.cc`); clicks and closes reach the page and its service worker | done |
| `notification.push` | `GetPushMessagingService` null | gap (needs a push service; likely out of scope) |
| `desktop.tray`: status item, tray icon | `zebkit_tray_new()`: `NSStatusItem` on macOS, `Shell_NotifyIcon` on Windows, a StatusNotifierItem with a `com.canonical.dbusmenu` menu on Linux (ported from Chrome's, `tray_linux_dbus.cc`) | done |
| `desktop.share`: `navigator.share()` | `zebkit_view_on_share`: the application shares it; without a function macOS's share sheet, cancelled elsewhere (`share.cc`) | done |

### 5.7 Appearance, locale, accessibility

| Surface | Behaviour | Status |
|---|---|---|
| `appearance.color-scheme` | light by default, as before; with the startup setting `appearance.follow` the system's colour scheme, contrast and forced colours and their changes (`NativeTheme`, on Linux the XDG portal's `color-scheme`, `appearance.cc`); per view `colorScheme` and `contrast` in `zebkit_view_set_settings` still win; `zebkit_get_environment()` and `zebkit_on_environment_changed()` | done |
| `appearance.reduced-motion` | the system's; nothing of the application's to change it but automation's media emulation (forced colours follow the system with `appearance.follow`, above) | partial |
| `locale` | the system's, from Chromium 157 on (`patches/1083`); `Accept-Language` from it | done |
| `accessibility.platform` | the trees when an assistive technology asks: on macOS VoiceOver's `AXEnhancedUserInterface`, `AXManualAccessibility` and a read of the application's role (`application_mac.mm`, as Chrome's), also in an application's own `NSApplication` when it owns the loop, elsewhere content's detection; the environment's `accessibility` says which; no client reads AT-SPI, NSAccessibility or UI Automation on the test machines | partial: the trees' reading is not tested |

### 5.8 Files, other programs, network

| Surface | Behaviour | Status |
|---|---|---|
| `download` | `zebkit_view_on_download` decides; default `<profile>/Downloads`, no dialog, never opened (`download_manager.cc`); `zebkit_view_on_download_progress` reports each download to its end, with its outcome (`complete` with the saved path, `cancelled`, `failed` with the error's category and Chromium's interrupt reason) (`tests/engine/download_outcomes.py`) | done |
| `external-protocol`: `mailto:` and others | `zebkit_view_on_external_protocol` tells; nothing opened | done (the application opens it) |
| `network.proxy`, `network.certificates`, `network.client-certificates` | startup `proxy`; certificate and client-certificate hooks; the system's trust store | done |
| `storage.cookie-keys` | the OS key store, a key file, or a host key (`zebkit_set_cookie_storage`, `zebkit_set_cookie_encryption_key`) | done |
| `devtools`, `cdp` | off unless asked (`zebkit_cdp_listen`, the switch); 127.0.0.1 by default | done |

### 5.9 Power, session, displays

| Surface | Behaviour | Status |
|---|---|---|
| `power.wake-lock` | the screen wake lock granted; system wake lock via Chromium's device service | default only |
| `power.events`: suspend, resume, lock, battery | `zebkit_on_power_event`: suspend, resume, the power source, from `base::PowerMonitor`; the session's lock and unlock from `ui::CheckIdleStateIsLocked()` (the desktop's screen saver on Linux), checked each second while a function is set; `zebkit_prevent_sleep()` (`system_events.cc`) | done |
| `display.list`: screens, scale, work area, changes | `zebkit_get_displays()`, `zebkit_on_displays_changed()` from `display::Screen` | done |

## 6. The common contract

What every surface follows, so that an application learns it once.

### 6.1 Defaults

Each surface's default is written in its row of the registry (6.6), with
the test that holds it where there is one (many rows have none yet: the
registry's `tests` is empty for them). A default may depend on the
placement (section 4) and on capabilities, never on anything else: not on
the build, not on an environment variable the application cannot see.

Where a default presenter exists, it is the system's own UI if the system
has one (a file dialog, an alert), else the engine's (Chromium's views,
AppKit) if small, else none, and the request gets its "no presenter"
answer: denied for a permission, cancelled for a chooser, refused for an
external action. That answer is part of the surface's definition; "wait
forever" is never one.

### 6.2 Decisions: the hook model, unchanged

Decisions use `docs/HOOKS.md` as it is: asked and answered later
where Chromium waits, a setting where Chromium decides on the spot, an
event where nothing is decided; a deadline per request (30000 ms by
default, 1 to 300000); requests of a page that went away answered for the
application. What the surfaces of this document follow:

- **The default is the absence of a function.** A request goes to the
  application's function when one is set, and to the default presenter
  when none is. There is no answer that hands one request back to the
  default presenter (the design had one; it was not built): an
  application that wants the default for some requests removes its
  function, or presents those itself.
- **Reasons of the end.** A request's result event (the `_result`
  functions of `<zebkit/hooks.h>`, and `zebkit_view_on_download_result`)
  says what was decided, `state`, and how it ended, `reason`, in one form
  for every hook (`docs/HOOKS.md`, "The outcome"): `answered`, `timeout`,
  `cancelled` (the page, the frame or the view went away),
  `handler-changed`, `opener-closed` (a popup whose opener closed),
  `navigation`, and `shutdown` when the engine stops; a few have their own
  (`policy-changed`, `no-key`). A refusal by the system (as
  macOS's camera prompt can) is not a reason of its own.
- **Handler replaced.** Removing or replacing a function cancels its
  pending requests; it never leaves them hanging (`hook_requests.h`).

### 6.3 Configuration

Configuration is JSON at the C boundary, as the settings functions are:
validated whole, applied whole or not at all, unknown keys and bad values
an error naming the key. Inside the engine it is typed structures
(`WindowOptions`, `window_options.h`); platform code never parses JSON.

Every field belongs to one of four lifetimes:

| Lifetime | Set | Changed later |
|---|---|---|
| startup | `zebkit_init_with_settings()` | never: needs a restart |
| context | `zebkit_context_new()` (it takes no options) | never: needs a new context |
| creation | the view's options (section 7.3) | `requires-recreate` error |
| runtime | the surface's setter | yes |

Scopes, from wide to narrow: engine, context, view, request. A field
declares which scopes it has; the narrowest set wins, and a popup starts
with its opener's, as the runtime settings do. No deep merging of arrays:
an array replaces the one before.

Getters return what was asked and what holds: the runtime settings as
`{"explicit": ..., "effective": ...}`, a window's state with
`frame.requested` and `frame.effective` and `options.explicit` (7.4).
The design also named where each effective value came from; that was not
built.

### 6.4 Capabilities

```c
/* <zebkit/window.h>: what this build can do on this system and display, as
 * JSON: {"window": {"frames": [...], "position", "actions",
 * "controlsDrawnByEngine", "controlsHeight", "appearance"}, "headless",
 * "platform"}. Caller frees. */
char *zebkit_get_capabilities(void);
```

A capability is what the engine found, not what an OS normally has: on
Linux, X11 and Wayland differ (Wayland's compositor places windows, so
`position` is false there). Some answers are only known once a window
exists, so a window's own state (7.4) has its capabilities too, and wins.
The capabilities are of windows only; other surfaces say what the system
cannot do through their own result (a tray icon a desktop without a host
for it does not show, a badge function returning false). There is no
change event for them (the design had one; it was not built).

### 6.5 Threads, lifetimes and bindings

As everywhere in zebkit: every call and callback on the engine's thread;
strings returned are the caller's to free with `zebkit_free()`; strings
given to a callback are valid during it; a callback may close its view,
and the close happens after it returns. Each C function has its Rust,
Python, Go and host JavaScript form with the same names, values and
errors; `scripts/check-bindings.py` fails when a binding lacks one. A
binding never gives more than C does. The tests of these surfaces run
mostly through the Python binding (`tests/engine/window_frame.py`,
`system_holes.py` and the others), with host JavaScript in
`tests/engine/script/system.mjs`; the hooks of `docs/HOOKS.md` run the same
cases in C, Rust, Python and host JavaScript (`tests/engine/hook_policies.py`).

### 6.6 The registry, and how "complete" is checked

The surfaces are in `src/engine/surfaces.json`, one entry per surface:

```json
{
  "id": "dialog.file-system-access",
  "status": "done",
  "default": "the system's picker through the file chooser hook; reads what was picked, writes where it saves; more is the file-system-read/-write permission",
  "control": ["decide", "present", "observe"],
  "tests": ["tests/engine/system_holes.py"]
}
```

and, per content class, every virtual method, as reaching the system or
the person (with its surface, and whether zebkit implements it) or as
plumbing:

```json
"BrowserContext": {
  "header": "content/public/browser/browser_context.h",
  "system": {"GetFileSystemAccessPermissionContext": {"surface": "dialog.file-system-access", "zebkit": true}},
  "plumbing": [...]
}
```

A surface no content method reaches (the menu, the tray) is marked
`zebkit_only`. The status is one for every system; the rows of section 5
say where a system differs.

`scripts/check-surfaces.py` keeps the list honest, and runs with the engine
tests where Chromium's tree is (`engine-test.sh`, section `surfaces`). For
each delegate class the engine implements (`WebContentsDelegate`,
`ContentBrowserClient`, `BrowserContext`, `WebContentsViewDelegate`,
`PermissionControllerDelegate`), it reads every virtual method from
Chromium's header and fails when one is in neither list of the registry. A
Chromium update that adds a method fails it until the method is
classified. It also fails when a method the registry says zebkit
implements is not overridden in `src/engine`, or one it leaves to Chromium
is; when a surface's test is not there; and when a surface is missing from
this document. On 157: 564 methods, 112 of them reaching the system or the
person, in 67 surfaces. The tables of section 5 are written by hand, not
generated; the check holds them to the registry's surfaces, not to its
wording.

Done means: no entry point to the system that is not in the registry, and
no entry whose status the tests do not hold. A release's notes list the
gaps that remain.

## 7. Windows

The first surface built (phase 3), because it is the one an application
sees first, and because it needs what every other surface needs: options
before the native object exists, capabilities, state and events. The C
contract is `<zebkit/window.h>`.

### 7.1 What it achieves

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
| `system` (the default) | the system's title bar and buttons | below the title bar, as before |
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
`WebContentsDelegate::GetDisplayMode`, which `Window` implements
(`window.cc`). A page written for Chrome's installed apps or for
Electron's overlay works unchanged, and no zebkit-specific layout API is
needed for the page.

A frame is fixed at creation: changing it at runtime returns
`requires-recreate`. Recreating an `NSWindow`'s style or a Views frame
under a live page changes focus, size and attachment in ways not worth
the risk; it can be designed later on its own.

A frame the system cannot give is an error at creation (`unsupported:
frame ...`, `window_api.cc`), unless the options say `"fallback":
"system"`; then the window has the system frame and the state's
`frame.effective` says `system` beside `frame.requested`.

### 7.3 Creating a window with options

The window is configured before its native object exists, so options go
to creation: `NewView` (`view.cc`) takes the parsed `WindowOptions` to
`Window::Create`, and a setter after `zebkit_view_new()` would be too late
for a frame.

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
| `controls` | runtime | the system's | `height` of the title bar area, at least the system's; `color` and `symbolColor` of buttons the engine draws (Windows, Linux); ignored where the system draws them (macOS), which the capability `controlsDrawnByEngine` says |
| `background` | runtime | white | the window's colour before the page's first frame and where the page is transparent; avoids a white flash in a dark application |
| `appearance` | runtime | `system` | `system`, `light`, `dark`: the frame's and the buttons' look, honoured on macOS (`NSAppearance`); elsewhere the frame has the toolkit's colours, and the capability `appearance` is false. Does not change the page's `prefers-color-scheme` (11) |
| `show` | creation | true | false: made hidden, not activated; `zebkit_view_window_action(view, "show")` later |
| `pageFullscreen` | runtime | `content` | 7.6 |
| `dragRegions` | runtime | `none` | 7.5 |
| `fallback` | creation | `error` | 7.2 |

`zebkit_view_new()` and `zebkit_view_new_in()` stay as they were: `{}`,
with `pageFullscreen: "content"`. Popups made by a page
(`zebkit_view_on_window_open`) have the system frame; an application that
wants another frame for a popup decides before it is made (7.8).

### 7.4 State, actions and events

```c
/* The window's state as JSON (below); NULL for a closed view. Caller frees. */
char *zebkit_view_get_window(zebkit_view *view);

/* Runtime fields of the options above, merged by key; width, height,
 * frame, show and fallback are requires-recreate. Validated whole,
 * applied whole. */
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
  "pageFullscreen": false,
  "visible": true, "focused": true,
  "bounds": {"x": 100, "y": 80, "width": 1200, "height": 840},
  "content": {"width": 1200, "height": 800},
  "scale": 2,
  "frame": {"requested": "overlay", "effective": "overlay"},
  "titlebarArea": {"x": 78, "y": 0, "width": 1122, "height": 40},
  "controls": [{"x": 12, "y": 13, "width": 54, "height": 14}],
  "capabilities": {"frames": ["system", "overlay", "none"], "position": true, "actions": true, ...},
  "options": {"explicit": {"frame": "overlay", ...}}
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
  `application` (the host's window), `view` (another view's,
  `zebkit_view_attach()`) or `none` (off screen, no window); the state is
  what the engine can see, and actions fail.

### 7.5 Dragging the window

Two sources, one per window, chosen in `dragRegions.source`:

- **`css`**: the page's `app-region: drag` and `no-drag` (`-webkit-app-region`
  too), the standard way, as Chrome's installed apps and Electron use. The
  engine calls `WebContents::SetSupportsDraggableRegions(true)` and takes
  `WebContentsDelegate::DraggableRegionsChanged` (`window.cc`). What the
  157 source shows: the property is stable (`CSSWindowDrag`); Blink collects regions
  only when the page is told it supports them; it collects them from the
  **main frame's document only** (`WebViewImpl::DraggableRegionsChanged`
  reads `MainFrameImpl()->GetDocument()`), so no iframe, same-site or not,
  can contribute a region; and it sends them in DIPs
  (`BlinkSpaceToEnclosedDIPs`). The engine sets it again when the primary
  page changes (`Window::PrimaryPageChanged`).
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
can paint, and it gets none; a renderer crash drops them too
(`Window::PrimaryMainFrameRenderProcessGone`). Regions are
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

- **The page's** (`element.requestFullscreen()`): by default it fills the
  content area only, as before 0.4.0. Chrome makes the browser window
  fullscreen for it. `pageFullscreen: "window"` does what Chrome does in
  an engine window: the window goes fullscreen with the page and comes
  back when the page leaves; `"content"`, the default, keeps the earlier
  behaviour. The fullscreen policy still decides whether the page may.
- **The window's** (`zebkit_view_window_action(view, "fullscreen")`, the
  green button, F11 by the application's menu): the window, with the page
  in its normal state.

The state reports both (`state: "fullscreen"` and `pageFullscreen`), and
`GetDisplayMode` answers `fullscreen` for either, as Chrome does.

### 7.7 Per system

**macOS** (`window_mac.mm`):

- The window is made hidden and shown once its page is in it
  (`SetContents()`), unless `show` is false: the first frame the person
  sees is the window as configured, for every frame, not only the new
  ones. It does not wait for the page's first painted frame.
- `overlay`: `Titled | Closable | Miniaturizable | Resizable` kept,
  `FullSizeContentView` added, `titlebarAppearsTransparent` and
  `titleVisibility = hidden` set. The system keeps its buttons, shadow,
  rounded corners, fullscreen and tiling. `controls` and `titlebarArea`
  come from the buttons' real frames (`standardWindowButton:`), converted
  to the content view's coordinates, and from the content layout rect, as
  the state is read. A `controls.height` taller than the system's is the
  system's own taller title bar: an `NSToolbar` with the unified compact
  style above 28, the unified style from 46, which places the buttons as
  the system lays them out; no button is placed by coordinates.
- `none`: `Borderless` is not used: it loses key-window behaviour, the
  menu's window commands and the resize edges. `none` is `overlay` with
  the three buttons hidden.
- Dragging: the window (`ZebkitNSWindow`'s `sendEvent:`), on a mouse down
  in a drag region, calls `-[NSWindow performWindowDragWithEvent:]` with
  that event, so the system moves the window, with its snapping and Stage
  Manager; a double click does what the person set for a title bar
  (`AppleActionOnDoubleClick`). Not `movableByWindowBackground`, which
  would take the whole page.
- Fullscreen: `toggleFullScreen:`; state from the window's
  `windowDidEnterFullScreen:` and `windowDidExitFullScreen:`, not assumed.

**Windows and Linux** (`window_views.cc`, one implementation on Views):

- Views shows the window in `SetContents()`, unless `show` is false; the
  background colour is set there too.
- `overlay` and `none`: the widget made with `remove_standard_frame`, and
  a `views::FrameView` of zebkit's (`ZebkitFrameView`), installed with
  `WidgetDelegate::SetFrameViewFactory` (the 157 names; there is no
  `CreateNonClientFrameView` any more). Its `NonClientHitTest` returns the
  resize edges (`HTLEFT` ... `HTBOTTOMRIGHT`), the buttons (`HTMINBUTTON`,
  `HTMAXBUTTON`, `HTCLOSE`), `HTCAPTION` in drag regions, and `HTCLIENT`
  elsewhere. Returning these, not moving the window from mouse events, is
  what keeps the system's behaviour: on Windows, Aero Snap and the system
  menu, and Windows 11's snap layouts on hover over the maximize button,
  which need `HTMAXBUTTON` (not confirmed by a test, section 19); on
  Linux, Ozone turns `HTCAPTION` into the window manager's move
  (`_NET_WM_MOVERESIZE` on X11, which `tests/engine/window_frame.py`
  sees), which on Wayland is `xdg_toplevel.move` with the press's serial.
- The buttons for `overlay` are drawn by the engine with Views'
  `FrameCaptionButton`, as Chrome draws its own caption buttons for Window
  Controls Overlay, 46 by 32 DIPs, in `controls.color` and `symbolColor`.
  They are the same code on Windows and Linux.

**No window**: every action fails; `zebkit_view_get_window()` reports
`owner: "none"`. Options are validated all the same, so a test of an
application's options runs headless (`tests/engine/window_frame.py` does).

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
| C contract | `src/engine/include/zebkit/window.h`; exported as every `ZEBKIT_API` function is (`zebkit.ver` and `mac/exports.txt` take every `zebkit_*` name) |
| Options and state | `window_options.cc/.h` (parsing, validation, typed `WindowOptions`), `window_api.cc` (the C functions, the state's JSON), `view.cc` (`NewView` passes the options to `Window::Create`), `window.cc/.h` (state, revision, regions snapshot, `DraggableRegionsChanged`, `GetDisplayMode`) |
| Platform interface | `window_platform.h`: `Create`, `SetContents`, `Capabilities`, `Geometry`, `PerformAction`, `OptionsChanged`, `DragRegionsChanged`, `SetFullscreen`; typed values, no JSON |
| macOS | `window_mac.mm`: `ZebkitNSWindow` takes the drag hit test |
| Views | `window_views.cc`: `ZebkitFrameView` and its buttons |
| Bindings | `bindings/rust/zebkit-rs` (raw), `bindings/rust/zebkit` (`View::new_with_options`, `window`, `set_window`, `window_action`, `on_window_state`, `set_drag_regions`, `set_popup_window_options`, `zebkit::capabilities`), `bindings/python/zebkit`, `bindings/golang` (`NewViewWithOptions`, `WindowAction`, `SetDragRegions`, ...), `script.cc` (`new zebkit.View(url, {window: {...}})`) |
| Tests | `tests/engine/window_frame.py`: headless, the options' validation, `requires-recreate`, the state's shape and refused actions; with a window (X11, Wayland under labwc, macOS, Windows), each frame, the title bar area against what the page reads, drag regions against real input (X11), another origin's `app-region` not honoured, a hidden start, the position, state events |
| Example | `examples/custom-titlebar/`: a page with tabs in an `overlay` title bar, CSS regions, in C (`main.c`) and JavaScript (`main.mjs`) |
| Docs | `docs/API.md` (the reference), `docs/MACOS.md`, `docs/WINDOWS.md` |

## 8. Menus, commands and keys

**Before 0.4.0**: `zebkit_app_set_menu()` took the whole menu as JSON, with
roles for the standard commands; on macOS it was the menu bar, elsewhere
only its keys worked. `zebkit_view_on_context_menu` took a page's context
menu with its context, and without it nothing was shown.

**As built** (`tests/engine/menu_state.py`, `tests/engine/context_menu.py`):

- **One item model.** An item is `{id, title, role, key, modifiers,
  enabled, checked, radioGroup, items}`, the same in the application menu
  (`zebkit_app_set_menu()`), the Dock menu (`zebkit_app_set_dock_menu()`)
  and a tray menu (`zebkit_tray_set_menu()`); each menu's function gets
  the chosen item's `id`. A context menu shown with
  `zebkit_view_context_menu_show()` takes `{id, title, enabled}` and the
  default items' ids. Roles (`undo`, `copy`, `paste`, `selectAll`,
  `minimize`, `fullscreen`, `quit`, ...) are the system's standard
  commands, run by the engine (on macOS their selectors on the responder
  chain, `engine_mac.mm`).
- **State changes without rebuilding.** `zebkit_app_update_menu_item(id,
  json)` sets `enabled`, `checked` and `title` of every item with that id;
  checking one of a radio group unchecks the others; a disabled item's key
  is left to the page. A function to fill a menu just before it opens
  (macOS `menuNeedsUpdate:`) is not offered: it would run the
  application's code while the system waits on the UI thread. The
  application updates state as it changes.
- **Menu bars on Windows and Linux** are not the engine's. An application
  that wants one draws it, in its toolkit or in its page (the overlay
  title bar is where most do now); the keys work through the engine. This
  is a decision, not a gap: on macOS the menu bar is the system's and
  belongs to the application object, elsewhere it is a part of the
  window's content.
- **Context menus.** Without a function, the engine shows a default menu
  with the system's look where there is a window (Back, Forward, Reload;
  Cut, Copy, Paste, Select All in editable fields; Copy Link Address, Open
  Link in New Window; Save Image As, Copy Image), drawn with Chromium's
  views menu (`views::MenuRunner`, `context_menu_views.cc`) on Windows and
  Linux and `NSMenu` on macOS (`context_menu_mac.mm`), the same items on
  each. With a function, it gets the context and the default items, and
  answers with nothing (no menu), the default items, or its own list
  mixing default ids and its own. The engine runs a default item itself
  (`zebkit_view_context_menu_run()`), against the context it captured: a
  page cannot make the engine "Save Image As" a URL it chose later.
- **Keys**, in this order: the system's own (which no application gets),
  the view's reserved keys (`zebkit_view_set_reserved_keys`), the page and
  the input method (on X11 the engine's IBus client, section 5.3), then
  the menu's keys when the page did not take them. Global shortcuts (keys
  that work while the application is in the background) are for the
  host's toolkit and are not in the engine.

## 9. Dialogs and choosers

Each chooser is a hook, with the system's dialog or the engine's as the
default presenter where there is one, and none without a window.

- **File System Access** went past the file chooser hook before 0.4.0. Its
  pickers now go through the same decision as `<input type=file>`
  (`patches/1084`): `zebkit_view_on_file_chooser` gets a request with
  `"api": "file-system-access"`, `"mode"` `open`, `save` or `directory`,
  the suggested name and the types. The profile provides a
  `FileSystemAccessPermissionContext` (`file_system_access.cc`): the page
  reads what was picked and writes where it saves; writing to a file it
  opened, or reading or writing beyond what was picked, is the
  `file-system-write` or `file-system-read` permission, denied by default
  as other permissions are. This was the first fix of this work (phase 2,
  `tests/engine/system_holes.py`).
- **JavaScript dialogs**: `NSAlert` on macOS (`js_dialog_mac.mm`),
  `MessageBoxW` on Windows (`js_dialog_manager.cc`), and Chromium's views
  dialog, the one Chrome shows, on Linux and for Windows' `prompt()`,
  whose message box has no text field (`js_dialog_views.cc`). Suppressed
  without a window, so that nothing waits (`tests/engine/js_dialogs.py`).
- **EyeDropper** is the application's: `zebkit_view_on_eye_dropper`
  answers with a colour; without a function it is cancelled. Chrome's eye
  dropper is its browser's, capturing the screen, and content has none to
  host.
- **Colour, date and `<select>`** stay Blink's in-page popups (a native
  menu for `<select>` in macOS windows): they work the same everywhere,
  need no window of the system's, and are drawn by the page's renderer.
  Not hooks.
- **HTTP authentication, client certificates**: without a function, the
  engine's sign-in dialog (`sign_in_dialog_views.cc`,
  `sign_in_dialog_mac.mm`, `tests/engine/sign_in.py`) and, when the
  system's stores offer identities (`clientCertificateStores`), a list of
  them (`certificate_picker_views.cc`, `certificate_picker_mac.mm`), where
  there is a window; still cancelled or none without a window.
- **Print**: no printing backend in the release build, for its size
  (section 19). `window.print()` is told to the application as a dialog
  and the page continues; `zebkit_view_print_to_pdf` makes the output, and
  the application prints it with the system's own command if it wants.
  The design's system print dialog was declined; print preview is
  Chrome's WebUI and not in the engine.

## 10. Permissions, devices and capture

The permission hook decides (`zebkit_view_on_permission`); what 0.4.0 added
is the parts that had no implementation behind the decision.

- **Screen capture** (`getDisplayMedia()`): a request to the media access
  hook with `"type": "display"` and the sources, answered with one: `view`
  (the view itself, with its sound when asked) or `screen` (the whole
  desktop, where WebRTC's capturer reaches one: X11, macOS, Windows)
  (`page_hooks.cc`, `tests/engine/display_capture.py`). Without a
  function it is refused. The system's picker as a default presenter (the
  XDG portal's on Linux, the system's content-sharing picker on macOS)
  was declined: the
  source is the application's to choose (section 19). Wayland's PipeWire
  capture is off in this build. The system's own consent stays the
  system's.
- **USB, HID, Serial**: a `UsbDelegate`, `HidDelegate` and
  `SerialDelegate` whose choosers are one hook,
  `zebkit_view_on_device_chooser`, with the device list following devices
  as they come and go, answered with one device or none
  (`device_choosers.cc`, `tests/engine/device_chooser.py`). No device
  without a function, and no chooser of the engine's own. A chosen device
  is the origin's for the engine's run, until `forget()`. Web Bluetooth
  has no delegate (section 19).
- **Pointer lock and keyboard lock** are permissions with a policy per
  view: pointer lock granted by default where there is a window, as
  before, keyboard lock denied.
- **`registerProtocolHandler()`** no longer reports a registration it did
  not make: the request is the `protocol-handler` permission, denied by
  default, and a handler granted handles its links in the profile
  (`protocol_handler_delegate.cc`). Registering the scheme with the system
  is the application's (section 15).
- **Clipboard**: writing after a click (`clipboard-write`, what a site's
  "Copy" button calls) is granted by default, as Chrome grants it; reading
  (`clipboard-read`) is denied unless the application permits it
  (`permission_manager.cc`).
- **Geolocation** on macOS uses the system's source through Chromium's
  `GeolocationSystemPermissionManager` (`client_defaults.cc`), behind the
  permission hook. On Linux and Windows there is no system provider; that part of the
  design was not built (`permission.geolocation.system`, partial).

## 11. Appearance, locale, accessibility

**Appearance.** Two things, kept apart: the window's look (its frame and
buttons, `appearance` in 7.3) and the page's `prefers-color-scheme`. Before
0.4.0 the page was light unless told otherwise, and the system's theme was
not read. Now (`appearance.cc`, `tests/engine/appearance.py`):

- The engine follows the system's colour scheme, contrast and forced
  colours, and their changes while running, through `ui::NativeTheme`
  (which reads the system on macOS and Windows) and, on Linux, the XDG
  portal's `org.freedesktop.appearance.color-scheme`. This is a change of
  default for the page, so it is a startup setting, `"appearance":
  {"follow": true}`, false by default.
- `zebkit_view_set_settings` `colorScheme` and `contrast` keep winning
  for their view.
- `zebkit_get_environment()` returns what the system says
  (`colorScheme`, `contrast`, `forcedColors`, `reducedTransparency`,
  `invertedColors`, `locale`, `following`, `accessibility`) and
  `zebkit_on_environment_changed` tells changes. The design's
  accent colour and per-screen scale were not added; the screens' scale
  is in `zebkit_get_displays()` (14). An application never changes the
  system's settings through zebkit.

**Locale.** From Chromium 157 the page's default locale is the system's
(`patches/1083`), en-US without one (`tests/engine/system_locale.py`);
`Accept-Language` follows it. There is no startup setting to
override it (the design had one; it was not built).

**Accessibility** is a requirement for every window the engine shows, not
an option:

- The platform tree is turned on when an assistive technology asks, as
  Chrome does: on macOS `ZebkitApplication` has Chrome's handling of
  `AXEnhancedUserInterface` and `AXManualAccessibility`
  (`application_mac.mm`), also in an application's own `NSApplication`
  when it owns the loop; on Windows and Linux content's detection does it.
  The environment's `accessibility` says which tree there is
  (`tests/engine/accessibility.py`).
- An attached or off-screen view's accessibility belongs in the host's
  tree; for off-screen views that needs a bridge the host builds on CDP's
  `Accessibility.getFullAXTree`. It is not built (section 19).
- Tests that read the real tree (AT-SPI, `NSAccessibility`, UI
  Automation, finding a page's button by its name and pressing it) were
  not written: no client that reads those trees is on the test machines.
  This is why `accessibility.platform` is partial.
- Whatever replaces a system UI (an engine-drawn button in an overlay, an
  engine dialog) has an accessible name and works from the keyboard.

## 12. Notifications and the desktop

- **Web notifications**: a `PlatformNotificationService` in the profile
  (`notifications.cc`), so that `Notification.permission` follows the
  permission hook: `"default"` until the application decides, where it
  said `"denied"` before, and requests denied without a decision. Once
  granted, `zebkit_on_notification` gets each one (title, body, icon,
  actions, tag, the origin) and shows it, telling what the person did with
  `zebkit_notification_clicked()` and `zebkit_notification_closed()`.
  Without a function the system shows them: `org.freedesktop.Notifications`
  over D-Bus on Linux, UserNotifications on macOS (for an application with
  a bundle identifier, and the person's consent), and on Windows a balloon
  of a notification area icon (`Shell_NotifyIcon`, which Windows 10 and
  later show as a toast; one at a time) in place of the design's
  toast API, which needs an application identity (AppUserModelID). Clicks and closes come back to the
  page's `onclick` and its service worker's `notificationclick`
  (`tests/engine/notifications.py`). Notifications of a service worker
  with no page open are shown only while the engine runs (no background
  launch).
- **Push** needs a push service, which is Google's for Chrome; not in the
  engine. A hook could carry an application's own later.
- **Tray and status item**: `zebkit_tray_new(png, size, tooltip, error)`,
  a menu of the item model (8), click events. `NSStatusItem` on macOS,
  `Shell_NotifyIcon` on Windows, a StatusNotifierItem with a
  `com.canonical.dbusmenu` menu on Linux (`tray_linux_dbus.cc`), where a
  desktop without a host for it shows none (`tests/engine/tray.py`). Small
  and common enough for the engine; nothing appears unless asked.
- **Dock and taskbar**: badge text, progress, the Dock menu, attention
  requests. `NSDockTile` on macOS, `ITaskbarList3` and `FlashWindowEx` on
  Windows, the launcher API (`com.canonical.Unity.LauncherEntry`) on Linux
  docks that know it (`dock.cc`, `tests/engine/dock.py`).
- **Web Share**: `zebkit_view_on_share`; without a function, the system's
  share sheet on macOS (`NSSharingServicePicker`, `share_mac.mm`) and
  cancelled elsewhere. The design's Windows share sheet was not built.

## 13. Files, other programs, printing

- **Downloads** stay as they were (`docs/HOOKS.md`): saved to the
  profile's `Downloads` without a dialog and never opened, unless
  `zebkit_view_on_download` decides. Each download's report
  (`zebkit_view_on_download_progress`) ends with its outcome: `complete`
  with the saved path, `cancelled`, or `failed` with the error's category
  and Chromium's interrupt reason (`tests/engine/download_outcomes.py`).
  The design's `"downloads": "ask"` value that would show the system's
  save dialog was not built; `"ask"` is only what the view's effective
  `downloads` setting reports while a function decides, and an
  application that wants a dialog shows its own from its function.
- **Other programs.** `mailto:` and other external schemes stay told
  (`zebkit_view_on_external_protocol`) and not opened. For the
  application, `zebkit_open_external(url)` and `zebkit_show_in_folder(path)`
  call the system's handler (`NSWorkspace` on macOS, `ShellExecuteW` on
  Windows, `xdg-open` and `org.freedesktop.FileManager1` on Linux;
  `desktop_actions_mac.mm`, `desktop_actions_win.cc`,
  `desktop_actions_linux.cc`) with the URL or path as one argument, never
  through a shell (`tests/engine/system_services.py`). Pages never reach
  them except through the application.
- **Printing**: section 9.

## 14. Power, session, displays

`system_events.cc`, `tests/engine/system_services.py`,
`tests/engine/session_lock.py`:

- `zebkit_on_power_event`: suspend, resume and the power source from
  Chromium's `base::PowerMonitor`, which the engine runs; the session's
  lock and unlock from `ui::CheckIdleStateIsLocked()` (the desktop's
  screen saver on Linux), checked each second while a function is set.
- `zebkit_get_displays()` and `zebkit_on_displays_changed()`: bounds,
  work area, scale, rotation, primary, from `display::Screen`.
- Keeping the system awake for the application (not for a page, which has
  the wake lock API) is `zebkit_prevent_sleep(keep_display_on, reason)`
  returning a token, released by `zebkit_allow_sleep(token)` or by the
  engine's end. Session end (logout) arrives as a quit request where the
  system gives one, as on macOS.

## 15. What the engine does not do

These change the system for longer than the process, and are the
installer's or the application's, not a library's:

- registering URL schemes and file types with the system, login items,
  default-browser status, Start menu and desktop entries, the Dock's
  permanent icon;
- writing the system's settings (theme, proxy, keychain entries the
  application does not own).

The engine receives what they deliver at runtime: files opened
(`zebkit_app_on_open_files`); a URL the system gives the application
(`app.open-url`, 5.1) is not received yet. How each system's packaging
declares them (`Info.plist`, a `.desktop` file, the Windows registry) is
not documented here or in `docs/API.md`.

## 16. Security and privacy rules

- A page gets nothing of this document without a grant to its origin;
  grants are per view and never inherited by a popup.
- No surface reaches the system without a decision the application can
  take. The File System Access picker (9) and protocol handlers (10) were
  the exceptions before 0.4.0, and were fixed first (phase 2).
- The engine never asks the system for a sensitive capability (camera,
  location, notifications, screen) without a request from a page or the
  application that the decision allowed.
- Commands run from menus run against the context the engine captured.
- Window regions are dropped at a cross-origin navigation and a renderer
  crash (`window.cc`).
- Events that carry personal data (a notification's body, a credential, a
  pairing code) are never logged by the engine, as for WebAuthn.
- CDP stays off unless asked, on 127.0.0.1 by default.

## 17. Order of work

The work was done in seven phases, each finished when its tests passed on
Linux, macOS and Windows; cases a machine cannot run are reported, never
counted as passed. All seven are in 0.4.0.

| Phase | Content | Held by | Status |
|---|---|---|---|
| 1. Registry | `src/engine/surfaces.json` from section 5; `scripts/check-surfaces.py` in `engine-test.sh` (section `surfaces`) | the check passes on 157 and names every unclassified method; section 5 is written by hand and checked to name every registry surface | done |
| 2. Holes | File System Access through the file chooser hook and the permission table (`patches/1084`); protocol handlers asked, not faked; pointer and keyboard lock as permissions | `tests/engine/system_holes.py`, headless and in windows on X11, macOS and Windows: each denied by default and decided by a hook | done |
| 3. Windows | section 7: options at creation, the macOS first frame, `system`, `overlay`, `none`, Window Controls Overlay, CSS and host regions, state, actions, events, popups' options, page fullscreen | `tests/engine/window_frame.py`, headless and in real windows on X11, Wayland (labwc), macOS and Windows; `examples/custom-titlebar` | done |
| 4. Appearance and accessibility | following the system's theme and its changes; the environment query; the platform tree on demand | `tests/engine/appearance.py` (on Linux through a fake XDG portal), `tests/engine/accessibility.py` | done, but no test reads a platform tree (11) |
| 5. Menus and dialogs | menu item state by id; the default context menu and running its commands; Chromium's views dialogs for JavaScript dialogs and sign-in on Linux and Windows, a client certificate list; the eye dropper as the application's | `tests/engine/menu_state.py`, `context_menu.py`, `js_dialogs.py`, `sign_in.py`: the default, a hook, and no window | done |
| 6. Notifications and desktop | web notifications; tray; Dock and taskbar; Web Share; `zebkit_open_external`, `zebkit_show_in_folder` | `tests/engine/notifications.py`, `tray.py` (on Linux against fake desktop services on a private bus), `dock.py`, `system_services.py`, `tests/engine/script/system.mjs` | done |
| 7. Capture, devices, power | screen capture through the media access hook, the source the application's choice; device choosers for USB, HID and serial; displays; power events and the session's lock; keeping the system awake | `tests/engine/display_capture.py`: `getDisplayMedia()` answered with the application's choice of source; `device_chooser.py`, with the machine's own USB, HID and serial devices where it has them ("not run" where it has none); `system_services.py`, `session_lock.py` | done |

Phase 7 as first written also asked for a print dialog and for
`getDisplayMedia()` through the system's picker. Both were declined
(section 19): the release build has no printing backend, and the source
of a capture is the application's to choose.

Phase 1 and 2 came before anything new, because they made the rest
measurable and closed what reached the system without the application.
Phase 3 was the first user-visible one and the template for the API shape
of the others. The size of each phase was measured as for any change
(`docs/SIZE_AND_SPEED.md`): the 0.4.0 archives are 0.2 to 0.4 MiB larger
unpacked than 0.3.0's (`docs/releases/0.4.0.md`), as phase 6's services
are D-Bus and system frameworks, already linked, not new libraries.

What remains open is listed in section 5 with its reason: the system's
screen-capture picker as a default (declined), Web Bluetooth, the reading
of platform accessibility trees in tests, and physical printing (declined
for the release build, section 19). Checks that need what a test machine
lacks (a focused window on a locked macOS session, a device on a machine
without one) report "not run".

## 18. Testing

- **Each surface**: the default; each answer; the page or view going away
  while it waits; the handler replaced; no window. The hooks of
  `docs/HOOKS.md` run the same cases through C, Rust, Python and host
  JavaScript (`tests/engine/hook_policies.py`); the surfaces of 0.4.0 are
  tested through the Python binding, with host JavaScript in
  `tests/engine/script/system.mjs`, and `scripts/check-bindings.py`
  checks that Rust, Python, Go and host JavaScript each have every C
  function.
- **Real windows, not screenshots of pages.** CDP's screenshot is the
  page; a frame's correctness is in the system's window.
  `tests/engine/window_frame.py` runs on X11 in Xvfb with xdotool's real
  presses and drags, and xev to see the move asked of the window manager;
  on Wayland under labwc, as `native-window-wayland` runs; and in windows
  of macOS and Windows, where the system's window manager takes the
  actions.
- **Fake desktop services, not skipped ones.** On Linux, notifications,
  the tray, the appearance portal, the screen saver and IBus are fakes on
  a private bus (`fake_notifications.py`, `fake_tray_watcher.py`,
  `fake_portal.py`, `fake_screensaver.py`, `fake_ibus.py`), so the
  engine's side of each protocol is tested without a desktop.
- **No silent skip.** Each test reports passed, failed, unsupported (with
  the capability that said so) or not run; the release notes count each.
- **Security cases**: a document of another origin setting `app-region`
  in a window whose regions are for a granted origin (`window_frame.py`).
- **Cost**: like the network hook, a surface costs nothing until it is
  set, as `tests/bench/web/` checks for that hook. The frame time of a
  page animating a region was not made a test.

## 19. Decisions, and what is open

Decided:

| Decision | Reason |
|---|---|
| The earlier behaviour stays the default, of the old entry points and of the new ones (`pageFullscreen: "content"`) | no application changes without asking |
| No presets ("desktop", "embedded", "headless" as bundles of defaults) | they hide what a default is and multiply what must be tested; ownership (section 4) and capabilities explain the same differences directly |
| `overlay` is Window Controls Overlay | content has it; pages written for Chrome and Electron work; no layout API of zebkit's for pages |
| Frames are fixed at creation | changing a live window's frame risks focus, size and attachment; later, separately |
| Drag regions from the main frame only, for granted origins | Blink collects them from the main frame's document only; a grant keeps other origins out |
| A macOS window is shown once its page is in it, without waiting for the page's first painted frame | it removes the first frame without its page, which is what 7.1 asks; the page's first paint is not waited for |
| A taller title bar on macOS is the system's unified toolbar style | the system places its buttons; none is moved by coordinates |
| No menu bar on Windows and Linux from the engine | it is window content there, the application's to draw |
| No global shortcuts, no install-level registration in the engine | the host's toolkit and the installer do them better, and they outlive the process |
| The system's consent is never replaced | the system's to give |
| No printing backend in the release build: `window.print()` is told to the application, `zebkit_view_print_to_pdf` makes the output | the backend and its dialogs cost size for what an application does with the PDF and the system's own print command |
| Device choosers, the screen-capture source and Web Share (but macOS's share sheet) are the application's to present; the engine has no chooser of its own | a list of devices or screens is the application's UI, in its style; the hook gives it everything it needs |
| Web Bluetooth stays unsupported | its scanning, pairing and PIN prompts are a separate chooser, with the system's Bluetooth stack under it |

Open:

- Whether Windows 11's snap layouts appear over the engine-drawn maximize
  button, which returns `HTMAXBUTTON` as Chrome's does; not checked by a
  test.
- The accessibility bridge for off-screen views (11): what the host
  needs, and whether CDP's tree is enough.
- Tests that read the platform accessibility trees, once a test machine
  has a client for them.
