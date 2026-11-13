"""The Python host keeps its loop, and can close a view from a hook event.

    python3 tests/engine/lifecycle_python.py [engine switches...]

The engine is driven by the host's own loop (zebkit.set_wakeup), not pumped
by the harness. Exit 0 when every check passes.
"""
import gc
import os
import sys
import threading
import time
import weakref

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from harness import check, finish, zebkit  # noqa: E402

condition = threading.Condition()
next_wakeup = None


def wakeup(delay_ms):
    global next_wakeup
    due = time.monotonic() + delay_ms / 1000
    with condition:
        if next_wakeup is None or due < next_wakeup:
            next_wakeup = due
            condition.notify()


class ReadyProbe:
    def __call__(self):
        check(False, "rejected startup called ready")


def main():
    global next_wakeup
    check(not zebkit.pump() and not zebkit.shutdown(), "before init, pump() and shutdown() do nothing")
    check(zebkit.set_wakeup(wakeup), "set_wakeup before init")
    if not check(zebkit.init() == 0, "init"):
        return finish()
    check(zebkit.init() != 0, "a second init is refused")
    check(not zebkit.set_wakeup(lambda delay: None), "set_wakeup after init is refused")
    state = {"value": None, "event": None, "posted": False, "worker": False}

    probe = ReadyProbe()
    reference = weakref.ref(probe)
    check(zebkit.main(probe) != 0, "main() refused once the engine runs")
    del probe
    gc.collect()
    check(reference() is None, "rejected startup releases its callback")

    view = zebkit.View()
    view.set_popup_policy("block")
    try:
        view.set_popup_policy("invalid")
        invalid = "accepted"
    except ValueError:
        invalid = None
    check(invalid is None and view.popup_policy == "block", "set_popup_policy: block, and an invalid policy refused",
          invalid)

    def blocked(event):
        state["event"] = event
        # Notification is outside CanCreateWindow: the host may close its view.
        view.close()

    view.on_popup_blocked(blocked)
    view.load("data:text/html,<script>window.open('https://popup.invalid','target')</script>")
    page = zebkit.View()

    def evaluated(value, error):
        state["value"] = (value, error)

    page.evaluate("6 * 7", evaluated)

    def worker():
        # Off the engine's thread the binding refuses both, with an error.
        refused = 0
        for function in (zebkit.pump, zebkit.shutdown):
            try:
                function()
            except zebkit.Error:
                refused += 1
        state["worker"] = refused == 2
        zebkit.post(lambda: state.update(posted=True))

    thread = threading.Thread(target=worker)
    thread.start()
    thread.join()
    deadline = time.monotonic() + 15
    while time.monotonic() < deadline and not all(state.values()):
        with condition:
            while next_wakeup is None or next_wakeup > time.monotonic():
                remaining = min(next_wakeup or deadline, deadline) - time.monotonic()
                if remaining <= 0:
                    break
                condition.wait(remaining)
            ready = next_wakeup is not None and next_wakeup <= time.monotonic()
            if ready:
                next_wakeup = None
        if ready and not zebkit.pump():
            break

    check(state["value"] == (42, None), "the host's loop runs the page's evaluation", state["value"])
    check(state["worker"] and state["posted"], "off the engine's thread: pump and shutdown refused, post taken", state)
    check(state["event"] == {"url": "https://popup.invalid/", "frameName": "target", "userGesture": False},
          "on_popup_blocked", state["event"])
    check(view._closed, "the view closed from its popup hook")
    check(zebkit.shutdown(), "shutdown")
    check(page._closed, "shutdown closed the other view")
    check(not zebkit.pump() and not zebkit.shutdown(), "after shutdown, pump() and shutdown() do nothing")
    check(zebkit.init() != 0, "init after shutdown is refused")
    return finish()


if __name__ == "__main__":
    sys.exit(main())
