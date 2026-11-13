"""zebkit_view_on_responsiveness(): a page that stops answering input is told
{"responsive": false}, then {"responsive": true} once it answers again, each
once. Chromium's hang monitor, run with its delay lowered
(--enable-features=RendererHangWatcher:delay/2s, which the test adds): the
page's main thread is kept busy for 6 s while keys wait on it.

With --headless, off-screen views, the keys sent with zebkit_view_send_key():
one after an evaluation (the view's own DevTools session does not hold the
monitor), and one beside it with an external CDP client attached
(zebkit_cdp_listen()), which, as in Chrome, holds it: nothing is told. Without
--headless (X11, xdotool), a window and a view attached in another view's
window, the keys the user's.

    python3 -I tests/engine/responsiveness.py [engine switches...]

Exit 0 when every check passes.
"""

import ctypes
import json
import os
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from harness import Cdp, check, ev, finish, pump, wait, x11_window, xdotool, zebkit  # noqa: E402

lib = zebkit._lib
TOLD = ctypes.CFUNCTYPE(None, ctypes.c_void_p, ctypes.c_char_p, ctypes.c_void_p)
lib.zebkit_view_on_responsiveness.argtypes = [ctypes.c_void_p, TOLD, ctypes.c_void_p]
lib.zebkit_view_attach.restype = ctypes.c_bool
lib.zebkit_view_attach.argtypes = [ctypes.c_void_p, ctypes.c_void_p] + [ctypes.c_int] * 4
lib.zebkit_cdp_listen.restype = ctypes.c_uint
lib.zebkit_cdp_listen.argtypes = [ctypes.c_char_p, ctypes.c_uint, ctypes.c_void_p]
lib.zebkit_cdp_get_url.restype = ctypes.c_void_p
lib.zebkit_view_get_target_id.restype = ctypes.c_void_p
lib.zebkit_view_get_target_id.argtypes = [ctypes.c_void_p]
lib.zebkit_free.argtypes = [ctypes.c_void_p]

BUSY_MS = 6000
_kept = []


def page(title):
    # A listener, so that keys wait on the page's main thread.
    return ("data:text/html,<title>%s</title><script>addEventListener('keydown', () => {});"
            "addEventListener('mousedown', () => {})</script>" % title)


def text(pointer):
    value = ctypes.string_at(pointer).decode() if pointer else None
    if pointer:
        lib.zebkit_free(pointer)
    return value


def listen(view):
    """What zebkit_view_on_responsiveness() tells of view, in order."""
    told = []
    function = TOLD(lambda h, j, d: told.append(json.loads(j.decode())["responsive"]))
    _kept.append(function)
    lib.zebkit_view_on_responsiveness(view._check(), function, None)
    return told


def opened(view, title):
    loads = []
    view.on_load(lambda phase, u: loads.append(phase))
    view.load(page(title))
    wait(lambda: zebkit.LOAD_FINISHED in loads, 20)
    return zebkit.LOAD_FINISHED in loads


def busy(views, send, told_lists):
    """Keeps each page's main thread busy for BUSY_MS, input sent meanwhile,
    until every list in told_lists has two entries or the time is up."""
    for view in views:
        ev(view, "setTimeout(() => { const end = Date.now() + %d; while (Date.now() < end); }, 100); 1" % BUSY_MS)
    pump(0.3)
    end = time.time() + BUSY_MS / 1000 + 4
    while time.time() < end and not all(len(t) >= 2 for t in told_lists):
        send()
        pump(0.25)
    pump(0.5)


def headless():
    plain = zebkit.View.offscreen("about:blank", 300, 200)
    held = zebkit.View.offscreen("about:blank", 300, 200)
    told, told_held = listen(plain), listen(held)
    check(opened(plain, "plain") and opened(held, "held"), "two off-screen pages loaded")
    check(ev(plain, "1 + 1") == 2, "the first evaluated, through its own DevTools session")
    # An external client on the second: attached from a thread of its own, as
    # the engine runs on this one.
    error = ctypes.c_void_p()
    port = lib.zebkit_cdp_listen(None, 0, ctypes.byref(error))
    url = text(lib.zebkit_cdp_get_url()) if port else None
    target = text(lib.zebkit_view_get_target_id(held._check()))
    attached = []

    def attach():
        try:
            client = Cdp(url)
            client.call("Target.attachToTarget", {"targetId": target, "flatten": True})
            attached.append(client)
        except Exception as failure:  # noqa: BLE001
            attached.append(failure)

    if url and target:
        threading.Thread(target=attach, daemon=True).start()
        wait(lambda: attached, 15)
    ok = bool(attached) and isinstance(attached[0], Cdp)
    check(ok, "an external CDP client attached to the second", None if ok else (attached or text(error.value)))

    def keys():
        for view in (plain, held):
            view.send_key(zebkit.KEY_DOWN, key_code=0x41, code="KeyA", key="a")
            view.send_key(zebkit.KEY_UP, key_code=0x41, code="KeyA", key="a")

    busy([plain, held], keys, [told])
    check(told == [False, True], "an off-screen view is told unresponsive, then responsive, each once", told)
    check(told_held == [], "a page an external DevTools client holds is told nothing", told_held)
    if attached and isinstance(attached[0], Cdp):
        attached[0].close()
    for view in (plain, held):
        view.close()


def x11():
    window = zebkit.View()
    told = listen(window)
    check(opened(window, "hang-window"), "a window's page loaded")
    found = x11_window("hang-window", 10)
    check(found is not None, "its X11 window is there")
    parent = zebkit.View()
    child = zebkit.View()
    told_child = listen(child)
    check(opened(parent, "hang-parent") and opened(child, "hang-child"), "a parent and a child page loaded")
    check(lib.zebkit_view_attach(child._check(), parent._check(), 0, 0, 300, 200),
          "the child attached in the parent's window")
    parent_window = x11_window("hang-parent", 10)
    if not (found and parent_window):
        return
    # Apart, so that each click lands in its own window.
    xdotool("windowmove", found, "0", "0")
    xdotool("windowmove", parent_window, "900", "0")
    pump(0.5)

    def input_events():
        for target in (found, parent_window):
            xdotool("mousemove", "--window", target, "60", "60", "click", "1")

    busy([window, child], input_events, [told, told_child])
    check(told == [False, True], "a window is told unresponsive, then responsive, each once", told)
    check(told_child == [False, True], "an attached view is told unresponsive, then responsive", told_child)
    for view in (child, parent, window):
        view.close()


def main():
    if sys.platform == "win32":
        # Chromium gives a visible page's renderer the normal priority class
        # (base::Process::SetPriority), whatever the browser's own. A test run
        # below normal, as Task Scheduler starts its tasks, is then starved by
        # two busy pages on a two-core machine: its pump, the keys and the hang
        # monitor's timer wait for the pages to finish. The browser at normal,
        # as an application started by a person is.
        kernel32 = ctypes.windll.kernel32
        kernel32.GetCurrentProcess.restype = ctypes.c_void_p
        kernel32.SetPriorityClass.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
        kernel32.SetPriorityClass(kernel32.GetCurrentProcess(), 0x20)  # NORMAL_PRIORITY_CLASS
    args = sys.argv[1:] + ["--enable-features=RendererHangWatcher:delay/2s"]
    if zebkit.init(["responsiveness"] + args) != 0:
        print("FAIL zebkit.init()")
        return 1
    try:
        try:
            headless() if "--headless" in args else x11()
        except Exception as error:  # noqa: BLE001
            check(False, "the test ran to the end", repr(error))
    finally:
        zebkit.shutdown()
    return finish()


if __name__ == "__main__":
    sys.exit(main())
