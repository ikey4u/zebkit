"""The Python host keeps its loop, and can close a view from a hook event."""
import gc
import threading
import time
import weakref

import zebkit

assert not zebkit.pump() and not zebkit.shutdown()
condition = threading.Condition()
next_wakeup = None

def wakeup(delay_ms):
    global next_wakeup
    due = time.monotonic() + delay_ms / 1000
    with condition:
        if next_wakeup is None or due < next_wakeup:
            next_wakeup = due
            condition.notify()

assert zebkit.set_wakeup(wakeup)
assert zebkit.init() == 0
assert zebkit.init() != 0
assert not zebkit.set_wakeup(lambda delay: None)
state = {"value": None, "event": None, "posted": False, "worker": False}

class ReadyProbe:
    def __call__(self):
        raise AssertionError("rejected startup called ready")

probe = ReadyProbe()
reference = weakref.ref(probe)
assert zebkit.main(probe) != 0
del probe
gc.collect()
assert reference() is None, "rejected startup must release its callback"

view = zebkit.View()
view.set_popup_policy("block")
assert view.popup_policy == "block"
try:
    view.set_popup_policy("invalid")
    raise AssertionError("invalid policy was accepted")
except ValueError:
    pass
assert view.popup_policy == "block"

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
    state["worker"] = not zebkit.pump() and not zebkit.shutdown()
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
    if ready:
        assert zebkit.pump()

assert state["value"] == (42, None), state
assert state["worker"] and state["posted"], state
assert state["event"] == {"url": "https://popup.invalid/", "frameName": "target", "userGesture": False}, state
assert view._closed
assert zebkit.shutdown()
assert page._closed
assert not zebkit.pump() and not zebkit.shutdown()
assert zebkit.init() != 0
print("ok Python host loop, popup hook, cross-thread post and shutdown", flush=True)
