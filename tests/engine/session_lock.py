"""The session's lock as power events (docs/SYSTEM_INTEGRATION.md, 14): on
Linux the desktop's screen saver (a fake org.freedesktop.ScreenSaver on a
private bus, fake_screensaver.py) locking and unlocking reaches
zebkit_on_power_event as {"event": "lock"} and {"event": "unlock"}.

    dbus-run-session -- python3 tests/engine/session_lock.py [engine switches...]

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
        time.sleep(.01)
    return False


def main():
    with tempfile.TemporaryDirectory(prefix="zebkit-lock-") as directory:
        ready = os.path.join(directory, "ready")
        saver = subprocess.Popen([sys.executable, os.path.join(HERE, "fake_screensaver.py"), ready])
        end = time.monotonic() + 10
        while not os.path.exists(ready) and time.monotonic() < end:
            time.sleep(.05)
        if zebkit.init(["session-lock"] + sys.argv[1:]) != 0:
            print("FAIL zebkit.init()")
            return 1
        try:
            events = []
            zebkit.on_power_event(lambda event: events.append(event["event"]))
            # The engine's watcher reads the state first, then follows the signal.
            wait(lambda: False, 2)
            saver.send_signal(signal.SIGUSR1)
            wait(lambda: "lock" in events, 8)
            check("lock" in events, "the screen locked: a lock event", events)
            saver.send_signal(signal.SIGUSR2)
            wait(lambda: "unlock" in events, 8)
            check(events[-1:] == ["unlock"], "unlocked: an unlock event", events)
            zebkit.on_power_event(None)
            saver.send_signal(signal.SIGUSR1)
            count = len(events)
            wait(lambda: False, 2.5)
            check(len(events) == count, "without a function nothing is told", events)
        except Exception as error:  # noqa: BLE001
            check(False, "the test ran to the end", repr(error))
        finally:
            zebkit.shutdown()
            saver.terminate()
    print("%d of %d checks passed" % (checks - len(failures), checks), flush=True)
    return 1 if failures or not checks else 0


if __name__ == "__main__":
    sys.exit(main())
