"""A view's window (<zebkit/window.h>, docs/SYSTEM_INTEGRATION.md section 7):
options before the window is made, its frame, the title bar area the page sees
(Window Controls Overlay), where it is dragged from, its state and its events.

    python3 tests/engine/window_frame.py [engine switches...]

Without a window (--headless) it checks what does not need one: the options'
validation, requires-recreate, the state's shape and refused actions. With one
(X11, Wayland) the frames, the title bar area against what the page reads,
drag regions against real input (X11: xdotool's presses and drags, and xev to
see the window's move asked of the window manager), a hidden start and its
show, the position, and state events. Exit 0 when every check passes.

On Linux, as in Chrome, a press in a drag region still reaches the page; a drag
from it is the window's: the window asks the window manager to move it
(_NET_WM_MOVERESIZE, sent to the root window), which xev sees with no window
manager running.
"""

import atexit
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from harness import (Server, activated, check, ev, finish, note, pump, refused, skip, wait, x11_window,  # noqa: E402
                     xdotool, zebkit)

PAGE = b"""<!DOCTYPE html><title>frame</title>
<style>
  html, body { margin: 0; height: 100%; background: #224; }
  #bar { position: fixed; top: 0; left: 0; right: 0; height: 40px; app-region: drag; background: #446; }
  #field { position: fixed; top: 4px; left: 200px; width: 120px; height: 30px; app-region: no-drag; }
  #area { position: fixed; top: 0; left: env(titlebar-area-x, 0); width: env(titlebar-area-width, 0);
          height: env(titlebar-area-height, 0); }
</style>
<div id=bar></div><input id=field><div id=area></div>
<script>
  window.downs = [];
  addEventListener('mousedown', e => downs.push([e.clientX, e.clientY]), true);
</script>"""


def loaded(view, url):
    loads = []
    view.on_load(lambda phase, u: loads.append((phase, u)))
    wait(lambda: (zebkit.LOAD_FINISHED, url) in loads, 15)
    return wait(lambda: ev(view, "location.href + ' ' + document.readyState") == url + " complete", 15)


def options(base):
    for bad, word in (({"frame": "round"}, "frame"), ({"width": -1}, "width"), ({"colour": "#fff"}, "colour"),
                      ({"background": "red"}, "background"), ({"x": 1}, "x and y"),
                      ({"dragRegions": {"source": "css"}}, "origins"),
                      ({"dragRegions": {"source": "css", "origins": ["not an origin"]}}, "origins")):
        message = refused(lambda: zebkit.View(base + "/page", options=bad))
        check(message is not None and word in message, "invalid options are refused before a window: %s" % word,
              message)
    view = zebkit.View(base + "/page", 640, 480, options={"title": "Options", "minWidth": 300, "minHeight": 200})
    loaded(view, base + "/page")
    state = view.window
    check(state and state["frame"] == {"requested": "system", "effective": "system"} and
          state["options"]["explicit"].get("title") == "Options", "the state has the frame and the options given",
          state and (state["frame"], state["options"]))
    message = refused(lambda: view.set_window({"frame": "none"}))
    check(message and message.startswith("requires-recreate"), "a frame cannot change on a made window", message)
    check(refused(lambda: view.set_window({"title": "Changed"})) is None, "a runtime option changes")
    check(view.window["options"]["explicit"].get("title") == "Changed", "and the state says so")
    check(refused(lambda: view.window_action("wiggle")) is not None, "an unknown action is refused")
    return view


def headless(base):
    view = options(base)
    state = view.window
    check(state["owner"] == "none" and state["state"] == "normal", "without a window: owner none", state["owner"])
    message = refused(lambda: view.window_action("minimize"))
    check(message and "unsupported" in message, "and window actions are unsupported", message)
    check(zebkit.capabilities()["window"]["frames"] == ["system"], "capabilities say only the system's frame",
          zebkit.capabilities())
    view.close()


def drag_input():
    """None when drag regions are checked with real input here (X11 with
    xdotool and xev); otherwise (skipped, why): not skipped where the suite's
    X11 run of this test has them (Wayland, macOS, Windows, no window)."""
    if os.environ.get("WAYLAND_DISPLAY") or not os.environ.get("DISPLAY") or not sys.platform.startswith("linux"):
        return False, "drag regions with real input: the X11 run's (window-frame-x11)"
    missing = [tool for tool in ("xdotool", "xev") if not shutil.which(tool)]
    return (True, "no " + " or ".join(missing)) if missing else None


class MoveRequests:
    """The _NET_WM_MOVERESIZE requests windows send to the root window, as xev
    sees them (SubstructureNotify on the root): a window manager's or not."""

    def __init__(self):
        self.log = tempfile.NamedTemporaryFile(prefix="window-frame-xev-", suffix=".log", delete=False)
        self.process = subprocess.Popen(["xev", "-root", "-event", "substructure"], stdout=self.log,
                                        stderr=subprocess.DEVNULL)
        atexit.register(self.close)
        pump(0.5)

    def count(self, window):
        with open(self.log.name, encoding="utf-8", errors="replace") as log:
            text = log.read()
        found = re.findall(r"window (0x[0-9a-f]+),\s*message_type 0x[0-9a-f]+ \(_NET_WM_MOVERESIZE\)", text)
        return sum(1 for value in found if int(value, 16) == int(window))

    def close(self):
        if self.process.poll() is not None:
            return
        self.process.terminate()
        self.process.wait()
        self.log.close()
        os.unlink(self.log.name)


def click(window, x, y):
    xdotool("mousemove", "--window", window, str(x), str(y), "click", "1")
    pump(0.05)


def drag(window, x, y):
    """A press at x, y, 30 pixels to the right in steps (past the drag
    threshold), the release; the engine pumped between them."""
    steps = [("mousemove", "--window", window, str(x), str(y)), ("mousedown", "1")]
    steps += [("mousemove_relative", "--", "5", "0")] * 6 + [("mouseup", "1")]
    for step in steps:
        xdotool(*step)
        pump(0.05)
    pump(0.3)


def presses(view):
    downs = ev(view, "JSON.stringify(downs)")
    return [[round(x), round(y)] for x, y in (json.loads(downs) if isinstance(downs, str) else [])]


def windowed(base, other):
    caps = zebkit.capabilities()["window"]
    check("overlay" in caps["frames"] and "none" in caps["frames"], "with a window: overlay and none frames", caps)
    view = options(base)
    check(view.window["owner"] == "engine", "the engine's window: owner engine", view.window["owner"])
    view.close()

    states = []
    overlay = zebkit.View(base + "/page", 800, 500, options={
        "frame": "overlay", "title": "Overlay", "background": "#222244",
        "controls": {"height": 40, "color": "#333355", "symbolColor": "#ffffff"},
        "dragRegions": {"source": "css", "origins": [base]}})
    overlay.on_window_state(states.append)
    loaded(overlay, base + "/page")
    pump(0.5)
    state = overlay.window
    area = state["titlebarArea"]
    check(state["frame"]["effective"] == "overlay" and area["height"] >= 32 and area["width"] > 0,
          "an overlay has a title bar area", state["frame"], )
    page = ev(overlay, "JSON.stringify([navigator.windowControlsOverlay.visible, "
                       "navigator.windowControlsOverlay.getTitlebarAreaRect(), "
                       "matchMedia('(display-mode: window-controls-overlay)').matches, "
                       "document.getElementById('area').getBoundingClientRect()])")
    if isinstance(page, str):
        visible, rect, display_mode, env = json.loads(page)
        check(visible and display_mode, "the page sees Window Controls Overlay and its display mode", page)
        check(abs(rect["x"] - area["x"]) <= 1 and abs(rect["width"] - area["width"]) <= 1 and
              abs(rect["height"] - area["height"]) <= 1,
              "navigator.windowControlsOverlay has the state's title bar area", (rect, area))
        check(abs(env["width"] - area["width"]) <= 1 and abs(env["height"] - area["height"]) <= 1,
              "and env(titlebar-area-*) lays the page out around the buttons", (env, area))
    else:
        check(False, "the page's view of the overlay", page)
    controls = state["controls"]
    check(controls and all(c["y"] < area["height"] for c in controls) and
          all(c["x"] >= area["x"] + area["width"] or c["x"] + c["width"] <= area["x"] for c in controls),
          "the buttons are in the title bar, outside its free area", controls)
    told = len(states)
    overlay.resize(820, 520)
    wait(lambda: len(states) > told and states[-1]["content"]["width"] == 820, 5)
    check(len(states) > told and states[-1]["content"]["width"] == 820, "a resize is told as a state event",
          states[-1:] or None)

    no_input = drag_input()
    if no_input and no_input[0]:
        skip("drag regions with real input", no_input[1])
    elif no_input:
        note(no_input[1])
    moves = None if no_input else MoveRequests()
    xid = moves and x11_window("Overlay")
    if moves and not check(xid is not None, "the overlay's X11 window is found"):
        moves.close()
        moves = None
    if moves:
        ev(overlay, "downs.length = 0")
        click(xid, 250, 20)  # the field: no-drag
        click(xid, 400, 300)  # the page
        pump(0.3)
        got = presses(overlay)
        check([250, 20] in got and [400, 300] in got, "clicks where it is not a drag region reach the page", got)
        drag(xid, 400, 300)
        check(moves.count(xid) == 0, "a drag where it is not a drag region does not move the window",
              moves.count(xid))
        drag(xid, 100, 20)  # the bar: app-region: drag
        check(moves.count(xid) == 1, "a drag in a drag region is the window's: a move asked of the window manager",
              moves.count(xid))

        # A document of another origin sets no region.
        overlay.load(other + "/page")
        loaded(overlay, other + "/page")
        ev(overlay, "downs.length = 0")
        drag(xid, 100, 20)
        check(moves.count(xid) == 1 and [100, 20] in presses(overlay),
              "another origin's app-region is not the window's", (moves.count(xid), presses(overlay)))
    overlay.close()

    placed = {"x": 120, "y": 90} if caps["position"] else {}
    if not caps["position"]:
        message = refused(lambda: zebkit.View(base + "/page", options={"x": 1, "y": 1}))
        check(message and "unsupported" in message, "x and y are refused where the compositor places windows", message)
    hidden = zebkit.View(base + "/page", 400, 300, options=dict(placed, **{
        "frame": "none", "show": False, "title": "Hidden", "dragRegions": {"source": "host"}}))
    loaded(hidden, base + "/page")
    check(not hidden.window["visible"], "show false: made hidden", hidden.window["visible"])
    hidden.set_drag_regions([{"x": 0, "y": 0, "width": 400, "height": 30}])
    message = refused(lambda: hidden.set_drag_regions([{"x": 0}]))
    check(message is not None, "malformed host regions are refused", message)
    hidden.window_action("show")
    wait(lambda: hidden.window["visible"], 5)
    check(hidden.window["visible"], "and shown by the show action", hidden.window["visible"])
    if caps["position"]:
        bounds = hidden.window["bounds"]
        check(abs(bounds["x"] - 120) <= 2 and abs(bounds["y"] - 90) <= 2, "x and y place it", bounds)
    check(hidden.window["frame"]["effective"] == "none" and hidden.window["titlebarArea"]["width"] == 0,
          "a frame none has no title bar area", hidden.window["titlebarArea"])
    xid = moves and x11_window("Hidden")
    if moves and check(xid is not None, "the hidden view's X11 window is found"):
        ev(hidden, "downs.length = 0")
        drag(xid, 50, 100)
        outside = moves.count(xid)
        drag(xid, 50, 10)
        check(outside == 0 and moves.count(xid) == 1 and [50, 100] in presses(hidden),
              "the application's regions are the window's, and only they", (outside, moves.count(xid)))
    if moves:
        moves.close()
    hidden.close()

    # Actions need a window manager: labwc's under Wayland here, AppKit's and
    # Windows' own; Xvfb has none.
    if os.environ.get("WAYLAND_DISPLAY") or sys.platform in ("darwin", "win32"):
        acted = zebkit.View(base + "/page", 500, 400, options={"title": "Actions", "pageFullscreen": "window"})
        loaded(acted, base + "/page")
        acted.focus()
        wait(lambda: acted.window["focused"], 3)
        if not acted.window["focused"]:
            # A locked screen (a Mac's here) gives no window focus, and no
            # fullscreen; nor does a compositor whose seat has no keyboard
            # (engine-test.sh gives its labwc wtype's): not passed, not failed.
            skip("window actions", "the window cannot have focus (no keyboard on the compositor's seat?)"
                 if os.environ.get("WAYLAND_DISPLAY") else "the window cannot have focus (a locked screen?)")
            acted.close()
            return
        for action, wanted in (("maximize", "maximized"), ("restore", "normal"), ("fullscreen", "fullscreen"),
                               ("exit-fullscreen", "normal")):
            acted.window_action(action)
            wait(lambda: acted.window["state"] == wanted, 5)
            check(acted.window["state"] == wanted, "the %s action: %s" % (action, wanted), acted.window["state"])
        acted.focus()
        entered = ev(acted, activated("document.documentElement.requestFullscreen().then(() => 1, e => e.name)"))
        check(entered == 1, "the page's requestFullscreen() is granted", entered)
        wait(lambda: acted.window["state"] == "fullscreen", 5)
        check(acted.window["state"] == "fullscreen" and acted.window["pageFullscreen"],
              "pageFullscreen window: the page's fullscreen is the window's", acted.window["state"])
        mode = ev(acted, "matchMedia('(display-mode: fullscreen)').matches")
        check(mode is True, "and the page's display mode is fullscreen", mode)
        ev(acted, "document.exitFullscreen().then(() => 1, e => e.name)")
        wait(lambda: acted.window["state"] == "normal", 5)
        check(acted.window["state"] == "normal", "leaving it leaves the window's", acted.window["state"])
        acted.close()


def main():
    servers = [Server(PAGE) for _ in range(2)]
    base, other = (server.base for server in servers)
    if zebkit.init(["window-frame"] + sys.argv[1:]) != 0:
        print("FAIL zebkit.init()")
        return 1
    try:
        try:
            headless(base) if zebkit.is_headless() else windowed(base, other)
        except Exception as error:  # noqa: BLE001
            check(False, "the test ran to the end", repr(error))
    finally:
        zebkit.shutdown()
        for server in servers:
            server.close()
    return finish()


if __name__ == "__main__":
    sys.exit(main())
