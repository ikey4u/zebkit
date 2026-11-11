"""A page's prompt() and confirm() with no function of the application's
(docs/SYSTEM_INTEGRATION.md, 9): Chromium's views dialog where there is a
window (Linux; Windows' prompt()), answered by real keys; without one,
suppressed as before, so that nothing waits. And EyeDropper.open(), the
application's to answer (zebkit_view_on_eye_dropper).

    python3 tests/engine/js_dialogs.py [engine switches...]

Exit 0 when every check passes.
"""

import os
import shutil
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from harness import (Server, activated, check, ev, finish, held, wait, x11_focused, x11_window,  # noqa: E402
                     xdotool, zebkit)


def keys(*args):
    xdotool(*args, seconds=15)


def dialog_up(view, window, what):
    """The view's window has the focus and the page is held by its dialog:
    only then are keys typed (the views dialog is drawn in the window, no
    X11 window of its own)."""
    view.focus()
    return check(x11_focused(window) and held(view), "%s: its dialog is up, in the focused window" % what)


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
        window = x11_window("Dialogs") if windowed else None
        if windowed:
            check(window is not None, "the view's X11 window")
        # Asked from a timer, so that the evaluation itself is not held.
        ev(view, "setTimeout(() => answers.push(prompt('Your name?', '')), 0); 1")
        if windowed and window and dialog_up(view, window, "prompt()"):
            keys("type", "--delay", "60", "zk")
            keys("key", "Return")
            wait(lambda: ev(view, "answers.length") == 1, 5)
            check(ev(view, "answers[0]") == "zk", "prompt(): the views dialog, typed into and accepted",
                  ev(view, "JSON.stringify(answers)"))
            ev(view, "setTimeout(() => answers.push(confirm('Sure?')), 0); 1")
            if dialog_up(view, window, "confirm()"):
                keys("key", "Escape")
                wait(lambda: ev(view, "answers.length") == 2, 5)
                check(ev(view, "answers[1]") is False, "confirm(): cancelled with Escape",
                      ev(view, "JSON.stringify(answers)"))
        elif not windowed:
            wait(lambda: ev(view, "answers.length") == 1, 5)
            check(ev(view, "answers.length") == 1 and ev(view, "answers[0]") is None,
                  "without a window prompt() is suppressed, and nothing waits", ev(view, "JSON.stringify(answers)"))
        # EyeDropper.open(): cancelled without a function, the application's colour with one;
        # a secure context's (http://127.0.0.1).
        server = Server({"/": b"<title>Picker</title>"})
        secure = server.base + "/"
        view.load(secure)
        wait(lambda: ev(view, "location.href + document.readyState") == secure + "complete")
        # EyeDropper.open() wants its document focused.
        view.focus()
        wait(lambda: ev(view, "document.hasFocus()") is True, 3)
        # After the evaluation's user activation has reached the browser
        # (device_chooser.py): EyeDropper.open() consumes it there.
        pick = activated("new EyeDropper().open()") + ".then(r => r.sRGBHex, e => e.name)"
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
        # navigator.share(): cancelled without a function on Linux, the application's with one.
        share = activated("navigator.share({title: 'A page', url: location.href})") + ".then(() => 'shared', e => e.name)"
        if sys.platform.startswith("linux"):
            alone = ev(view, share)
            check(alone == "AbortError", "navigator.share() without a function is cancelled (Linux has no sheet)", alone)
        shares = []

        def shared(request):
            shares.append(request.metadata)
            request.respond(True)
        view.on_share(shared)
        answered = ev(view, share)
        check(answered == "shared" and shares and shares[0]["title"] == "A page" and shares[0]["url"] == secure,
              "with one, the application shares it", (answered, shares[:1]))
        view.close()
        server.close()
    except Exception as error:  # noqa: BLE001
        check(False, "the test ran to the end", repr(error))
    finally:
        zebkit.shutdown()
    return finish()


if __name__ == "__main__":
    sys.exit(main())
