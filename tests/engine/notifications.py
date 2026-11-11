"""Web notifications (docs/SYSTEM_INTEGRATION.md, 12): denied by default; once
granted, shown by the application's function or, without one, by the
desktop's service (a fake org.freedesktop.Notifications on a private bus,
fake_notifications.py); a click, from either, reaches the page's onclick.

    dbus-run-session -- python3 tests/engine/notifications.py [engine switches...]

Exit 0 when every check passes.
"""

import json
import os
import signal
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

from harness import Server, check, ev, finish, wait, zebkit  # noqa: E402


PAGE = (b"<title>Notes</title><script>window.clicks = 0; window.shown = 0;"
        b"window.note = (title) => { const n = new Notification(title, {body: 'from the page'});"
        b" n.onclick = () => clicks++; n.onshow = () => shown++; return 1; };</script>")


def read(log):
    try:
        with open(log) as lines:
            return [json.loads(line) for line in lines if line.strip()]
    except FileNotFoundError:
        return []


def main():
    server = Server({"/": PAGE})
    url = server.base + "/"
    with tempfile.TemporaryDirectory(prefix="zebkit-notifications-") as directory:
        log, ready = os.path.join(directory, "log"), os.path.join(directory, "ready")
        daemon = subprocess.Popen([sys.executable, os.path.join(HERE, "fake_notifications.py"), log, ready])
        end = time.monotonic() + 10
        while not os.path.exists(ready) and time.monotonic() < end:
            time.sleep(.05)
        if zebkit.init(["notifications"] + sys.argv[1:]) != 0:
            print("FAIL zebkit.init()")
            return 1
        try:
            view = zebkit.View(url)
            wait(lambda: ev(view, "location.href + document.readyState") == url + "complete")
            check(ev(view, "Notification.permission") == "default", "not decided: Notification.permission default")
            asked = ev(view, "Notification.requestPermission()")
            check(asked == "denied", "and denied without the application's decision", asked)

            view.set_permission("notifications", "granted", "*")
            check(ev(view, "Notification.permission") == "granted", "granted by the application")
            ev(view, "note('to the desktop')")
            wait(lambda: read(log), 5)
            entries = read(log)
            check(entries and entries[-1]["summary"] == "to the desktop" and entries[-1]["body"] == "from the page",
                  "without a function, the desktop's notification service shows it", entries[-1:] or None)
            wait(lambda: ev(view, "shown") == 1, 5)
            check(ev(view, "shown") == 1, "and the page is told it was shown")
            daemon.send_signal(signal.SIGUSR1)
            wait(lambda: ev(view, "clicks") == 1, 5)
            check(ev(view, "clicks") == 1, "a click on the desktop's reaches the page's onclick")

            events = []
            zebkit.on_notification(events.append)
            ev(view, "note('to the application')")
            wait(lambda: events, 5)
            check(events and events[0]["event"] == "show" and events[0]["title"] == "to the application"
                  and events[0]["origin"] == url, "with a function, the application shows it", events[:1] or None)
            check(len(read(log)) == len(entries), "and the desktop shows nothing")
            if events:
                zebkit.notification_clicked(events[0]["id"])
                wait(lambda: ev(view, "clicks") == 2, 5)
                check(ev(view, "clicks") == 2, "the application's click reaches the page's onclick")
            zebkit.on_notification(None)
            view.close()
        except Exception as error:  # noqa: BLE001
            check(False, "the test ran to the end", repr(error))
        finally:
            zebkit.shutdown()
            daemon.terminate()
            server.close()
    return finish()


if __name__ == "__main__":
    sys.exit(main())
