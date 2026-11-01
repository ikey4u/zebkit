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
import re
import subprocess
import sys
import tempfile
import threading
import time

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from cdp_page import Cdp


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
print("webauthn.dll API version:", version, flush=True)
if version < 1:
    print("skip this machine's webauthn.dll offers no API; Windows' route cannot be tried here", flush=True)
    sys.exit(0)

server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Fixture)
threading.Thread(target=server.serve_forever, daemon=True).start()
origin = f"http://localhost:{server.server_port}"
failures = 0


def check(ok, what, detail=""):
    global failures
    failures += not ok
    print(("ok   " if ok else "FAIL ") + what + (f": {detail}" if detail != "" else ""), flush=True)


with tempfile.TemporaryDirectory(prefix="zebkit-win-webauthn-") as directory:
    logpath = pathlib.Path(directory) / "engine.log"
    with logpath.open("w") as log:
        process = subprocess.Popen([sys.argv[1], *sys.argv[2:], "--headless", "--disable-gpu", "--test-host-ui",
                                    f"--user-data-dir={directory}", "--remote-debugging-port=0", origin],
                                   stdout=subprocess.DEVNULL, stderr=log)
        try:
            cdp = None
            deadline = time.monotonic() + 30
            while time.monotonic() < deadline and process.poll() is None and not cdp:
                match = re.search(r"ws://\S+", logpath.read_text())
                if match:
                    cdp = Cdp(match[0])
                time.sleep(.1)
            assert cdp, "the host did not start"
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
            print("isUserVerifyingPlatformAuthenticatorAvailable:",
                  evaluate("PublicKeyCredential.isUserVerifyingPlatformAuthenticatorAvailable()"), flush=True)
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
                time.sleep(.05)
            print("first state:", {k: event.get(k) for k in ("state", "actions")} if event else None,
                  evaluate("window.authResult"), flush=True)
            offered = bool(event) and "system" in event.get("actions", [])
            check(offered, "Windows' route is offered to the host")
            if offered:
                check(action(event, "system"), "the host asks for it")
                time.sleep(6)
                print("windows it opened:", sorted(set(windows()) - before), flush=True)
                latest = state()
                print("then:", {k: latest.get(k) for k in ("state", "actions")}, evaluate("window.authResult"), flush=True)
                if evaluate("window.authResult") == "pending":
                    action(latest, "cancel")
                time.sleep(2)
                check(evaluate("window.authResult") == "NotAllowedError", "the page is told when it ends",
                      evaluate("window.authResult"))
        finally:
            if process.poll() is None:
                process.terminate()
            log.flush()
print(f"{failures} failures", flush=True)
sys.exit(1 if failures else 0)
