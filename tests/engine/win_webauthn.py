"""WebAuthn's system interface on Windows: webauthn.dll's window, through the host's
own events. Usage: win_webauthn.py [engine switches...]; ZEBKIT_DIR is the engine.

The machine has nobody to answer Windows' window, so the host cancels it after
ZEBKIT_TEST_HOLD seconds (for a picture of the screen from outside): what is checked
is that Windows was offered and asked, and the page told when it ended.
"""

import http.server
import json
import os
import sys
import threading
import time

import zebkit


class Page(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        body = b"<!doctype html><title>passkey</title>"
        self.send_response(200)
        self.send_header("Content-Type", "text/html")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *args):
        pass


server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Page)
threading.Thread(target=server.serve_forever, daemon=True).start()
url = f"http://127.0.0.1:{server.server_port}/"
hold = float(os.environ.get("ZEBKIT_TEST_HOLD", "8"))

CREATE = """navigator.credentials.create({publicKey: {
  challenge: new Uint8Array(32), rp: {name: 'zebkit', id: '127.0.0.1'},
  user: {id: new Uint8Array(8), name: 'u', displayName: 'u'},
  pubKeyCredParams: [{type: 'public-key', alg: -7}], timeout: 60000}})
  .then(() => 'created', e => e.name)"""

failures = []
seen = []
state = {"chosen": False}


def check(ok, what, detail=""):
    print(("ok   " if ok else "FAIL ") + what + (f": {detail}" if detail != "" else ""), flush=True)
    if not ok:
        failures.append(what)


def ready():
    view = zebkit.View(url, 640, 480)

    def on_event(text):
        event = json.loads(text)
        seen.append(event)
        print("event", {k: event.get(k) for k in ("state", "actions", "title")}, flush=True)
        if "system" in event.get("actions", []) and not state["chosen"]:
            state["chosen"] = True
            answer = {"request": event["request"], "revision": event["revision"]}
            view.authentication_action({**answer, "action": "system"})
            print(f"asked Windows; holding {hold} seconds", flush=True)
            def cancel():
                # The revision the state "waiting" has by then.
                latest = seen[-1]
                print("cancelling:", view.authentication_action(
                    {"request": latest["request"], "revision": latest["revision"], "action": "cancel"}), flush=True)

            zebkit.post(cancel, int(hold * 1000))

    view.on_authentication(on_event)

    def finished(answer, error=None):
        print("the page's promise:", answer, error, flush=True)
        check(state["chosen"], "Windows' route was offered to the host")
        check(answer == '"NotAllowedError"', "the page is told when it ends", answer)
        view.close()
        zebkit.quit()

    def uvpaa(answer, error=None):
        print("isUserVerifyingPlatformAuthenticatorAvailable:", answer, flush=True)
        view.evaluate(CREATE, finished)

    def loaded(event, _url):
        if event == zebkit.LOAD_FINISHED:
            view.evaluate("PublicKeyCredential.isUserVerifyingPlatformAuthenticatorAvailable()", uvpaa)

    view.on_load(loaded)


zebkit.main(ready, sys.argv)
print(f"{'FAIL' if failures else 'ok'} {len(failures)} failures", flush=True)
sys.exit(1 if failures else 0)
