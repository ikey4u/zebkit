"""WebAuthn's system interface on Windows: webauthn.dll, through the host's own events.

    win_webauthn.py <webauthn host> [engine switches...]

Offered to the host as "system"; asked for, Windows opens its window. With nobody at
the machine to answer it the host cancels: what is checked is that Windows was offered
and asked, and the page told when it ended. The windows on the screen are printed.
"""

import ctypes
import ctypes.wintypes as w
import http.server
import json
import pathlib
import sys
import tempfile
import threading
import time

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from harness import Cdp, Engine, check, failures, finish, note, skip  # noqa: E402


class Fixture(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        body = b"<title>passkey</title><button>Authenticate</button>"
        self.send_response(200)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *args):
        pass


def windows():
    user32, titles = ctypes.WinDLL("user32"), []
    enum = ctypes.WINFUNCTYPE(w.BOOL, w.HWND, w.LPARAM)

    def one(hwnd, _):
        if user32.IsWindowVisible(hwnd):
            title, kind = ctypes.create_unicode_buffer(200), ctypes.create_unicode_buffer(100)
            user32.GetWindowTextW(hwnd, title, 200)
            user32.GetClassNameW(hwnd, kind, 100)
            titles.append((kind.value, title.value))
        return True

    user32.EnumWindows(enum(one), 0)
    return titles


# Chromium offers the route only with webauthn.dll's API at version 1 or later
# (device/fido/win/webauthn_api.cc). Below that nothing here can be tried.
version = ctypes.WinDLL("webauthn").WebAuthNGetApiVersionNumber()
note(f"webauthn.dll API version: {version}")
if version < 1:
    # Nothing checked: no summary line; a failure under ENGINE_TEST_STRICT=1
    # unless the expected skips name it.
    skip("Windows' WebAuthn route", "this machine's webauthn.dll offers no API")
    sys.exit(1 if failures else 0)

server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Fixture)
threading.Thread(target=server.serve_forever, daemon=True).start()
origin = f"http://localhost:{server.server_port}"


def test(engine):
    if not check(engine.url is not None, "the host serves CDP"):
        return
    cdp = Cdp(engine.url)
    target = next(t["targetId"] for t in cdp.call("Target.getTargets")["targetInfos"] if t["type"] == "page")
    session = cdp.call("Target.attachToTarget", {"targetId": target, "flatten": True})["sessionId"]

    def call(method, params=None):
        return cdp.call(method, params, session=session)

    def evaluate(script):
        result = call("Runtime.evaluate", {"expression": script, "returnByValue": True,
                                           "awaitPromise": True, "userGesture": True})
        assert not result.get("exceptionDetails"), result
        return result["result"].get("value")

    def state():
        return evaluate("zebkit.app.authenticationState()")

    def action(event, name):
        value = dict(request=event["request"], revision=event["revision"], action=name)
        return evaluate("zebkit.app.authenticationAction(" + json.dumps(value) + ")")

    call("Page.enable")
    call("Page.navigate", {"url": origin})
    assert cdp.event("Page.loadEventFired", timeout=15)
    note("isUserVerifyingPlatformAuthenticatorAvailable: %s"
         % evaluate("PublicKeyCredential.isUserVerifyingPlatformAuthenticatorAvailable()"))
    call("Input.dispatchMouseEvent", {"type": "mousePressed", "x": 20, "y": 10, "button": "left", "clickCount": 1})
    call("Input.dispatchMouseEvent", {"type": "mouseReleased", "x": 20, "y": 10, "button": "left", "clickCount": 1})
    before = set(windows())
    evaluate("""window.authResult='pending';
      navigator.credentials.get({publicKey:{challenge:new Uint8Array(32),rpId:'localhost',
      userVerification:'required',timeout:60000}}).then(()=>window.authResult='unexpected success',
      e=>window.authResult=e.name); true""")
    event = None
    deadline = time.monotonic() + 8
    while time.monotonic() < deadline and not event:
        event = state()
        time.sleep(.01)
    note("first state: %s %s" % ({k: event.get(k) for k in ("state", "actions")} if event else None,
                                 evaluate("window.authResult")))
    offered = bool(event) and "system" in event.get("actions", [])
    check(offered, "Windows' route is offered to the host")
    if offered:
        check(action(event, "system"), "the host asks for it")
        # Windows' own dialog opens, or the request ends by itself.
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline and not set(windows()) - before and \
                evaluate("window.authResult") == "pending":
            time.sleep(.02)
        note("windows it opened: %s" % sorted(set(windows()) - before))
        latest = state()
        note("then: %s %s" % ({k: latest.get(k) for k in ("state", "actions")}, evaluate("window.authResult")))
        if evaluate("window.authResult") == "pending":
            action(latest, "cancel")
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline and evaluate("window.authResult") == "pending":
            time.sleep(.01)
        check(evaluate("window.authResult") == "NotAllowedError", "the page is told when it ends",
              evaluate("window.authResult"))


with tempfile.TemporaryDirectory(prefix="zebkit-win-webauthn-") as directory:
    engine = Engine([sys.argv[1], *sys.argv[2:], "--headless", "--disable-gpu", "--test-host-ui",
                     f"--user-data-dir={directory}"], page=origin)
    try:
        test(engine)
    except Exception as error:  # noqa: BLE001
        check(False, "the test went on", repr(error))
    finally:
        engine.stop(bool(failures))
sys.exit(finish())
