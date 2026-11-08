"""Pages following the system's appearance (docs/SYSTEM_INTEGRATION.md, 11), on
Linux through the XDG portal: a fake one on a private session bus
(fake_portal.py), dark at first, flipped to light by SIGUSR1.

    dbus-run-session -- python3 tests/engine/appearance.py follow|fixed [engine switches...]

follow: the startup setting appearance.follow; pages and zebkit.environment()
take the system's scheme and its change, the change is told, and a view's own
colorScheme still wins. fixed: without it, pages stay light, as before.
Exit 0 when every check passes.
"""

import os
import signal
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "bindings", "python"))

import zebkit  # noqa: E402

failures = []
checks = 0
DARK = "matchMedia('(prefers-color-scheme: dark)').matches"


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
    view.evaluate(script, lambda value, error=None: box.append((value, error)))
    wait(lambda: box)
    return box[0][0] if box and box[0][1] is None else None


def main():
    mode = sys.argv[1]
    with tempfile.TemporaryDirectory(prefix="zebkit-appearance-") as directory:
        ready = os.path.join(directory, "ready")
        portal = subprocess.Popen([sys.executable, os.path.join(HERE, "fake_portal.py"), "1", ready])
        end = time.monotonic() + 10
        while not os.path.exists(ready) and time.monotonic() < end:
            time.sleep(.05)
        settings = {"appearance": {"follow": True}} if mode == "follow" else {}
        if zebkit.init(["appearance"] + sys.argv[2:], settings=settings) != 0:
            print("FAIL zebkit.init()")
            return 1
        try:
            view = zebkit.View("data:text/html,<title>appearance</title>")
            wait(lambda: ev(view, "document.readyState") == "complete")
            if mode == "follow":
                wait(lambda: ev(view, DARK) is True)
                check(ev(view, DARK) is True, "the system's dark scheme is the page's")
                environment = zebkit.environment()
                check(environment["colorScheme"] == "dark" and environment["following"],
                      "zebkit.environment() says dark, following", environment)
                told = []
                zebkit.on_environment_changed(told.append)
                portal.send_signal(signal.SIGUSR1)
                wait(lambda: ev(view, DARK) is False)
                check(ev(view, DARK) is False, "the system's change to light is the page's at once")
                wait(lambda: told and told[-1]["colorScheme"] == "light", 5)
                check(told and told[-1]["colorScheme"] == "light", "and is told", told[-1:] or None)
                view.set_settings({"colorScheme": "dark"})
                wait(lambda: ev(view, DARK) is True, 5)
                check(ev(view, DARK) is True, "a view's own colorScheme wins over the system's")
                zebkit.on_environment_changed(None)
            else:
                time.sleep(1)
                wait(lambda: False, 1)
                check(ev(view, DARK) is False, "without appearance.follow the page stays light, as before")
                check(zebkit.environment()["following"] is False, "and the environment says it does not follow")
            view.close()
        except Exception as error:  # noqa: BLE001
            check(False, "the test ran to the end", repr(error))
        finally:
            zebkit.shutdown()
            portal.terminate()
    print("%d of %d checks passed" % (checks - len(failures), checks), flush=True)
    return 1 if failures or not checks else 0


if __name__ == "__main__":
    sys.exit(main())
