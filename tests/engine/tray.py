"""The application's tray icon (docs/SYSTEM_INTEGRATION.md, 12), on Linux as a
StatusNotifierItem: registered with the desktop's watcher (a fake one on a
private bus, fake_tray_watcher.py), its tooltip, a click (Activate) to the
application's function, and its menu over com.canonical.dbusmenu with an
item chosen there.

    dbus-run-session -- python3 tests/engine/tray.py [engine switches...]

Exit 0 when every check passes.
"""

import os
import re
import struct
import subprocess
import sys
import tempfile
import time
import zlib

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


def png(size=22):
    """A small square of colour, as PNG bytes."""
    rows = b"".join(b"\x00" + bytes([40, 120, 200, 255]) * size for _ in range(size))
    chunk = lambda kind, data: struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(rows)) + chunk(b"IEND", b""))


def wait(condition, seconds=10):
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        zebkit.pump()
        if condition():
            return True
        time.sleep(.005)
    return False


def gdbus(*args):
    """A call to the bus while the engine keeps pumping: its output."""
    child = subprocess.Popen(["gdbus", "call", "--session", *args], stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                             text=True)
    wait(lambda: child.poll() is not None, 15)
    out, err = child.communicate() if child.poll() is not None else ("", "timed out")
    return out + err


def main():
    with tempfile.TemporaryDirectory(prefix="zebkit-tray-") as directory:
        log, ready = os.path.join(directory, "log"), os.path.join(directory, "ready")
        watcher = subprocess.Popen([sys.executable, os.path.join(HERE, "fake_tray_watcher.py"), log, ready])
        end = time.monotonic() + 10
        while not os.path.exists(ready) and time.monotonic() < end:
            time.sleep(.05)
        if zebkit.init(["tray"] + sys.argv[1:]) != 0:
            print("FAIL zebkit.init()")
            return 1
        try:
            message = None
            try:
                zebkit.Tray(b"not a png", "nothing")
            except zebkit.Error as error:
                message = str(error)
            check(message is not None, "an icon that is not a PNG is refused", message)
            clicks, chosen = [], []
            tray = zebkit.Tray(png(), "Zebkit tray")
            tray.on_click(lambda: clicks.append(1))
            tray.set_menu([{"title": "Open", "id": "open"}, {"separator": True},
                           {"title": "Quiet", "id": "quiet", "enabled": False}], chosen.append)
            wait(lambda: os.path.exists(log) and open(log).read().strip(), 10)
            service = open(log).read().split()[0] if os.path.exists(log) and open(log).read().strip() else None
            check(service is not None, "the item registers with the desktop's watcher", service)
            if service:
                tip = gdbus("--dest", service, "--object-path", "/StatusNotifierItem", "--method",
                            "org.freedesktop.DBus.Properties.Get", "org.kde.StatusNotifierItem", "ToolTip")
                check("Zebkit tray" in tip, "its tooltip", tip.strip())
                gdbus("--dest", service, "--object-path", "/StatusNotifierItem", "--method",
                      "org.kde.StatusNotifierItem.Activate", "0", "0")
                wait(lambda: clicks, 5)
                check(clicks == [1], "a click on it is the application's", clicks)
                layout = gdbus("--dest", service, "--object-path", "/org/chromium/DbusMenu", "--method",
                               "com.canonical.dbusmenu.GetLayout", "--", "0", "-1", "@as []")
                found = re.search(r"\((\d+), \{[^}]*'label': <'Open'>", layout)
                check(found is not None and "'Quiet'" in layout, "its menu, over com.canonical.dbusmenu", layout[:300])
                if found:
                    gdbus("--dest", service, "--object-path", "/org/chromium/DbusMenu", "--method",
                          "com.canonical.dbusmenu.Event", found.group(1), "clicked", "<0>", "0")
                    wait(lambda: chosen, 5)
                    check(chosen == ["open"], "an item chosen there is the application's", chosen)
            tray.close()
        except Exception as error:  # noqa: BLE001
            check(False, "the test ran to the end", repr(error))
        finally:
            zebkit.shutdown()
            watcher.terminate()
    print("%d of %d checks passed" % (checks - len(failures), checks), flush=True)
    return 1 if failures or not checks else 0


if __name__ == "__main__":
    sys.exit(main())
