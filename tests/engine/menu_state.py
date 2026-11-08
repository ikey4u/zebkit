"""The application's menu items' state (docs/SYSTEM_INTEGRATION.md, 8):
enabled, checked and radio groups changed by id without a new menu, and a
disabled item's key left to the page.

    python3 tests/engine/menu_state.py [engine switches...]

Under X11 with xdotool, real Ctrl+S presses in a window: the page has a key
first, and one it does not take is the item's while it is enabled; a disabled
item takes none. Exit 0 when every check passes.
"""

import os
import shutil
import subprocess
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "bindings", "python"))

import zebkit  # noqa: E402

failures = []
checks = 0
MENU = [{"title": "File", "items": [
    {"title": "Save", "id": "save", "key": "s"},
    {"separator": True},
    {"title": "Small", "id": "small", "radioGroup": "size", "checked": True},
    {"title": "Large", "id": "large", "radioGroup": "size"},
]}]


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


def item(menu, item_id):
    for entry in menu:
        if entry.get("id") == item_id:
            return entry
        found = item(entry.get("items", []), item_id)
        if found:
            return found
    return None


def main():
    if zebkit.init(["menu-state"] + sys.argv[1:]) != 0:
        print("FAIL zebkit.init()")
        return 1
    chosen = []
    try:
        zebkit.app_set_menu(MENU, chosen.append)
        menu = zebkit.app_menu()
        check(item(menu, "save")["enabled"] and item(menu, "small")["checked"] and not item(menu, "large")["checked"],
              "the menu's state as given", menu)
        zebkit.app_update_menu_item("large", checked=True)
        menu = zebkit.app_menu()
        check(item(menu, "large")["checked"] and not item(menu, "small")["checked"],
              "checking one of a radio group unchecks the others", menu)
        zebkit.app_update_menu_item("save", title="Save All")
        check(item(zebkit.app_menu(), "save")["title"] == "Save All", "a title changes by id")
        for bad in ({"colour": "red"}, {"enabled": "no"}):
            try:
                zebkit.app_update_menu_item("save", **bad)
            except zebkit.Error:
                check(True, "invalid state is refused: %s" % list(bad))
            else:
                check(False, "invalid state is refused: %s" % list(bad))
        try:
            zebkit.app_update_menu_item("nothing", enabled=False)
        except zebkit.Error:
            check(True, "an id of no item is refused")
        else:
            check(False, "an id of no item is refused")

        if not zebkit.is_headless() and shutil.which("xdotool") and os.environ.get("DISPLAY") and \
                not os.environ.get("WAYLAND_DISPLAY") and sys.platform.startswith("linux"):
            view = zebkit.View("data:text/html,<title>Menu keys</title><script>window.keys=0;"
                               "addEventListener('keydown',e=>{if(e.key==='s'&&e.ctrlKey)keys++})</script>", 400, 300)
            wait(lambda: ev(view, "document.readyState") == "complete")
            view.focus()
            wait(lambda: False, 0.5)
            found = subprocess.run(["xdotool", "search", "--name", "^Menu keys$"], capture_output=True, text=True)
            window = (found.stdout.split() or [None])[-1]
            subprocess.run(["xdotool", "key", "--window", window, "ctrl+s"], check=False)
            wait(lambda: chosen, 3)
            # The page has a key first; one it does not take is the menu's.
            check(chosen == ["save"], "an enabled item takes its key", chosen)
            zebkit.app_update_menu_item("save", enabled=False)
            keys = ev(view, "keys")
            subprocess.run(["xdotool", "key", "--window", window, "ctrl+s"], check=False)
            wait(lambda: ev(view, "keys") == keys + 1 or ev(view, "keys") > keys, 3)
            wait(lambda: False, 0.5)
            check(chosen == ["save"] and ev(view, "keys") > keys, "a disabled item's key is the page's alone",
                  (chosen, ev(view, "keys")))
            view.close()
        else:
            print("ok   keys: not on this display (X11 with xdotool only)", flush=True)
    except Exception as error:  # noqa: BLE001
        check(False, "the test ran to the end", repr(error))
    finally:
        zebkit.shutdown()
    print("%d of %d checks passed" % (checks - len(failures), checks), flush=True)
    return 1 if failures or not checks else 0


if __name__ == "__main__":
    sys.exit(main())
