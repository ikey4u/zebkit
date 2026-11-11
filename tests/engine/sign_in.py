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
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from harness import (Server, check, finish, pump, wait, x11_focused, x11_new_window, x11_windows,  # noqa: E402
                     xdotool, zebkit)

WANTED = "Basic " + base64.b64encode(b"alice:secret").decode()


challenged = []


class Guarded(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        if self.headers.get("Authorization") == WANTED:
            body = b"<title>Signed in</title>welcome alice"
            self.send_response(200)
        else:
            body = b"<title>Denied</title>who are you"
            self.send_response(401)
            self.send_header("WWW-Authenticate", 'Basic realm="zebkit test"')
            challenged.append(self.path)
        self.send_header("Content-Type", "text/html")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *args):
        pass


def main():
    server = Server(Guarded)
    url = server.base + "/"
    if zebkit.init(["sign-in"] + sys.argv[1:]) != 0:
        print("FAIL zebkit.init()")
        return 1
    try:
        windowed = not zebkit.is_headless() and shutil.which("xdotool") and os.environ.get("DISPLAY") and \
            not os.environ.get("WAYLAND_DISPLAY") and sys.platform.startswith("linux")
        before = x11_windows() if windowed else set()
        view = zebkit.View(url, 500, 400)
        if windowed:
            window = x11_new_window(before)
            view.focus()
            # The dialog is drawn in the view's window once the challenge came:
            # the keys only then, and into that window with the focus.
            up = window and x11_focused(window) and wait(lambda: challenged, 10)
            check(bool(up), "the challenge came, to the view's window with the focus", (window, challenged))
            if up:
                # Nothing tells when the dialog has the keyboard: as long as
                # before the challenge was waited for (2 s), from the challenge.
                pump(2)
                for args in (["type", "--delay", "40", "alice"], ["key", "Tab"], ["type", "--delay", "40", "secret"],
                             ["key", "Return"]):
                    xdotool(*args, seconds=15)
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
        server.close()
    return finish()


if __name__ == "__main__":
    sys.exit(main())
