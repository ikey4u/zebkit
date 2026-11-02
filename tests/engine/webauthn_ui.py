"""Host-owned WebAuthn events/actions on an owned localhost page only.

The C fixture deliberately binds its host interface to this origin to allow
assertions via CDP. Production embedders must never grant such a bridge to
untrusted pages. Pairing data/PINs are not printed or persisted by this test.
"""
import http.server
import json
import pathlib
import os
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
        body = b"<title>Owned host authentication fixture</title><button>Authenticate</button>"
        self.send_response(200)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *args):
        pass


def main():
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Fixture)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    origin = f"http://localhost:{server.server_port}"
    with tempfile.TemporaryDirectory(prefix="zebkit-host-auth-") as directory:
        logpath = pathlib.Path(directory) / "engine.log"
        with logpath.open("w") as log:
            arguments = [*sys.argv[2:], "--headless", "--disable-gpu", "--test-host-ui",
                         f"--user-data-dir={directory}", "--remote-debugging-port=0", origin]
            app = os.environ.get("ZEBKIT_WEBAUTHN_TEST_APP")
            command = (["open", "-n", "-W", "--stdout", str(logpath), "--stderr", str(logpath),
                        app, "--args", *arguments] if app else [sys.argv[1], *arguments])
            process = subprocess.Popen(command, stdout=subprocess.DEVNULL, stderr=log)
            cdp = None
            try:
                deadline = time.monotonic() + 30
                while time.monotonic() < deadline and process.poll() is None:
                    match = re.search(r"ws://\S+", logpath.read_text())
                    if match:
                        cdp = Cdp(match[0])
                        break
                    time.sleep(.1)
                assert cdp is not None, "fixture did not start"
                target = next(t["targetId"] for t in cdp.call("Target.getTargets")["targetInfos"] if t["type"] == "page")
                session = cdp.call("Target.attachToTarget", {"targetId": target, "flatten": True})["sessionId"]

                def call(method, params=None):
                    return cdp.call(method, params, session=session)

                def evaluate(script):
                    result = call("Runtime.evaluate", {"expression": script, "returnByValue": True,
                        "awaitPromise": True, "userGesture": True})
                    assert not result.get("exceptionDetails"), "owned test script failed"
                    return result["result"].get("value")

                call("Page.enable")
                call("Page.navigate", {"url": origin})
                assert cdp.event("Page.loadEventFired", timeout=15)
                assert evaluate("typeof zebkit.app.authenticationState") == "function"

                def state():
                    return evaluate("zebkit.app.authenticationState()")

                def wait_state(request=None, closed=False):
                    deadline = time.monotonic() + 8
                    while time.monotonic() < deadline:
                        event = state()
                        if event and (request is None or event["request"] == request) and ((event["state"] == "closed") == closed):
                            return event
                        time.sleep(.05)
                    raise AssertionError("expected host state did not arrive")

                def start():
                    call("Input.dispatchMouseEvent", {"type": "mousePressed", "x": 20, "y": 10, "button": "left", "clickCount": 1})
                    call("Input.dispatchMouseEvent", {"type": "mouseReleased", "x": 20, "y": 10, "button": "left", "clickCount": 1})
                    evaluate("""window.authResult='pending';window.authAbort=new AbortController();
                      navigator.credentials.get({publicKey:{challenge:crypto.getRandomValues(new Uint8Array(32)),
                      rpId:'localhost',userVerification:'required',timeout:60000},signal:authAbort.signal})
                      .then(()=>window.authResult='unexpected success',e=>window.authResult=e.name); true""")
                    return wait_state()

                def action(event, name="cancel", **overrides):
                    value = dict(request=event["request"], revision=event["revision"], action=name)
                    value.update(overrides)
                    return evaluate("zebkit.app.authenticationAction(" + json.dumps(value) + ")")

                first = start()
                assert first["version"] == 1 and first["origin"] == origin and first["rp_id"] == "localhost"
                assert first["state"] in ("external", "choose", "unavailable")
                assert "cancel" in first["actions"]
                assert not action(first, request="18446744073709551615")
                assert not action(first, revision=0)
                assert not action(first, name="unrecognized")
                assert evaluate("zebkit.app.authenticationAction('malformed')") is False
                assert action(first)
                assert not action(first), "a duplicate action must be rejected"
                closed = wait_state(first["request"], closed=True)
                assert closed["outcome"] == "cancelled"
                assert evaluate("window.authResult") == "NotAllowedError"
                print("ok host receives origin-bound events; malformed, stale and duplicate actions reject; cancel completes", flush=True)

                second = start()
                assert second["request"] != first["request"]
                assert not action(first), "old request cannot act on new UI"
                evaluate("window.authAbort.abort(); true")
                wait_state(second["request"], closed=True)
                # The host may hear of the close before the page's promise
                # settles: the two go to different processes.
                deadline = time.monotonic() + 5
                while time.monotonic() < deadline and evaluate("window.authResult") == "pending":
                    time.sleep(.05)
                assert evaluate("window.authResult") == "AbortError"
                print("ok AbortSignal closes host state and old request tokens remain invalid", flush=True)

                third = start()
                call("Page.navigate", {"url": "about:blank"})
                assert cdp.event("Page.loadEventFired", timeout=15)
                call("Page.navigate", {"url": origin})
                assert cdp.event("Page.loadEventFired", timeout=15)
                wait_state(third["request"], closed=True)
                assert not action(third)
                print("ok navigation closes the host request and rejects later replies", flush=True)

                fourth = start()
                assert fourth["request"] != third["request"]
                assert evaluate("zebkit.app.authenticationDetach()") is True
                deadline = time.monotonic() + 5
                while time.monotonic() < deadline and evaluate("window.authResult") == "pending":
                    time.sleep(.05)
                assert evaluate("window.authResult") == "NotAllowedError"
                assert not action(fourth)
                print("ok removing the callback cancels active authentication and invalidates its operations", flush=True)
                print("all host authentication checks passed", flush=True)
            except Exception:
                print(logpath.read_text()[-5000:], file=sys.stderr)
                raise
            finally:
                if cdp:
                    try:
                        cdp.call("Target.closeTarget", {"targetId": target})
                    except (OSError, EOFError):
                        pass
                    cdp.sock.close()
                process.terminate()
                try:
                    process.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
    server.shutdown()
    server.server_close()


if __name__ == "__main__":
    main()
