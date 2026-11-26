# Unreleased: on `main` since 0.6.1

**A draft.** What `main` has that the 0.6.1 release does not (`git log
v0.6.1..main`); the bump turns it into the release's notes.

The window, customized from its options: kept above other windows (at three
levels on macOS), translucent, held to a size range or an aspect ratio,
without its buttons, unmovable, out of the taskbar, on every workspace,
without its shadow, kept out of screenshots; and centred or asking for
attention. One set of options and actions on Linux, macOS and Windows, with
the capabilities saying what each system honours. Apple's and Microsoft's
SDKs come from zsdk.

| Archive | System | Unpacked | `.tar.xz` |
|---|---|---:|---:|
| **To fill at release** | | | |

## What changed since 0.6.1

**Window customization** (`<zebkit/window.h>`, docs/API.md "The window").
New options of `zebkit_view_new_with_options()` and
`zebkit_view_set_window()`, all of them changeable while the window is open:

- `alwaysOnTop`, and `alwaysOnTopLevel` `"floating"`, `"status"` or
  `"screen-saver"`: above the other windows, every application's (a
  floating window's level on macOS, the window manager's `_NET_WM_STATE_ABOVE`
  on X11, `HWND_TOPMOST` on Windows). The state has `alwaysOnTop` as the
  system has it.
- `opacity` (0 to 1), `maxWidth` and `maxHeight`, `aspectRatio`.
- `minimizable`, `maximizable`, `closable`, `fullscreenable`, `movable`: what
  the person can do to the window; the application's minimize, maximize and
  fullscreen are refused too, its close is not.
- `skipTaskbar`, `visibleOnAllWorkspaces`, `shadow`, `contentProtection`.

New actions of `zebkit_view_window_action()`: `center`, `request-attention`
(the taskbar button flashes, the Dock's icon bounces) and `cancel-attention`.

What a system cannot do is in the state's `capabilities` (`alwaysOnTop`,
`onTopLevels`, `opacity`, `aspectRatio`, `contentProtection`, `buttons`,
`movable`, `skipTaskbar`, `visibleOnAllWorkspaces`, `shadow`); asking for it
fails with `unsupported: <option>`, naming the capability, and changes
nothing, while an option's default is always taken. Wayland has the buttons
only. What no option covers is the application's to do on the system's
window itself (`zebkit_view_get_native_window()`).

**SDKs from zsdk.** Apple's and Microsoft's SDKs for the macOS, iOS and
Windows builds come from zsdk at the commit `upstream.lock` pins
(`scripts/zsdk.sh`), each Windows version pinned on its own.

## Tested

**To fill at release**: the suite's counts on this build, and the runs on the
test machines.
