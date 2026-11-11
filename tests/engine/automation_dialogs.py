"""Dialogs that native Playwright actions open (<zebkit/automation.h>,
docs/AUTOMATION.md) while someone else holds the page's dialogs: a click
whose handler calls confirm() or prompt(), with a CDP client attached that
has the Page domain on. The dialog stays open for that client
(Page.javascriptDialogOpening, held in zebkit_view_dialogs()), its type,
message and default text told to the automation client as well
(BrowserContext.dialog, not dismissed), until the CDP client answers it
(Page.handleJavaScriptDialog, a prompt with its text); then the page has the
answer, both are told it closed, and the click is done. The same with the
application's own dialog function. With nobody else holding dialogs and no
listener of the automation client's, a dialog is dismissed, as Playwright
does; with its listener, the automation client answers (Dialog.accept with
promptText).

    python3 tests/engine/automation_dialogs.py [engine switches...]

Exit 0 when every check passes.
"""

import ctypes
import json
import os
import queue
import sys
import threading

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from harness import Server, check, ev, finish, pump, wait, zebkit  # noqa: E402
from cdp_page import Cdp  # noqa: E402
from zebkit import automation  # noqa: E402

PAGE = b"""<!doctype html><title>Dialogs</title><body>
<button id=confirm onclick="out.textContent = confirm('Proceed?') ? 'yes' : 'no'">Confirm</button>
<button id=prompt onclick="out.textContent = 'got:' + prompt('Your name?', 'Bob')">Prompt</button>
<p id=out></p>"""

# ctypes callbacks are not hashable: kept by id until called.
_alive = {}


def call(view, method, params):
    """A native automation command: a list that gets (result, error)."""
    box = []

    def done(handle, result, error, data):
        _alive.pop(id(callback), None)
        box.append((json.loads(result) if result else None, error.decode() if error else None))
    callback = automation._DONE(done)
    _alive[id(callback)] = callback
    automation._call(view._check(), method.encode(), json.dumps(params).encode(), callback, None)
    return box


class CdpWorker:
    """A CDP client on a thread of its own: commands in, events collected."""

    def __init__(self, url):
        self.url = url
        self.cdp = None
        self.requests = queue.Queue()
        self.events = []
        self.stopped = False
        threading.Thread(target=self.run, daemon=True).start()
        # The handshake needs the engine running here.
        wait(lambda: self.cdp is not None, 15)

    def run(self):
        self.cdp = Cdp(self.url)
        while not self.stopped:
            try:
                method, params, session, box = self.requests.get(timeout=.05)
            except queue.Empty:
                self.cdp.event("#none", .05)
                self.drain()
                continue
            try:
                box.append((self.cdp.call(method, params, session), None))
            except Exception as error:  # noqa: BLE001
                box.append((None, error))
            self.drain()

    def drain(self):
        while self.cdp.events:
            event = self.cdp.events.pop(0)
            if event.get("method") != "#reply":
                self.events.append(event)

    def call(self, method, params=None, session=None):
        box = []
        self.requests.put((method, params or {}, session, box))
        wait(lambda: box, 15)
        if not box:
            raise TimeoutError(method)
        if box[0][1]:
            raise box[0][1]
        return box[0][0]

    def got(self, method):
        return [e["params"] for e in self.events if e.get("method") == method]

    def close(self):
        self.stopped = True
        pump(.2)
        self.cdp.sock.close()


def main():
    if zebkit.init(["automation-dialogs"] + sys.argv[1:]) != 0:
        print("FAIL zebkit.init()")
        return 1
    server = Server({"/": PAGE})
    view = zebkit.View(server.base + "/", 600, 400)
    # The page itself, not the blank document before it.
    wait(lambda: ev(view, "location.pathname + document.readyState") == "/complete")
    told = []
    callback = automation._EVENT(lambda h, name, params, d: told.append(
        (name.decode(), json.loads(params) if params else {})))
    automation._on_event(view._check(), callback, None)

    def automation_events(name):
        return [p for n, p in told if n == name]

    # ---- a CDP client holds the page's dialogs ----
    zebkit.cdp_listen("127.0.0.1", 0)
    cdp = CdpWorker(zebkit.cdp_url())
    session = cdp.call("Target.attachToTarget", {"targetId": view.target_id(), "flatten": True})["sessionId"]
    cdp.call("Page.enable", {}, session)

    clicked = call(view, "Frame.click", {"selector": "#confirm", "timeout": 10000})
    wait(lambda: cdp.got("Page.javascriptDialogOpening"), 10)
    opening = cdp.got("Page.javascriptDialogOpening")
    check(opening and opening[0].get("type") == "confirm" and opening[0].get("message") == "Proceed?",
          "a native click's confirm(): Page.javascriptDialogOpening", opening)
    wait(lambda: automation_events("BrowserContext.dialog"), 5)
    events = automation_events("BrowserContext.dialog")
    check(events and events[0].get("type") == "confirm" and events[0].get("message") == "Proceed?" and
          events[0].get("held") is True and not events[0].get("auto"),
          "the automation client is told it, held and not dismissed", events)
    pump(1)
    open_now = view.dialogs()
    check(len(open_now) == 1 and open_now[0]["type"] == "confirm" and open_now[0]["message"] == "Proceed?",
          "the dialog stays open for the CDP client", open_now)
    check(not cdp.got("Page.javascriptDialogClosed") and not automation_events("BrowserContext.dialogClosed"),
          "nobody is told it closed")
    cdp.call("Page.handleJavaScriptDialog", {"accept": True}, session)
    wait(lambda: ev(view, "out.textContent") == "yes", 5)
    check(ev(view, "out.textContent") == "yes", "the CDP client's OK reaches the page")
    wait(lambda: cdp.got("Page.javascriptDialogClosed") and automation_events("BrowserContext.dialogClosed"), 5)
    closed = automation_events("BrowserContext.dialogClosed")
    check(cdp.got("Page.javascriptDialogClosed") and closed and closed[0].get("accepted") is True and
          closed[0].get("dialog") == events[0].get("dialog"), "both clients are told it closed, accepted", closed)
    wait(lambda: clicked, 10)
    check(clicked and clicked[0][1] is None, "the click is done once the dialog is answered", clicked)

    # A prompt with the CDP client's text.
    told.clear()
    clicked = call(view, "Frame.click", {"selector": "#prompt", "timeout": 10000})
    wait(lambda: len(cdp.got("Page.javascriptDialogOpening")) == 2, 10)
    opening = cdp.got("Page.javascriptDialogOpening")[-1]
    check(opening.get("type") == "prompt" and opening.get("defaultPrompt") == "Bob", "a prompt, its default", opening)
    events = automation_events("BrowserContext.dialog")
    check(events and events[-1].get("defaultValue") == "Bob" and not events[-1].get("auto"),
          "its default text told to the automation client", events)
    cdp.call("Page.handleJavaScriptDialog", {"accept": True, "promptText": "Ann"}, session)
    wait(lambda: ev(view, "out.textContent") == "got:Ann", 5)
    check(ev(view, "out.textContent") == "got:Ann", "the prompt's answer reaches the page")
    wait(lambda: automation_events("BrowserContext.dialogClosed"), 5)
    closed = automation_events("BrowserContext.dialogClosed")
    check(closed and closed[-1].get("userInput") == "Ann", "closed with the user's input", closed)
    wait(lambda: clicked, 10)
    check(clicked and clicked[0][1] is None, "that click is done too", clicked)

    # Dismissed by the CDP client.
    clicked = call(view, "Frame.click", {"selector": "#confirm", "timeout": 10000})
    wait(lambda: len(cdp.got("Page.javascriptDialogOpening")) == 3, 10)
    cdp.call("Page.handleJavaScriptDialog", {"accept": False}, session)
    wait(lambda: ev(view, "out.textContent") == "no", 5)
    check(ev(view, "out.textContent") == "no", "the CDP client's Cancel reaches the page")
    wait(lambda: clicked, 10)
    cdp.close()
    pump(.5)

    # ---- the application's own function holds them ----
    told.clear()
    seen = []
    view.on_dialog(seen.append)
    clicked = call(view, "Frame.click", {"selector": "#confirm", "timeout": 10000})
    wait(lambda: seen, 10)
    pump(.5)
    check(seen and seen[0]["held"] and len(view.dialogs()) == 1, "held for the application's function", seen)
    events = automation_events("BrowserContext.dialog")
    check(events and not events[0].get("auto"), "and not dismissed by automation", events)
    if seen:
        check(view.respond_dialog(seen[0]["id"], True), "the application answers it")
    wait(lambda: ev(view, "out.textContent") == "yes", 5)
    check(ev(view, "out.textContent") == "yes", "its OK reaches the page")
    wait(lambda: clicked, 10)
    view.on_dialog(None)

    # ---- nobody else: dismissed without a listener, as Playwright ----
    told.clear()
    ev(view, "out.textContent = ''; 1")
    clicked = call(view, "Frame.click", {"selector": "#confirm", "timeout": 10000})
    wait(lambda: ev(view, "out.textContent") == "no", 10)
    # Not held at all (dialog.cc's DialogState::Decide()): answered Cancel
    # at once, as headless Chrome does with nobody to ask.
    check(ev(view, "out.textContent") == "no" and not view.dialogs(),
          "with nobody holding it, dismissed at once", told)
    wait(lambda: clicked, 10)
    check(clicked and clicked[0][1] is None, "and the click done", clicked)

    # ---- the automation client's listener answers, a prompt with its text ----
    told.clear()
    sub = call(view, "Page.updateSubscription", {"event": "dialog", "enabled": True})
    wait(lambda: sub, 5)
    clicked = call(view, "Frame.click", {"selector": "#prompt", "timeout": 10000})
    wait(lambda: automation_events("BrowserContext.dialog"), 10)
    events = automation_events("BrowserContext.dialog")
    answer = call(view, "Dialog.accept", {"dialog": events[0]["dialog"], "promptText": "Zed"}) if events else []
    wait(lambda: answer, 5)
    check(answer and answer[0][1] is None, "Dialog.accept with promptText", answer)
    wait(lambda: ev(view, "out.textContent") == "got:Zed", 5)
    check(ev(view, "out.textContent") == "got:Zed", "the automation client's text reaches the page")
    again = call(view, "Dialog.accept", {"dialog": events[0]["dialog"]}) if events else []
    wait(lambda: again, 5)
    check(again and again[0][1], "a dialog answered already cannot be answered again", again)
    wait(lambda: clicked, 10)

    automation._on_event(view._check(), automation._EVENT(), None)
    view.close()
    server.close()
    zebkit.shutdown()
    return finish()


if __name__ == "__main__":
    sys.exit(main())
