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
sys.path.insert(0, HERE)

from harness import SETTLE, check, finish, pump, wait, zebkit  # noqa: E402


def main():
    with tempfile.TemporaryDirectory(prefix="zebkit-lock-") as directory:
        ready, asked = os.path.join(directory, "ready"), os.path.join(directory, "asked")
        saver = subprocess.Popen([sys.executable, os.path.join(HERE, "fake_screensaver.py"), ready, asked])
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
            check(wait(lambda: os.path.exists(asked), 10), "the engine's watcher reads the screen saver's state")
            pump(SETTLE)
            saver.send_signal(signal.SIGUSR1)
            wait(lambda: "lock" in events, 8)
            check("lock" in events, "the screen locked: a lock event", events)
            saver.send_signal(signal.SIGUSR2)
            wait(lambda: "unlock" in events, 8)
            check(events[-1:] == ["unlock"], "unlocked: an unlock event", events)
            zebkit.on_power_event(None)
            saver.send_signal(signal.SIGUSR1)
            count = len(events)
            pump(SETTLE)
            check(len(events) == count, "without a function nothing is told", events)
        except Exception as error:  # noqa: BLE001
            check(False, "the test ran to the end", repr(error))
        finally:
            zebkit.shutdown()
            saver.terminate()
    return finish()


if __name__ == "__main__":
    sys.exit(main())
