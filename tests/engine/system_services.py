"""The system's own services for the application (docs/SYSTEM_INTEGRATION.md,
13 and 14): the screens, keeping the system awake, and opening a URL with the
system's handler (on Linux an xdg-open of the test's on PATH that records its
arguments: one argument, no shell) with what must not be opened refused.

    python3 tests/engine/system_services.py [engine switches...]

Exit 0 when every check passes.
"""

import os
import stat
import sys
import tempfile
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


def refused(function):
    try:
        function()
    except zebkit.Error as error:
        return str(error)
    return None


def main():
    with tempfile.TemporaryDirectory(prefix="zebkit-services-") as directory:
        record = os.path.join(directory, "opened")
        if sys.platform.startswith("linux"):
            opener = os.path.join(directory, "xdg-open")
            with open(opener, "w") as script:
                script.write('#!/bin/sh\nfor a in "$@"; do printf "%%s\\n" "$a"; done > "%s"\n' % record)
            os.chmod(opener, stat.S_IRWXU)
            os.environ["PATH"] = directory + os.pathsep + os.environ.get("PATH", "")
        if zebkit.init(["system-services"] + sys.argv[1:]) != 0:
            print("FAIL zebkit.init()")
            return 1
        try:
            screens = zebkit.displays()
            check(screens and any(s["primary"] for s in screens) and screens[0]["bounds"]["width"] > 0,
                  "the screens, with the primary one", screens)
            token = zebkit.prevent_sleep("a test that must not sleep")
            check(token > 0, "keeping the system awake gives a token", token)
            check(zebkit.allow_sleep(token) and not zebkit.allow_sleep(token), "released once, not twice")
            zebkit.on_power_event(lambda event: None)
            zebkit.on_power_event(None)
            for bad in ("javascript:alert(1)", "data:text/html,x", "not a url"):
                check(refused(lambda: zebkit.open_external(bad)) is not None, "refused: %s" % bad)
            check(refused(lambda: zebkit.show_in_folder("relative/path")) is not None, "a relative path is refused")
            if sys.platform.startswith("linux"):
                url = "https://example.com/a b?c=d;rm -rf /"
                zebkit.open_external(url)
                wait(lambda: os.path.exists(record), 5)
                lines = open(record).read().splitlines() if os.path.exists(record) else []
                # Canonical, as a browser hands a URL on: one argument, nothing a shell would split.
                check(lines == ["https://example.com/a%20b?c=d;rm%20-rf%20/"],
                      "the system's handler gets the URL as one argument, no shell", lines)
        except Exception as error:  # noqa: BLE001
            check(False, "the test ran to the end", repr(error))
        finally:
            zebkit.shutdown()
    print("%d of %d checks passed" % (checks - len(failures), checks), flush=True)
    return 1 if failures or not checks else 0


if __name__ == "__main__":
    sys.exit(main())
