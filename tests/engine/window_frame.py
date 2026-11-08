"""A view's window (<zebkit/window.h>, docs/SYSTEM_INTEGRATION.md section 7):
options before the window is made, its frame, the title bar area the page sees
(Window Controls Overlay), where it is dragged from, its state and its events.

    python3 tests/engine/window_frame.py [engine switches...]

Without a window (--headless) it checks what does not need one: the options'
validation, requires-recreate, the state's shape and refused actions. With one
(X11, Wayland) the frames, the title bar area against what the page reads,
drag regions against real clicks (xdotool, X11), a hidden start and its show,
the position, and state events. Exit 0 when every check passes.
"""

import http.server
import json
import os
import shutil
import subprocess
import sys
import threading
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "bindings", "python"))

import zebkit  # noqa: E402

failures = []
checks = 0

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


def check(ok, what, detail=None):
    global checks
    checks += 1
    print(("ok   " if ok else "FAIL ") + what + (": %r" % (detail,) if detail is not None else ""), flush=True)
    if not ok:
        failures.append(what)


class Pages(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        self.send_response(200)
        self.send_header("Content-Type", "text/html")
        self.send_header("Content-Length", str(len(PAGE)))
        self.end_headers()
        self.wfile.write(PAGE)

    def log_message(self, *args):
        pass


def wait(condition, seconds=15):
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        zebkit.pump()
        if condition():
            return True
        time.sleep(.002)
    return False


def pump(seconds):
    wait(lambda: False, seconds)


def ev(view, script, seconds=15):
    box = []
    view.evaluate(script, lambda value, error=None: box.append((value, error)))
    if not wait(lambda: box, seconds):
        return ("timeout", script)
    return box[0][0] if box[0][1] is None else ("error", box[0][1])


def loaded(view, url):
    loads = []
    view.on_load(lambda phase, u: loads.append((phase, u)))
    wait(lambda: (zebkit.LOAD_FINISHED, url) in loads)
    return wait(lambda: ev(view, "location.href + ' ' + document.readyState") == url + " complete")


def refused(function):
    try:
        function()
    except zebkit.Error as error:
        return str(error)
    return None


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


def window_id(title):
    found = subprocess.run(["xdotool", "search", "--name", "^%s$" % title], capture_output=True, text=True)
    ids = found.stdout.split()
    return ids[-1] if ids else None


def click(window, x, y):
    subprocess.run(["xdotool", "mousemove", "--window", window, str(x), str(y), "click", "1"], check=False)


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

    xid = window_id("Overlay") if shutil.which("xdotool") and os.environ.get("DISPLAY") and \
        not os.environ.get("WAYLAND_DISPLAY") else None
    if xid:
        ev(overlay, "downs.length = 0")
        click(xid, 100, 20)  # the bar: app-region: drag
        click(xid, 250, 20)  # the field: no-drag
        click(xid, 400, 300)  # the page
        pump(0.5)
        downs = ev(overlay, "JSON.stringify(downs)")
        got = json.loads(downs) if isinstance(downs, str) else []
        check([250, 20] in [[round(x), round(y)] for x, y in got] and [400, 300] in [[round(x), round(y)] for x, y in got],
              "clicks where it is not a drag region reach the page", got)
        check([100, 20] not in [[round(x), round(y)] for x, y in got],
              "a press in a drag region is the window's, not the page's", got)

        # A document of another origin sets no region.
        overlay.load(other + "/page")
        loaded(overlay, other + "/page")
        ev(overlay, "downs.length = 0")
        click(xid, 100, 20)
        pump(0.5)
        downs = ev(overlay, "JSON.stringify(downs)")
        got = json.loads(downs) if isinstance(downs, str) else []
        check([100, 20] in [[round(x), round(y)] for x, y in got],
              "another origin's app-region is not the window's", got)
    else:
        print("ok   drag regions with real clicks: not on this display (X11 with xdotool only)", flush=True)
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
    xid = window_id("Hidden") if shutil.which("xdotool") and os.environ.get("DISPLAY") and \
        not os.environ.get("WAYLAND_DISPLAY") else None
    if xid:
        ev(hidden, "downs.length = 0")
        click(xid, 50, 10)
        click(xid, 50, 100)
        pump(0.5)
        downs = ev(hidden, "JSON.stringify(downs)")
        got = [[round(x), round(y)] for x, y in (json.loads(downs) if isinstance(downs, str) else [])]
        check([50, 100] in got and [50, 10] not in got, "the application's regions are the window's", got)
    hidden.close()

    # Actions need a window manager: labwc's, under Wayland here.
    if os.environ.get("WAYLAND_DISPLAY"):
        acted = zebkit.View(base + "/page", 500, 400, options={"title": "Actions", "pageFullscreen": "window"})
        loaded(acted, base + "/page")
        for action, wanted in (("maximize", "maximized"), ("restore", "normal"), ("fullscreen", "fullscreen"),
                               ("exit-fullscreen", "normal")):
            acted.window_action(action)
            wait(lambda: acted.window["state"] == wanted, 5)
            check(acted.window["state"] == wanted, "the %s action: %s" % (action, wanted), acted.window["state"])
        acted.focus()
        ev(acted, "document.documentElement.requestFullscreen().then(() => 1, e => e.name)")
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
    servers = []
    for _ in range(2):
        server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Pages)
        threading.Thread(target=server.serve_forever, daemon=True).start()
        servers.append(server)
    base, other = ("http://127.0.0.1:%d" % server.server_address[1] for server in servers)
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
            server.shutdown()
    print("%d of %d checks passed" % (checks - len(failures), checks), flush=True)
    return 1 if failures or not checks else 0


if __name__ == "__main__":
    sys.exit(main())
