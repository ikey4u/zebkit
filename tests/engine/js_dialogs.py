"""A page's prompt() and confirm() with no function of the application's
(docs/SYSTEM_INTEGRATION.md, 9): Chromium's views dialog where there is a
window (Linux; Windows' prompt()), answered by real keys; without one,
suppressed as before, so that nothing waits. And EyeDropper.open(), the
application's to answer (zebkit_view_on_eye_dropper).

    python3 tests/engine/js_dialogs.py [engine switches...]

Exit 0 when every check passes.
"""

import http.server
import os
import shutil
import threading
import subprocess
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "bindings", "python"))

import zebkit  # noqa: E402

failures = []
checks = 0


def check(ok, what, detail=None):
    global checks
    checks += 1
    print(("ok   " if ok else "FAIL ") + what + (": %r" % (detail,) if detail is not None else ""), flush=True)
    if not ok:
        failures.append(what)


def wait(condition, seconds=10):
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        zebkit.pump()
        if condition():
            return True
        time.sleep(.005)
    return False


def ev(view, script):
    box = []
    view.evaluate(script, lambda value, error=None: box.append(value))
    wait(lambda: box)
    return box[0] if box else None


def keys(*args):
    subprocess.run(["xdotool", *args], check=False)


def main():
    if zebkit.init(["js-dialogs"] + sys.argv[1:]) != 0:
        print("FAIL zebkit.init()")
        return 1
    try:
        view = zebkit.View("data:text/html,<title>Dialogs</title>", 500, 300)
        wait(lambda: ev(view, "document.readyState") == "complete")
        ev(view, "window.answers = []; 1")
        windowed = not zebkit.is_headless() and shutil.which("xdotool") and os.environ.get("DISPLAY") and \
            not os.environ.get("WAYLAND_DISPLAY") and sys.platform.startswith("linux")
        # Asked from a timer, so that the evaluation itself is not held.
        ev(view, "setTimeout(() => answers.push(prompt('Your name?', '')), 0); 1")
        if windowed:
            view.focus()
            wait(lambda: False, 1.5)
            keys("type", "--delay", "60", "zk")
            keys("key", "Return")
            wait(lambda: ev(view, "answers.length") == 1, 5)
            check(ev(view, "answers[0]") == "zk", "prompt(): the views dialog, typed into and accepted",
                  ev(view, "JSON.stringify(answers)"))
            ev(view, "setTimeout(() => answers.push(confirm('Sure?')), 0); 1")
            wait(lambda: False, 1.5)
            keys("key", "Escape")
            wait(lambda: ev(view, "answers.length") == 2, 5)
            check(ev(view, "answers[1]") is False, "confirm(): cancelled with Escape", ev(view, "JSON.stringify(answers)"))
        else:
            wait(lambda: ev(view, "answers.length") == 1, 5)
            check(ev(view, "answers.length") == 1 and ev(view, "answers[0]") is None,
                  "without a window prompt() is suppressed, and nothing waits", ev(view, "JSON.stringify(answers)"))
        # EyeDropper.open(): cancelled without a function, the application's colour with one;
        # a secure context's (http://127.0.0.1).
        class Page(http.server.BaseHTTPRequestHandler):
            def do_GET(self):
                body = b"<title>Picker</title>"
                self.send_response(200)
                self.send_header("Content-Type", "text/html")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)

            def log_message(self, *args):
                pass
        server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Page)
        threading.Thread(target=server.serve_forever, daemon=True).start()
        secure = "http://127.0.0.1:%d/" % server.server_address[1]
        view.load(secure)
        wait(lambda: ev(view, "location.href + document.readyState") == secure + "complete")
        # EyeDropper.open() wants its document focused.
        view.focus()
        wait(lambda: ev(view, "document.hasFocus()") is True, 3)
        pick = "new EyeDropper().open().then(r => r.sRGBHex, e => e.name)"
        cancelled = ev(view, pick)
        check(cancelled == "AbortError", "EyeDropper.open() without a function is cancelled", cancelled)
        asked = []

        def dropper(request):
            asked.append(request.metadata)
            request.respond("#336699")
        view.on_eye_dropper(dropper)
        picked = ev(view, pick)
        check(picked == "#336699" and asked and asked[0]["origin"] == secure.rstrip("/"),
              "with one, the page gets the application's colour", (picked, asked[:1]))
        view.close()
    except Exception as error:  # noqa: BLE001
        check(False, "the test ran to the end", repr(error))
    finally:
        zebkit.shutdown()
    print("%d of %d checks passed" % (checks - len(failures), checks), flush=True)
    return 1 if failures or not checks else 0


if __name__ == "__main__":
    sys.exit(main())
