"""A real input method in an X11 window (src/engine/ime_ibus_linux.cc): the
engine as a client of IBus, here a fake one on a bus of its own
(fake_ibus.py). Letters typed with real keys are composed in the field, a
space commits Chinese, Enter the letters, Escape nothing; keys the input method
does not take reach the page; a password field is not the input method's. And
a focused field's ring is Chrome's colour, not Blink's placeholder orange.

    dbus-run-session -- python3 tests/engine/input_method.py [engine switches...]

Needs an X display (not Wayland) and xdotool. Exit 0 when every check passes.
"""

import json
import os
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "bindings", "python"))

failures = []
checks = 0

PAGE = ("data:text/html,<title>ime</title><input id=a autofocus><input id=p type=password><p id=out></p><script>"
        "window.events = []; for (const e of ['compositionstart','compositionupdate','compositionend','keydown'])"
        " document.addEventListener(e, ev => events.push(e + ':' + (ev.data ?? ev.key)), true);</script>")


def check(ok, what, detail=None):
    global checks
    checks += 1
    print(("ok   " if ok else "FAIL ") + what + (": %r" % (detail,) if detail is not None else ""), flush=True)
    if not ok:
        failures.append(what)


def main():
    with tempfile.TemporaryDirectory(prefix="zebkit-ime-") as directory:
        ready, log = os.path.join(directory, "ready"), os.path.join(directory, "log")
        fake = subprocess.Popen([sys.executable, os.path.join(HERE, "fake_ibus.py"), ready, log])
        end = time.monotonic() + 10
        while not os.path.exists(ready) and time.monotonic() < end:
            time.sleep(.05)
        # Before the engine starts: where it looks for IBus.
        os.environ["IBUS_ADDRESS"] = os.environ["DBUS_SESSION_BUS_ADDRESS"]
        import zebkit

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

        def keys(*args):
            # The engine answers the input method while xdotool types: keep pumping.
            child = subprocess.Popen(["xdotool", *args])
            wait(lambda: child.poll() is not None, 30)

        if zebkit.init(["input-method"] + sys.argv[1:]) != 0:
            print("FAIL zebkit.init()")
            return 1
        try:
            view = zebkit.View(PAGE, 500, 300)
            wait(lambda: view.title == "ime")
            view.focus()
            wait(lambda: ev(view, "document.hasFocus() && document.activeElement.id") == "a", 5)
            ring = ev(view, "getComputedStyle(a).outlineColor")
            check(ring != "rgb(229, 151, 0)", "the focus ring is not Blink's placeholder orange", ring)
            wait(lambda: os.path.exists(log) and "FocusIn" in open(log).read(), 5)
            check(os.path.exists(log) and "FocusIn" in open(log).read(), "the field's focus reaches the input method",
                  os.path.exists(log) and open(log).read())

            keys("type", "--delay", "60", "nihao")
            wait(lambda: ev(view, "a.value") == "nihao", 5)
            events = json.loads(ev(view, "JSON.stringify(events)") or "[]")
            check(ev(view, "a.value") == "nihao" and "compositionstart:" in events and
                  "compositionupdate:nihao" in events and "keydown:Process" in events,
                  "letters are composed in the field", events[-4:])
            keys("key", "space")
            wait(lambda: ev(view, "a.value") == "你好", 5)
            events = json.loads(ev(view, "JSON.stringify(events)") or "[]")
            check(ev(view, "a.value") == "你好" and "compositionend:你好" in events, "space commits the Chinese",
                  (ev(view, "a.value"), events[-3:]))

            keys("type", "--delay", "60", "ok")
            keys("key", "Return")
            wait(lambda: ev(view, "a.value") == "你好ok", 5)
            check(ev(view, "a.value") == "你好ok", "Enter commits the letters as they are", ev(view, "a.value"))
            keys("type", "--delay", "60", "zz")
            keys("key", "Escape")
            wait(lambda: ev(view, "a.value") == "你好ok", 5)
            check(ev(view, "a.value") == "你好ok", "Escape drops the composition", ev(view, "a.value"))

            ev(view, "events.length = 0; 1")
            keys("key", "exclam")
            wait(lambda: ev(view, "a.value") == "你好ok!", 5)
            events = json.loads(ev(view, "JSON.stringify(events)") or "[]")
            check(ev(view, "a.value") == "你好ok!" and "keydown:!" in events,
                  "a key the input method does not take is the page's", (ev(view, "a.value"), events))
            keys("key", "ctrl+a")
            keys("key", "BackSpace")
            wait(lambda: ev(view, "a.value") == "", 5)
            check(ev(view, "a.value") == "", "shortcuts and editing keys still work", ev(view, "a.value"))

            ev(view, "p.focus(); events.length = 0; 1")
            wait(lambda: "FocusOut" in open(log).read(), 5)
            keys("type", "--delay", "60", "nihao")
            keys("key", "space")
            wait(lambda: ev(view, "p.value") == "nihao ", 5)
            check(ev(view, "p.value") == "nihao " and "FocusOut" in open(log).read(),
                  "a password field is not the input method's", ev(view, "p.value"))
            check("SetCursorLocation" in open(log).read(), "the caret's place is told, for the candidates")
            view.close()
        except Exception as error:  # noqa: BLE001
            check(False, "the test ran to the end", repr(error))
        finally:
            zebkit.shutdown()
            fake.terminate()
    print("%d of %d checks passed" % (checks - len(failures), checks), flush=True)
    return 1 if failures or not checks else 0


if __name__ == "__main__":
    sys.exit(main())
