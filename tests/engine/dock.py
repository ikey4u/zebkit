"""The application's icon in the Dock or on the taskbar (docs/SYSTEM_INTEGRATION.md,
12): a badge, a progress and a request for attention, taken on every system; on
Linux sent as com.canonical.Unity.LauncherEntry's Update signal, which a
dbus-monitor on the (private) session bus sees; the Dock's menu, macOS's only.

    [dbus-run-session --] python3 tests/engine/dock.py [engine switches...]

Exit 0 when every check passes.
"""

import os
import shutil
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from harness import check, finish, wait, zebkit  # noqa: E402


def main():
    linux = sys.platform.startswith("linux")
    bus = linux and os.environ.get("DBUS_SESSION_BUS_ADDRESS") and shutil.which("dbus-monitor")
    with tempfile.TemporaryDirectory(prefix="zebkit-dock-") as directory:
        log = os.path.join(directory, "signals")
        monitor = None
        if bus:
            with open(log, "w") as out:
                monitor = subprocess.Popen(["dbus-monitor", "--session",
                                            "type='signal',interface='com.canonical.Unity.LauncherEntry'"],
                                           stdout=out, stderr=subprocess.DEVNULL)
            time.sleep(.5)
        if zebkit.init(["dock"] + sys.argv[1:]) != 0:
            print("FAIL zebkit.init()")
            return 1
        try:
            view = zebkit.View("data:text/html,<title>Dock</title>", 300, 200)
            wait(lambda: view.title == "Dock", 5)
            check(zebkit.app_set_badge("3"), "a badge is taken")
            check(zebkit.app_set_progress(.5), "a progress is taken")
            check(not zebkit.app_set_progress(2), "a progress past 1 is refused")
            zebkit.app_request_attention()
            check(zebkit.app_set_progress(None) and zebkit.app_set_badge(None), "both are cleared")
            chosen = []
            try:
                zebkit.app_set_dock_menu([{"title": "New window", "id": "new"}], chosen.append)
                refused = None
            except zebkit.Error as error:
                refused = str(error)
            if sys.platform == "darwin":
                check(refused is None, "the Dock's menu is taken (macOS)", refused)
            else:
                check(refused is not None and "unsupported" in refused, "the Dock's menu is macOS's", refused)
            if monitor:
                def seen():
                    with open(log) as f:
                        return f.read()
                wait(lambda: seen().count("member=Update") >= 4, 5)
                text = seen()
                check("member=Update" in text and ".desktop" in text, "the launcher's Update signal is sent",
                      text[:300])
                check('"count"' in text and "int64 3" in text, "with the badge's count")
                check('"progress"' in text and "double 0.5" in text, "and the progress")
                check('"urgent"' in text and "boolean true" in text, "the request for attention is urgent")
            view.close()
        except Exception as error:  # noqa: BLE001
            check(False, "the test ran to the end", repr(error))
        finally:
            zebkit.shutdown()
            if monitor:
                monitor.terminate()
    return finish()


if __name__ == "__main__":
    sys.exit(main())
