"""Find in page (<zebkit/page.h>, zebkit_view_find), as Chrome's find bar
finds: text in the main document, a same-site frame, another site's frame
and open and closed shadow trees; the count and the active match, next and
previous around the end, match case, counting with no active match, every
match highlighted and the active one distinctly, stopping (the highlights
gone, the match kept selected); and the same over CDP (Zebkit.find,
Zebkit.findResult, Zebkit.stopFinding, Zebkit.getFindResult).

    python3 tests/engine/find_in_page.py [engine switches...]

Exit 0 when every check passes.
"""

import os
import sys
import threading

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from harness import Server, check, ev, finish, skip, wait, zebkit  # noqa: E402
from cdp_page import Cdp  # noqa: E402

FRAME = b"<!doctype html><body style='margin:0'><p>apple in a frame</p>"
CROSS = b"<!doctype html><body style='margin:0'><p>apple of another site</p>"


def page(other):
    return ("""<!doctype html><title>Find</title><body style="margin:8px;font:20px sans-serif">
<p id=first>apple one</p><p>Apple two</p><div id=open></div><div id=closed></div>
<iframe src="/frame.html" style="width:300px;height:60px;border:0"></iframe>
<iframe src="%s/cross.html" style="width:300px;height:60px;border:0"></iframe>
<script>
window.loaded = 0;
for (const f of document.querySelectorAll('iframe')) f.onload = () => window.loaded++;
document.getElementById('open').attachShadow({mode: 'open'}).innerHTML = '<span>apple in the open</span>';
document.getElementById('closed').attachShadow({mode: 'closed'}).innerHTML = '<span>apple, closed</span>';
</script>""" % other).encode()


class Results:
    def __init__(self, view):
        self.all = []
        view.on_find(self.all.append)

    def final(self, request):
        """The final result of request, once it has come."""
        found = [r for r in self.all if r["requestId"] == request and r["final"]]
        return found[-1] if found else None


def find(view, results, text, flags=0):
    request = view.find(text, flags)
    if not check(request > 0, "find(%r) is request %d" % (text, request)):
        return {}
    wait(lambda: results.final(request), 10)
    return results.final(request) or {}


def highlight_colours(view):
    """(yellow pixels, orange pixels) in a capture: Blink's find highlights,
    the matches yellow and the active one orange."""
    box = []
    view.capture(lambda png, error=None: box.append(png))
    if not wait(lambda: box, 10) or not box[0]:
        return None
    width, height, rgba = zebkit.image_decode(box[0])
    yellow = orange = 0
    for i in range(0, len(rgba), 4):
        r, g, b = rgba[i], rgba[i + 1], rgba[i + 2]
        if r > 230 and g > 230 and b < 60:
            yellow += 1
        elif r > 230 and 120 < g < 180 and b < 80:
            orange += 1
    return yellow, orange


def in_thread(function, seconds=30):
    """function() on a thread of its own while the engine runs here."""
    box = []

    def run():
        try:
            box.append((function(), None))
        except Exception as error:  # noqa: BLE001
            box.append((None, error))
    threading.Thread(target=run, daemon=True).start()
    wait(lambda: box, seconds)
    if not box:
        return None, TimeoutError("no answer")
    return box[0]


def main():
    if zebkit.init(["find-in-page"] + sys.argv[1:]) != 0:
        print("FAIL zebkit.init()")
        return 1
    other = Server({"/cross.html": CROSS})
    # 127.0.0.1 and localhost are two sites: the second frame is another site's.
    other_base = "http://localhost:%d" % other.port
    server = Server({"/": page(other_base), "/frame.html": FRAME})
    view = zebkit.View(server.base + "/", 800, 600)
    wait(lambda: ev(view, "document.readyState + window.loaded") == "complete2", 15)
    check(ev(view, "window.loaded") == 2, "the page and both of its frames are loaded")
    results = Results(view)

    # Six matches: two in the document, one in each shadow tree, one in each frame.
    first = find(view, results, "apple")
    check(first.get("matches") == 6, "six matches across the document, frames and shadow trees", first)
    check(first.get("activeMatch") == 1 and first.get("text") == "apple", "the first is active", first)
    rect = first.get("rect") or {}
    check(rect.get("width", 0) > 0 and rect.get("height", 0) > 0, "the active match's rect", rect)
    colours = highlight_colours(view)
    if colours is None:
        skip("highlight colours", "no capture of the view")
    else:
        check(colours[0] > 50 and colours[1] > 50, "matches highlighted, the active one distinctly", colours)
    # Next, previous, and around the end.
    order = [find(view, results, "apple").get("activeMatch") for _ in range(5)]
    check(order == [2, 3, 4, 5, 6], "next goes through all of them", order)
    wrapped = find(view, results, "apple")
    check(wrapped.get("activeMatch") == 1, "and back to the first after the last", wrapped)
    back = find(view, results, "apple", zebkit.FIND_BACKWARD)
    check(back.get("activeMatch") == 6 and back.get("matches") == 6, "previous from the first is the last", back)
    back = find(view, results, "apple", zebkit.FIND_BACKWARD)
    check(back.get("activeMatch") == 5, "and the one before", back)
    result = view.find_result()
    check(result.get("activeMatch") == 5 and result.get("stopped") is False, "find_result() is the last result", result)

    # Case.
    cased = find(view, results, "Apple", zebkit.FIND_MATCH_CASE)
    check(cased.get("matches") == 1 and cased.get("activeMatch") == 1, "match case: one", cased)
    missing = find(view, results, "pear")
    check(missing.get("matches") == 0 and missing.get("activeMatch") == 0 and missing.get("rect") is None,
          "no match", missing)

    # Stopping: the highlights go, and the active match stays selected with KEEP.
    view.stop_finding(zebkit.FIND_STOP_CLEAR)
    check(any(r.get("stopped") for r in results.all[-1:]), "stopping is told", results.all[-1:])
    first = find(view, results, "apple")
    check(first.get("activeMatch") == 1, "a new search starts at the first again", first)
    view.stop_finding(zebkit.FIND_STOP_KEEP)
    check(ev(view, "getSelection().toString()") == "apple", "KEEP: the active match stays selected")
    stopped = view.find_result()
    check(stopped.get("stopped") is True and stopped.get("matches") == 0, "nothing is on after stopping", stopped)
    colours = highlight_colours(view)
    if colours is not None:
        check(colours[0] == 0 and colours[1] == 0, "stopping clears every highlight", colours)
    ev(view, "getSelection().removeAllRanges(); 1")

    # Counting and highlighting alone: no active match.
    counted = find(view, results, "apple", zebkit.FIND_HIGHLIGHT_ONLY)
    check(counted.get("matches") == 6 and counted.get("activeMatch") == 0, "highlight only: counted, none active",
          counted)
    view.stop_finding(zebkit.FIND_STOP_CLEAR)
    check(view.find("", 0) == 0, "empty text finds nothing")

    # A search ends with its document.
    find(view, results, "apple")
    view.load(server.base + "/frame.html")
    wait(lambda: view.find_result().get("stopped") is True, 10)
    check(view.find_result().get("stopped") is True, "navigating stops the search")
    view.load(server.base + "/")
    wait(lambda: ev(view, "document.readyState + window.loaded") == "complete2", 15)

    # CDP: the page's session finds, hears the results and stops.
    zebkit.cdp_listen("127.0.0.1", 0)
    target = view.target_id()
    url = zebkit.cdp_url()

    def over_cdp():
        cdp = Cdp(url)
        session = cdp.call("Target.attachToTarget", {"targetId": target, "flatten": True})["sessionId"]
        out = {}
        out["request"] = cdp.call("Zebkit.find", {"text": "apple"}, session)["requestId"]
        while True:
            event = cdp.event("Zebkit.findResult", 10)
            if event is None or (event["params"]["requestId"] == out["request"] and event["params"]["final"]):
                out["first"] = event and event["params"]
                break
        nxt = cdp.call("Zebkit.find", {"text": "apple"}, session)["requestId"]
        while True:
            event = cdp.event("Zebkit.findResult", 10)
            if event is None or (event["params"]["requestId"] == nxt and event["params"]["final"]):
                out["next"] = event and event["params"]
                break
        out["result"] = cdp.call("Zebkit.getFindResult", {}, session)["result"]
        cdp.call("Zebkit.stopFinding", {"action": "clear"}, session)
        out["stopped"] = (cdp.event("Zebkit.findResult", 10) or {}).get("params")
        try:
            cdp.call("Zebkit.find", {"text": ""}, session)
            out["empty"] = "accepted"
        except RuntimeError as error:
            out["empty"] = str(error)
        try:
            cdp.call("Zebkit.find", {"text": "x"})
            out["browser"] = "accepted"
        except RuntimeError as error:
            out["browser"] = str(error)
        return out
    out, error = in_thread(over_cdp)
    if check(error is None, "the CDP client's run", error):
        # As Chrome, a new search starts in the focused frame: the active
        # match is one of them, and the next follows it.
        active = (out["first"] or {}).get("activeMatch", 0)
        check((out["first"] or {}).get("matches") == 6 and 1 <= active <= 6,
              "Zebkit.find: Zebkit.findResult with six matches, one active", out["first"])
        check((out["next"] or {}).get("activeMatch") == active % 6 + 1, "Zebkit.find again: the next", out["next"])
        check(out["result"].get("activeMatch") == active % 6 + 1, "Zebkit.getFindResult", out["result"])
        check((out["stopped"] or {}).get("stopped") is True, "Zebkit.stopFinding is told", out["stopped"])
        check("text" in out["empty"], "Zebkit.find without text is refused", out["empty"])
        check("page" in out["browser"], "Zebkit.find is a page's", out["browser"])
    view.close()
    server.close()
    other.close()
    zebkit.shutdown()
    return finish()


if __name__ == "__main__":
    sys.exit(main())
