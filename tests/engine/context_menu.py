"""A page's context menu (docs/SYSTEM_INTEGRATION.md, 8): the default items for
what was right-clicked, their commands run against the context the engine
captured, and the default menu shown where no function takes it.

    python3 tests/engine/context_menu.py [engine switches...]

Headless: the hook's menuId and items, commands run and refused. Under X11
with xdotool: a real right click shows the default menu, and the keyboard
chooses Reload in it. Exit 0 when every check passes.
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
PAGE = ("data:text/html,<title>Menus</title><body style='margin:0'>"
        "<a id=a href='https://example.com/' style='position:absolute;left:10px;top:10px'>a link</a>"
        "<input id=f value='some words' style='position:absolute;left:10px;top:60px;width:200px'>"
        "<script>window.loads=(+window.name||0)+1;window.name=loads</script>")


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


def refused(function):
    try:
        function()
    except zebkit.Error as error:
        return str(error)
    return None


def headless_checks():
    from zebkit.automation import Page, run
    told = []
    state = {}

    async def go():
        page = Page(width=400, height=300)
        await page.goto(PAGE)
        state["view"] = page.view
        page.view.on_context_menu(told.append)
        await page.mouse.click(20, 15, button="right")
        await page.mouse.click(60, 70, button="right")

    future = run(go())
    wait(lambda: future.done() and len(told) >= 2, 15)
    check(len(told) >= 2, "the hook is told each right click", len(told))
    if len(told) < 2:
        return
    link, field = told[0], told[1]
    view = state["view"]
    ids = [item.get("id") for item in link["items"]]
    check("menuId" in link and "copyLink" in ids and "openLink" in ids, "a link's menu: its id and default items", ids)
    ids = [item.get("id") for item in field["items"]]
    check(field["editable"] and "selectAll" in ids and "paste" in ids, "a field's menu: the editing items", ids)
    view.context_menu_run(field["menuId"], "selectAll")
    measure = "(() => { const f = document.getElementById('f'); return f.selectionEnd - f.selectionStart })()"
    # The command goes to the renderer: its selection a moment later.
    wait(lambda: ev(view, measure) == len("some words"), 5)
    selected = ev(view, measure)
    check(selected == len("some words"), "a default command runs against what was right-clicked", selected)
    message = refused(lambda: view.context_menu_run(link["menuId"], "saveImage"))
    check(message is not None, "a command that menu did not offer is refused", message)
    message = refused(lambda: view.context_menu_run(link["menuId"], "rm -rf /"))
    check(message is not None, "and one that is not the engine's", message)
    message = refused(lambda: view.context_menu_run(999999, "copy"))
    check(message is not None, "and a menu that is not there", message)
    if zebkit.is_headless():
        message = refused(lambda: view.context_menu_show(field["menuId"]))
        check(message and "unsupported" in message, "without a window no menu is shown", message)


def windowed_default():
    view = zebkit.View(PAGE, 400, 300)
    wait(lambda: ev(view, "document.readyState") == "complete")
    view.focus()
    wait(lambda: False, 0.5)
    found = subprocess.run(["xdotool", "search", "--name", "^Menus$"], capture_output=True, text=True)
    window = (found.stdout.split() or [None])[-1]
    loads = ev(view, "loads")
    subprocess.run(["xdotool", "mousemove", "--window", window, "300", "200", "click", "3"], check=False)
    wait(lambda: False, 1)
    # The page's menu: Back and Forward disabled, Reload the first enabled.
    subprocess.run(["xdotool", "key", "Down", "Return"], check=False)
    wait(lambda: (ev(view, "window.loads") or 0) > loads, 5)
    check((ev(view, "window.loads") or 0) > loads, "without a function, the default menu is shown, and Reload reloads",
          (loads, ev(view, "window.loads")))
    view.close()


def main():
    if zebkit.init(["context-menu"] + sys.argv[1:]) != 0:
        print("FAIL zebkit.init()")
        return 1
    try:
        headless_checks()
        if not zebkit.is_headless() and shutil.which("xdotool") and os.environ.get("DISPLAY") and \
                not os.environ.get("WAYLAND_DISPLAY") and sys.platform.startswith("linux"):
            windowed_default()
    except Exception as error:  # noqa: BLE001
        check(False, "the test ran to the end", repr(error))
    finally:
        zebkit.shutdown()
    print("%d of %d checks passed" % (checks - len(failures), checks), flush=True)
    return 1 if failures or not checks else 0


if __name__ == "__main__":
    sys.exit(main())
