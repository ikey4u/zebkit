"""HTTP authentication with no function of the application's
(docs/SYSTEM_INTEGRATION.md, 9): the engine's sign-in dialog where there is a
window, answered by real keys; without one, cancelled as before.

    python3 tests/engine/sign_in.py [engine switches...]

Exit 0 when every check passes.
"""

import base64
import http.server
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
WANTED = "Basic " + base64.b64encode(b"alice:secret").decode()


def check(ok, what, detail=None):
    global checks
    checks += 1
    print(("ok   " if ok else "FAIL ") + what + (": %r" % (detail,) if detail is not None else ""), flush=True)
    if not ok:
        failures.append(what)


class Guarded(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        if self.headers.get("Authorization") == WANTED:
            body = b"<title>Signed in</title>welcome alice"
            self.send_response(200)
        else:
            body = b"<title>Denied</title>who are you"
            self.send_response(401)
            self.send_header("WWW-Authenticate", 'Basic realm="zebkit test"')
        self.send_header("Content-Type", "text/html")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *args):
        pass


def wait(condition, seconds=10):
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        zebkit.pump()
        if condition():
            return True
        time.sleep(.005)
    return False


def main():
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Guarded)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    url = "http://127.0.0.1:%d/" % server.server_address[1]
    if zebkit.init(["sign-in"] + sys.argv[1:]) != 0:
        print("FAIL zebkit.init()")
        return 1
    try:
        windowed = not zebkit.is_headless() and shutil.which("xdotool") and os.environ.get("DISPLAY") and \
            not os.environ.get("WAYLAND_DISPLAY") and sys.platform.startswith("linux")
        view = zebkit.View(url, 500, 400)
        if windowed:
            view.focus()
            wait(lambda: False, 2)
            for args in (["type", "--delay", "40", "alice"], ["key", "Tab"], ["type", "--delay", "40", "secret"],
                         ["key", "Return"]):
                subprocess.run(["xdotool", *args], check=False)
            wait(lambda: view.title == "Signed in", 10)
            check(view.title == "Signed in", "the sign-in dialog, typed into, signs the page in", view.title)
        else:
            wait(lambda: view.title == "Denied", 10)
            check(view.title == "Denied", "without a window the challenge is cancelled, as before", view.title)
        view.close()
    except Exception as error:  # noqa: BLE001
        check(False, "the test ran to the end", repr(error))
    finally:
        zebkit.shutdown()
        server.shutdown()
    print("%d of %d checks passed" % (checks - len(failures), checks), flush=True)
    return 1 if failures or not checks else 0


if __name__ == "__main__":
    sys.exit(main())
