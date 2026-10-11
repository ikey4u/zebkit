#!/usr/bin/env python3
"""Raw Chrome DevTools Protocol checks: protocol.py CDP_PORT PAGE_PORT OTHER_PORT

Talks to a browser the way a CDP client would and checks what it answers.
Pages are tests/cdp/pages, served on PAGE_PORT and again on OTHER_PORT (a
different origin). Run through scripts/cdp.sh.

The same checks run against Chrome itself (scripts/cdp.sh --reference),
which is what keeps them honest: a check that Chrome fails is asserting
something Chrome does not do. The few things zebkit does on purpose that
Chrome does not are marked zebkit-only and left out of that run.

Every message in both directions is also compared with the standard
protocol definition (see schema.py).
"""

import base64
import os
import pathlib
import socket
import sys
import tempfile
import time
import traceback

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from cdpclient import NONSTANDARD, Connection, ProtocolError, http_json  # noqa: E402

CDP_PORT, PAGE_PORT, OTHER_PORT = (int(argument) for argument in sys.argv[1:4])
ONLY = sys.argv[4:]
# Set when the browser on CDP_PORT is Chrome and not zebkit.
REFERENCE = bool(os.environ.get("CDP_REFERENCE"))
PAGES = f"http://127.0.0.1:{PAGE_PORT}"
OTHER = f"http://localhost:{OTHER_PORT}"

CHECKS = []


def check(function):
    CHECKS.append(function)
    return function


def zebkit_only(function):
    """A check of something zebkit does on purpose and Chrome does not."""
    function.zebkit_only = True
    return check(function)


def expect(condition, message):
    if not condition:
        raise AssertionError(message)


def equal(actual, expected, what):
    if actual != expected:
        raise AssertionError(f"{what}: expected {expected!r}, got {actual!r}")


def near(actual, expected, what, tolerance=1.5):
    if abs(actual - expected) > tolerance:
        raise AssertionError(f"{what}: expected about {expected}, got {actual}")


class Page:
    """A fresh page with its own socket, closed at the end of the check."""

    def __init__(self, url=None):
        self.browser = Connection(http_json(CDP_PORT, "/json/version")["webSocketDebuggerUrl"])
        self.target = self.browser.call("Target.createTarget", {"url": "about:blank"})["targetId"]
        self.cdp = Connection(f"ws://127.0.0.1:{CDP_PORT}/devtools/page/{self.target}")
        self.cdp.call("Page.enable")
        self.cdp.call("Page.setLifecycleEventsEnabled", {"enabled": True})
        self.cdp.call("Runtime.enable")
        self.cdp.call("Network.enable")
        if url:
            self.goto(url)

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.cdp.close()
        try:
            self.browser.call("Target.closeTarget", {"targetId": self.target})
        except (ProtocolError, OSError):
            pass
        self.browser.close()

    def goto(self, url, wait="load"):
        self.cdp.take_events("Page.lifecycleEvent")
        result = self.cdp.call("Page.navigate", {"url": url})
        expect("errorText" not in result, f"navigation to {url} failed: {result.get('errorText')}")
        loader = result.get("loaderId")
        self.cdp.wait_event("Page.lifecycleEvent", lambda p: p["name"] == wait and p["frameId"] == self.target
                            and (loader is None or p["loaderId"] == loader))
        return result

    def evaluate(self, expression, **options):
        params = {"expression": expression, "returnByValue": True, "awaitPromise": True}
        params.update(options)
        result = self.cdp.call("Runtime.evaluate", params)
        if "exceptionDetails" in result:
            raise AssertionError(f"{expression} threw: {result['exceptionDetails']}")
        return result["result"].get("value")

    def handle(self, expression):
        result = self.cdp.call("Runtime.evaluate", {"expression": expression})
        expect("objectId" in result["result"], f"{expression} did not give an object: {result}")
        return result["result"]["objectId"]

    def mouse(self, kind, x, y, **options):
        params = {"type": kind, "x": x, "y": y}
        params.update(options)
        self.cdp.call("Input.dispatchMouseEvent", params)

    def click(self, x, y, count=1, button="left"):
        self.mouse("mouseMoved", x, y)
        self.mouse("mousePressed", x, y, button=button, clickCount=count)
        self.mouse("mouseReleased", x, y, button=button, clickCount=count)

    def key(self, key, text=None, modifiers=0):
        # As client libraries send it: with the physical key and its code.
        code, virtual = KEYS.get(key) or (f"Key{key.upper()}" if key.isalpha() else f"Digit{key}", ord(key.upper()))
        params = {"key": key, "code": code, "windowsVirtualKeyCode": virtual, "modifiers": modifiers}
        down = dict(params, type="keyDown" if text else "rawKeyDown")
        if text:
            down["text"] = text
        if modifiers & 2 and key == "a":
            down["commands"] = ["selectAll"]
        self.cdp.call("Input.dispatchKeyEvent", down)
        self.cdp.call("Input.dispatchKeyEvent", dict(params, type="keyUp"))


KEYS = {"Backspace": ("Backspace", 8), "Tab": ("Tab", 9), "Enter": ("Enter", 13), "Delete": ("Delete", 46), " ": ("Space", 32)}


# -- discovery and access -------------------------------------------------


@check
def http_discovery():
    version = http_json(CDP_PORT, "/json/version")
    expect(version["webSocketDebuggerUrl"].startswith(f"ws://127.0.0.1:{CDP_PORT}/devtools/browser/"), version)
    equal(version["Protocol-Version"], "1.3", "protocol version")
    pages = [target for target in http_json(CDP_PORT, "/json/list") if target["type"] == "page"]
    expect(pages, "no page is listed")
    expect(pages[0]["webSocketDebuggerUrl"].endswith("/devtools/page/" + pages[0]["id"]), pages[0])
    created = http_json(CDP_PORT, "/json/new?about:blank", method="PUT")
    expect(any(page["id"] == created["id"] for page in http_json(CDP_PORT, "/json")), "new page is not listed")
    with socket.create_connection(("127.0.0.1", CDP_PORT)) as raw:
        raw.sendall(f"GET /json/close/{created['id']} HTTP/1.1\r\nHost: 127.0.0.1:{CDP_PORT}\r\nConnection: close\r\n\r\n".encode())
        expect(b" 200 " in raw.recv(4096), "close was refused")
    time.sleep(0.5)
    expect(all(page["id"] != created["id"] for page in http_json(CDP_PORT, "/json")), "closed page is still listed")


@check
def refuses_foreign_hosts_and_origins():
    if not REFERENCE:
        with socket.create_connection(("127.0.0.1", CDP_PORT)) as raw:
            raw.sendall(b"GET /json/version HTTP/1.1\r\nHost: evil.example\r\nConnection: close\r\n\r\n")
            expect(b" 403 " in raw.recv(4096), "a request for another host name was answered")
    url = http_json(CDP_PORT, "/json/version")["webSocketDebuggerUrl"]
    try:
        connection = Connection(url, origin="http://evil.example")
        connection.call("Browser.getVersion", timeout=3)
    except (ProtocolError, OSError, TimeoutError):
        return
    raise AssertionError("a WebSocket from a web origin was accepted")


@check
def unknown_method_is_an_error():
    with Page() as page:
        expect("wasn't found" in page.cdp.error("Nonsense.method", standard=False), "wrong error text")
        if not REFERENCE:
            # Not implemented, and so not acknowledged either.
            expect("wasn't found" in page.cdp.error("Fetch.enable"), "Fetch.enable must not be acknowledged")


# -- Runtime --------------------------------------------------------------


@check
def evaluate_values():
    with Page(f"{PAGES}/index.html") as page:
        equal(page.evaluate("1 + 1"), 2, "number")
        equal(page.evaluate("'a' + 'b'"), "ab", "string")
        equal(page.evaluate("({a: [1, {b: null}]})"), {"a": [1, {"b": None}]}, "object by value")
        equal(page.evaluate("document.title"), "CDP test page", "title")
        result = page.cdp.call("Runtime.evaluate", {"expression": "0 / 0"})["result"]
        equal(result.get("unserializableValue"), "NaN", "NaN")
        result = page.cdp.call("Runtime.evaluate", {"expression": "123n"})["result"]
        equal(result.get("unserializableValue"), "123n", "bigint")
        result = page.cdp.call("Runtime.evaluate", {"expression": "undefined"})["result"]
        equal(result["type"], "undefined", "undefined")
        result = page.cdp.call("Runtime.evaluate", {"expression": "document.body"})["result"]
        equal((result["type"], result.get("subtype")), ("object", "node"), "node handle")
        expect(result.get("objectId"), "node has no objectId")


@check
def evaluate_promises_and_exceptions():
    with Page(f"{PAGES}/index.html") as page:
        equal(page.evaluate("new Promise(resolve => setTimeout(() => resolve({done: 7}), 50))"), {"done": 7}, "awaited promise")
        equal(page.evaluate("Promise.resolve('x')"), "x", "resolved promise")
        equal(page.evaluate("[1, 2, 3]", awaitPromise=True), [1, 2, 3], "non-promise with awaitPromise")
        result = page.cdp.call("Runtime.evaluate", {"expression": "null.x", "returnByValue": True})
        details = result.get("exceptionDetails")
        expect(details, "no exceptionDetails for a throwing expression")
        equal(details["exception"].get("subtype"), "error", "exception subtype")
        expect("TypeError" in details["exception"].get("description", ""), details)
        result = page.cdp.call("Runtime.evaluate", {"expression": "Promise.reject(new RangeError('no'))", "awaitPromise": True})
        expect("RangeError" in result.get("exceptionDetails", {}).get("exception", {}).get("description", ""), result)


@check
def call_function_on():
    with Page(f"{PAGES}/index.html") as page:
        contexts = page.cdp.take_events("Runtime.executionContextCreated")
        body = page.handle("document.body")
        result = page.cdp.call("Runtime.callFunctionOn", {
            "objectId": body,
            "functionDeclaration": "function(a, b, c) { return [this.tagName, a, b.id, c] }\n//# sourceURL=test\n",
            "arguments": [{"value": 5}, {"objectId": page.handle("document.getElementById('box')")}, {"unserializableValue": "-0"}],
            "returnByValue": True,
        })
        value = result["result"]["value"]
        equal(value[:3], ["BODY", 5, "box"], "this and arguments")
        expect(value[3] == 0, value)
        main = page.evaluate("1")  # make sure contexts have been announced
        del main
        page.cdp.drain()
        contexts += page.cdp.take_events("Runtime.executionContextCreated")
        default = [c["context"] for c in contexts if c["context"]["auxData"]["frameId"] == page.target and c["context"]["auxData"]["isDefault"]]
        expect(default, f"no default context for the main frame in {contexts}")
        result = page.cdp.call("Runtime.callFunctionOn", {
            "executionContextId": default[-1]["id"],
            "functionDeclaration": "async function(x) { await null; return location.pathname + x }",
            "arguments": [{"value": "!"}],
            "returnByValue": True,
            "awaitPromise": True,
        })
        equal(result["result"]["value"], "/index.html!", "function in a context, awaited")
        properties = page.cdp.call("Runtime.getProperties", {"objectId": page.handle("({a: 1, b: 'two'})"), "ownProperties": True})["result"]
        equal({p["name"]: p["value"].get("value") for p in properties if p["name"] in "ab"}, {"a": 1, "b": "two"}, "properties")
        page.cdp.call("Runtime.releaseObject", {"objectId": body})


@check
def contexts_and_isolated_worlds():
    with Page() as page:
        page.cdp.call("Page.addScriptToEvaluateOnNewDocument", {"source": "window.injected = 'yes'", "worldName": "helper"})
        page.cdp.call("Page.addScriptToEvaluateOnNewDocument", {"source": "window.inPage = 'yes'"})
        page.goto(f"{PAGES}/index.html")
        equal(page.evaluate("[window.inPage, window.injected]"), ["yes", None], "scripts on new document, seen from the page")
        page.cdp.drain()
        created = [c["context"] for c in page.cdp.take_events("Runtime.executionContextCreated")]
        frames = page.cdp.call("Page.getFrameTree")["frameTree"]
        child = frames["childFrames"][0]["frame"]["id"]
        for frame in (page.target, child):
            kinds = sorted((c["auxData"]["type"], c["name"]) for c in created if c["auxData"]["frameId"] == frame)
            expect(("default", "") in kinds and ("isolated", "helper") in kinds, f"contexts of frame {frame}: {kinds}")
            helper = [c["id"] for c in created if c["auxData"]["frameId"] == frame and c["name"] == "helper"][-1]
            value = page.cdp.call("Runtime.evaluate", {"expression": "[window.injected, typeof window.inPage, typeof window.events + typeof window.innerClicks]", "contextId": helper, "returnByValue": True})
            equal(value["result"]["value"], ["yes", "undefined", "undefinedundefined"], f"the helper world of frame {frame}")
        world = page.cdp.call("Page.createIsolatedWorld", {"frameId": child, "worldName": "another"})["executionContextId"]
        value = page.cdp.call("Runtime.evaluate", {"expression": "document.title", "contextId": world, "returnByValue": True})
        equal(value["result"]["value"], "Child frame", "evaluate in a world of the child frame")
        expect("Cannot find context" in page.cdp.error("Runtime.evaluate", {"expression": "1", "contextId": 987654}), "bad context id")


@check
def bindings():
    with Page() as page:
        page.cdp.call("Runtime.addBinding", {"name": "report"})
        page.goto(f"{PAGES}/index.html")
        page.evaluate("report('first'); report(JSON.stringify({n: 2}))")
        first = page.cdp.wait_event("Runtime.bindingCalled")
        equal((first["name"], first["payload"]), ("report", "first"), "binding call")
        equal(page.cdp.wait_event("Runtime.bindingCalled")["payload"], '{"n":2}', "second call")
        page.cdp.drain()
        equal(page.cdp.take_events("Runtime.consoleAPICalled"), [], "binding calls must not show up as console messages")


@check
def console_and_exceptions():
    with Page() as page:
        page.goto(f"{PAGES}/second.html")
        message = page.cdp.wait_event("Runtime.consoleAPICalled")
        equal(message["type"], "log", "console type")
        equal([a.get("value") for a in message["args"][:2]], ["second loaded", 42], "console arguments")
        equal(message["args"][2]["type"], "object", "object argument")
        page.cdp.call("Runtime.evaluate", {"expression": "setTimeout(() => { throw new Error('late failure') }, 0)"})
        thrown = page.cdp.wait_event("Runtime.exceptionThrown")
        expect("late failure" in thrown["exceptionDetails"]["exception"]["description"], thrown)
        page.cdp.call("Runtime.evaluate", {"expression": "console.warn('careful')"})
        equal(page.cdp.wait_event("Runtime.consoleAPICalled")["type"], "warning", "warning type")


# -- Page -----------------------------------------------------------------


@check
def frame_tree():
    with Page(f"{PAGES}/index.html") as page:
        tree = page.cdp.call("Page.getFrameTree")["frameTree"]
        equal(tree["frame"]["id"], page.target, "main frame id is the target id")
        equal(tree["frame"]["url"], f"{PAGES}/index.html", "main frame url")
        equal(len(tree["childFrames"]), 1, "child frames")
        child = tree["childFrames"][0]["frame"]
        equal(child["parentId"], page.target, "child's parent")
        equal(child["url"], f"{PAGES}/frame.html", "child url")
        equal(child["name"], "child", "child name")


@check
def navigation_events():
    with Page() as page:
        result = page.cdp.call("Page.navigate", {"url": f"{PAGES}/second.html"})
        expect(result.get("loaderId"), result)
        equal(result["frameId"], page.target, "frame of the navigation")
        navigated = page.cdp.wait_event("Page.frameNavigated", lambda p: p["frame"]["id"] == page.target and "second" in p["frame"]["url"])
        equal(navigated["frame"]["loaderId"], result["loaderId"], "loader of the committed frame")
        names = []
        while "networkIdle" not in names:
            event = page.cdp.wait_event("Page.lifecycleEvent", lambda p: p["loaderId"] == result["loaderId"])
            names.append(event["name"])
        for name in ("init", "DOMContentLoaded", "load", "networkIdle"):
            expect(name in names, f"no {name} lifecycle event in {names}")
        expect(names.index("init") < names.index("DOMContentLoaded") < names.index("load") < names.index("networkIdle"), names)
        page.cdp.wait_event("Page.loadEventFired")

        request = page.cdp.wait_event("Network.requestWillBeSent", lambda p: p["type"] == "Document" and "second" in p["request"]["url"])
        equal(request["requestId"], result["loaderId"], "the document request has the loader's id")
        equal(request["request"]["method"], "GET", "method")
        response = page.cdp.wait_event("Network.responseReceived", lambda p: p["requestId"] == request["requestId"])
        equal(response["response"]["status"], 200, "status")
        expect("text/html" in response["response"]["mimeType"], response["response"])
        page.cdp.wait_event("Network.loadingFinished", lambda p: p["requestId"] == request["requestId"])
        body = page.cdp.call("Network.getResponseBody", {"requestId": request["requestId"]})
        expect("<h1 id=\"heading\">Second</h1>" in body["body"], "response body")
        image = page.cdp.wait_event("Network.requestWillBeSent", lambda p: p["request"]["url"].endswith("pixel.png"))
        equal(image["type"], "Image", "image request type")


@check
def navigation_failures():
    with Page() as page:
        result = page.cdp.call("Page.navigate", {"url": f"http://127.0.0.1:{OTHER_PORT + 7}/nothing"})
        equal(result.get("errorText"), "net::ERR_CONNECTION_REFUSED", "refused connection")
        result = page.cdp.call("Page.navigate", {"url": "http://no-such-host.invalid/"})
        equal(result.get("errorText"), "net::ERR_NAME_NOT_RESOLVED", "unknown host")
        page.goto(f"{PAGES}/second.html")
        equal(page.evaluate("document.title"), "Second page", "page still usable afterwards")


@check
def cross_origin_navigation():
    with Page() as page:
        page.cdp.call("Page.addScriptToEvaluateOnNewDocument", {"source": "window.mark = (window.mark || 0) + 1"})
        page.cdp.call("Runtime.addBinding", {"name": "tell"})
        page.cdp.call("Network.setExtraHTTPHeaders", {"headers": {"X-Zebkit-Test": "1"}})
        page.goto(f"{PAGES}/index.html")
        equal(page.evaluate("mark"), 1, "script before first navigation")
        for url, origin in ((f"{OTHER}/second.html", OTHER), (f"{PAGES}/index.html", PAGES), (f"{OTHER}/index.html", OTHER)):
            page.cdp.take_events("Runtime.executionContextCreated")
            page.goto(url)
            equal(page.evaluate("location.origin"), origin, "origin after navigation")
            equal(page.evaluate("mark"), 1, "new-document script after changing origin")
            page.evaluate("tell('still here')")
            equal(page.cdp.wait_event("Runtime.bindingCalled")["payload"], "still here", "binding after changing origin")
            page.cdp.drain()
            contexts = [c["context"] for c in page.cdp.take_events("Runtime.executionContextCreated")]
            expect(any(c["auxData"]["frameId"] == page.target and c["auxData"]["isDefault"] for c in contexts),
                   f"no new default context announced for {url}: {contexts}")
        equal(page.cdp.call("Page.getFrameTree")["frameTree"]["frame"]["id"], page.target, "main frame id is stable")


@check
def history():
    with Page() as page:
        page.goto(f"{PAGES}/index.html")
        page.goto(f"{PAGES}/second.html")
        entries = page.cdp.call("Page.getNavigationHistory")
        urls = [entry["url"] for entry in entries["entries"]]
        expect(urls[-2:] == [f"{PAGES}/index.html", f"{PAGES}/second.html"], urls)
        equal(entries["currentIndex"], len(urls) - 1, "current index")
        page.cdp.take_events("Page.frameNavigated")
        page.cdp.call("Page.navigateToHistoryEntry", {"entryId": entries["entries"][-2]["id"]})
        page.cdp.wait_event("Page.frameNavigated", lambda p: p["frame"]["url"].endswith("/index.html"))
        equal(page.evaluate("location.pathname"), "/index.html", "went back")
        page.cdp.call("Page.reload")
        page.cdp.wait_event("Page.lifecycleEvent", lambda p: p["name"] == "load")
        equal(page.evaluate("location.pathname"), "/index.html", "reloaded")
        result = page.cdp.call("Page.navigate", {"url": f"{PAGES}/index.html#part"})
        expect("loaderId" not in result, f"a fragment navigation has no new loader: {result}")
        page.cdp.wait_event("Page.navigatedWithinDocument")
        equal(page.evaluate("location.hash"), "#part", "fragment")


@check
def screenshot_and_metrics():
    with Page(f"{PAGES}/index.html") as page:
        page.cdp.call("Emulation.setDeviceMetricsOverride", {"width": 800, "height": 600, "deviceScaleFactor": 1, "mobile": False})
        equal(page.evaluate("[innerWidth, innerHeight]"), [800, 600], "viewport after override")
        data = base64.b64decode(page.cdp.call("Page.captureScreenshot", {"format": "png"})["data"])
        expect(data.startswith(b"\x89PNG\r\n\x1a\n"), "not a PNG")
        equal((int.from_bytes(data[16:20], "big"), int.from_bytes(data[20:24], "big")), (800, 600), "screenshot size")
        clip = {"x": 100, "y": 80, "width": 200, "height": 50, "scale": 1}
        data = base64.b64decode(page.cdp.call("Page.captureScreenshot", {"format": "png", "clip": clip})["data"])
        equal((int.from_bytes(data[16:20], "big"), int.from_bytes(data[20:24], "big")), (200, 50), "clipped screenshot size")
        data = base64.b64decode(page.cdp.call("Page.captureScreenshot", {"format": "jpeg", "quality": 70})["data"])
        expect(data.startswith(b"\xff\xd8\xff"), "not a JPEG")
        data = base64.b64decode(page.cdp.call("Page.captureScreenshot", {"format": "webp"})["data"])
        expect(data[:4] == b"RIFF" and data[8:12] == b"WEBP", "not a WebP")
        metrics = page.cdp.call("Page.getLayoutMetrics")
        equal(metrics["cssVisualViewport"]["clientWidth"], page.evaluate("document.documentElement.clientWidth"), "visual viewport width")
        expect(metrics["cssContentSize"]["height"] >= 3040, metrics["cssContentSize"])


@check
def dialogs():
    with Page(f"{PAGES}/dialogs.html") as page:
        page.cdp.post("Runtime.evaluate", {"expression": "document.getElementById('alert').click()"})
        opening = page.cdp.wait_event("Page.javascriptDialogOpening")
        equal((opening["type"], opening["message"]), ("alert", "hello"), "alert")
        page.cdp.call("Page.handleJavaScriptDialog", {"accept": True})
        equal(page.evaluate("window.afterAlert"), True, "script continued after the alert")

        page.cdp.post("Runtime.evaluate", {"expression": "document.getElementById('prompt').click()"})
        opening = page.cdp.wait_event("Page.javascriptDialogOpening")
        equal((opening["type"], opening.get("defaultPrompt")), ("prompt", "nobody"), "prompt")
        page.cdp.call("Page.handleJavaScriptDialog", {"accept": True, "promptText": "somebody"})
        equal(page.evaluate("window.answer"), "somebody", "prompt answer")

        page.cdp.post("Runtime.evaluate", {"expression": "document.getElementById('confirm').click()"})
        page.cdp.wait_event("Page.javascriptDialogOpening")
        page.cdp.call("Page.handleJavaScriptDialog", {"accept": False})
        equal(page.evaluate("window.confirmed"), False, "dismissed confirm")


# -- Input ----------------------------------------------------------------


@check
def mouse():
    with Page(f"{PAGES}/index.html") as page:
        page.click(200, 105)
        equal(page.evaluate("clicks"), 1, "clicks")
        equal(page.evaluate("events"), ["mousedown:0@200,105", "mouseup:0@200,105", "click:0@200,105"], "trusted mouse events")
        page.evaluate("events.length = 0")
        page.click(200, 105)
        page.mouse("mousePressed", 200, 105, button="left", clickCount=2)
        page.mouse("mouseReleased", 200, 105, button="left", clickCount=2)
        expect("dblclick:0@200,105" in page.evaluate("events"), "no double click")
        page.evaluate("events.length = 0")
        page.click(200, 105, button="right")
        expect("contextmenu:2@200,105" in page.evaluate("events"), "no context menu event")

        page.mouse("mouseWheel", 300, 300, deltaX=0, deltaY=400)
        deadline = time.monotonic() + 3
        while page.evaluate("scrollY") < 300 and time.monotonic() < deadline:
            time.sleep(0.05)
        near(page.evaluate("scrollY"), 400, "scroll position after the wheel", tolerance=2)


@check
def keyboard():
    with Page(f"{PAGES}/index.html") as page:
        page.click(150, 170)
        equal(page.evaluate("document.activeElement.id"), "name", "focus follows the click")
        for character in "Hi 9":
            page.key(character, text=character)
        equal(page.evaluate("document.getElementById('name').value"), "Hi 9", "typed text")
        events = page.evaluate("events")
        expect("keydown:H:KeyH" in events and "keyup:i:KeyI" in events and "input" in events, events)
        page.key("Backspace")
        equal(page.evaluate("document.getElementById('name').value"), "Hi ", "backspace")
        page.cdp.call("Input.insertText", {"text": "世界 ✓"})
        equal(page.evaluate("document.getElementById('name').value"), "Hi 世界 ✓", "inserted text")
        page.key("a", modifiers=2)
        page.key("Delete")
        equal(page.evaluate("document.getElementById('name').value"), "", "select all and delete")
        page.key("Tab")
        equal(page.evaluate("document.activeElement.id"), "area", "tab moves focus")
        page.key("x", text="x")
        page.key("Enter", text="\r")
        page.key("y", text="y")
        equal(page.evaluate("document.getElementById('area').value"), "x\ny", "enter in a textarea")


# -- DOM ------------------------------------------------------------------


@check
def dom_geometry():
    with Page(f"{PAGES}/index.html") as page:
        box = page.handle("document.getElementById('box')")
        quads = page.cdp.call("DOM.getContentQuads", {"objectId": box})["quads"]
        equal(quads, [[100, 80, 300, 80, 300, 130, 100, 130]], "quads of #box")
        model = page.cdp.call("DOM.getBoxModel", {"objectId": box})["model"]
        equal((model["width"], model["height"], model["border"][:2]), (200, 50, [100, 80]), "box model of #box")

        # The button inside the frame: frame at 500,80 with a 4px border and
        # 6px padding, button at 30,40 inside it.
        tree = page.cdp.call("Page.getFrameTree")["frameTree"]
        child = tree["childFrames"][0]["frame"]["id"]
        world = page.cdp.call("Page.createIsolatedWorld", {"frameId": child, "worldName": "geometry"})["executionContextId"]
        inner = page.cdp.call("Runtime.evaluate", {"expression": "document.getElementById('inner')", "contextId": world})["result"]["objectId"]
        quad = page.cdp.call("DOM.getContentQuads", {"objectId": inner})["quads"][0]
        equal(quad[:2], [540, 130], "frame offset added to the quads of a node in a frame")
        page.click(quad[0] + 20, quad[1] + 10)
        equal(page.evaluate("document.getElementById('child').contentWindow.innerClicks"), 1, "click landed in the frame")

        far = page.handle("document.getElementById('far')")
        page.cdp.call("DOM.scrollIntoViewIfNeeded", {"objectId": far})
        quad = page.cdp.call("DOM.getContentQuads", {"objectId": far})["quads"][0]
        expect(0 <= quad[1] <= 720, f"#far is not in the viewport after scrolling: {quad}")
        page.click(quad[0] + 10, quad[1] + 10)
        equal(page.evaluate("window.farClicked"), True, "click after scrolling")
        detached = page.handle("document.createElement('p')")
        equal(page.cdp.call("DOM.getContentQuads", {"objectId": detached})["quads"], [], "a node that is not rendered has no quads")
        expect("box model" in page.cdp.error("DOM.getBoxModel", {"objectId": detached}), "box model of a node that is not rendered")


@check
def dom_nodes():
    with Page(f"{PAGES}/index.html") as page:
        owner = page.handle("document.getElementById('child')")
        described = page.cdp.call("DOM.describeNode", {"objectId": owner})["node"]
        equal((described["nodeName"], described["localName"]), ("IFRAME", "iframe"), "described node")
        child = page.cdp.call("Page.getFrameTree")["frameTree"]["childFrames"][0]["frame"]["id"]
        equal(described.get("frameId"), child, "frame held by the iframe element")
        resolved = page.cdp.call("DOM.resolveNode", {"backendNodeId": described["backendNodeId"]})["object"]
        same = page.cdp.call("Runtime.callFunctionOn", {
            "objectId": resolved["objectId"], "functionDeclaration": "function(other) { return this === other }",
            "arguments": [{"objectId": owner}], "returnByValue": True})
        equal(same["result"]["value"], True, "resolved node is the described one")
        frame_owner = page.cdp.call("DOM.getFrameOwner", {"frameId": child})
        resolved = page.cdp.call("DOM.resolveNode", {"backendNodeId": frame_owner["backendNodeId"]})["object"]
        same = page.cdp.call("Runtime.callFunctionOn", {
            "objectId": resolved["objectId"], "functionDeclaration": "function() { return this.id }", "returnByValue": True})
        equal(same["result"]["value"], "child", "frame owner")

        root = page.cdp.call("DOM.getDocument", {"depth": -1, "pierce": True})["root"]
        equal(root["nodeName"], "#document", "document node")

        def find(node, name):
            if node.get("nodeName") == name:
                return node
            for key in ("children", "shadowRoots"):
                for sub in node.get(key, []):
                    found = find(sub, name)
                    if found:
                        return found
            if "contentDocument" in node:
                return find(node["contentDocument"], name)
            return None

        textarea = find(root, "TEXTAREA")
        expect(textarea and textarea["backendNodeId"], "the whole tree was not returned")
        expect("id" in textarea["attributes"], textarea)
        found = page.cdp.call("DOM.querySelector", {"nodeId": root["nodeId"], "selector": "#link"})
        html = page.cdp.call("DOM.getOuterHTML", {"nodeId": found["nodeId"]})["outerHTML"]
        equal(html, '<a id="link" href="second.html?from=link">Second page</a>', "outer HTML")
        model = page.cdp.call("DOM.getBoxModel", {"backendNodeId": textarea["backendNodeId"]})["model"]
        equal(model["border"][:2], [100, 200], "box model by backend node id")
        page.cdp.call("DOM.focus", {"backendNodeId": textarea["backendNodeId"]})
        equal(page.evaluate("document.activeElement.id"), "area", "DOM.focus")


@check
def file_input():
    with Page(f"{PAGES}/index.html") as page, tempfile.TemporaryDirectory() as directory:
        paths = []
        for name, size in (("one.txt", 3), ("two.bin", 2048)):
            path = os.path.join(directory, name)
            with open(path, "wb") as handle:
                handle.write(b"x" * size)
            paths.append(path)
        page.cdp.call("DOM.setFileInputFiles", {"objectId": page.handle("document.getElementById('file')"), "files": paths})
        deadline = time.monotonic() + 3
        while page.evaluate("window.files") is None and time.monotonic() < deadline:
            time.sleep(0.05)
        equal(page.evaluate("window.files"), ["one.txt:3", "two.bin:2048"], "selected files")
        expect(page.cdp.error("DOM.setFileInputFiles", {"objectId": page.handle("document.getElementById('box')"), "files": paths}),
               "setting files on a button must fail")


# -- Network and storage ---------------------------------------------------


@check
def cookies_and_headers():
    with Page(f"{PAGES}/index.html") as page:
        page.cdp.call("Network.setCookie", {"name": "session", "value": "abc", "url": PAGES, "httpOnly": True})
        page.cdp.call("Network.setCookies", {"cookies": [{"name": "theme", "value": "dark", "domain": "127.0.0.1", "path": "/"}]})
        cookies = {c["name"]: c for c in page.cdp.call("Network.getCookies")["cookies"]}
        equal(cookies["session"]["value"], "abc", "cookie value")
        equal(cookies["session"]["httpOnly"], True, "httpOnly")
        equal(page.evaluate("document.cookie"), "theme=dark", "what script sees")
        page.cdp.call("Network.deleteCookies", {"name": "theme", "domain": "127.0.0.1"})
        equal(page.evaluate("document.cookie"), "", "after deleting")
        everything = page.cdp.call("Storage.getCookies")["cookies"]
        expect(any(c["name"] == "session" for c in everything), everything)
        page.cdp.call("Network.clearBrowserCookies")
        equal(page.cdp.call("Network.getAllCookies")["cookies"], [], "after clearing")

        page.cdp.call("Network.setUserAgentOverride", {"userAgent": "ZebkitTest/1.0"})
        page.cdp.call("Network.setExtraHTTPHeaders", {"headers": {"X-Extra": "yes"}})
        page.cdp.take_events("Network.requestWillBeSent")
        page.goto(f"{PAGES}/second.html")
        equal(page.evaluate("navigator.userAgent"), "ZebkitTest/1.0", "user agent seen by script")
        request = page.cdp.wait_event("Network.requestWillBeSent", lambda p: p["type"] == "Document")
        headers = {k.lower(): v for k, v in request["request"]["headers"].items()}
        equal(headers.get("x-extra"), "yes", "extra header on the request")


# -- Browser-level ---------------------------------------------------------


@check
def targets_and_sessions():
    browser = Connection(http_json(CDP_PORT, "/json/version")["webSocketDebuggerUrl"])
    try:
        version = browser.call("Browser.getVersion")
        expect(version["product"].startswith("Chrome/" if REFERENCE else "Zebkit/") and version["userAgent"], version)
        browser.call("Target.setDiscoverTargets", {"discover": True})
        browser.wait_event("Target.targetCreated", lambda p: p["targetInfo"]["type"] == "page")
        browser.events.clear()

        target = browser.call("Target.createTarget", {"url": f"{PAGES}/second.html"})["targetId"]
        created = browser.wait_event("Target.targetCreated", lambda p: p["targetInfo"]["targetId"] == target)
        equal(created["targetInfo"]["type"], "page", "created target type")
        session = browser.call("Target.attachToTarget", {"targetId": target, "flatten": True})["sessionId"]
        attached = browser.wait_event("Target.attachedToTarget", lambda p: p["sessionId"] == session)
        equal(attached["targetInfo"]["targetId"], target, "attached target")
        browser.wait_event("Target.targetInfoChanged", lambda p: p["targetInfo"]["url"].endswith("second.html"))

        browser.call("Page.enable", session=session)
        browser.call("Runtime.enable", session=session)
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            result = browser.call("Runtime.evaluate", {"expression": "document.readyState + ':' + document.title", "returnByValue": True}, session=session)
            if result["result"]["value"] == "complete:Second page":
                break
            time.sleep(0.1)
        equal(result["result"]["value"], "complete:Second page", "evaluate through a session")
        infos = browser.call("Target.getTargets")["targetInfos"]
        expect(any(info["targetId"] == target and info["attached"] for info in infos), infos)
        equal(browser.call("Target.getTargetInfo", session=session)["targetInfo"]["targetId"], target, "target info on a session")

        browser.call("Target.closeTarget", {"targetId": target})
        browser.wait_event("Target.detachedFromTarget", lambda p: p["sessionId"] == session)
        browser.wait_event("Target.targetDestroyed", lambda p: p["targetId"] == target)
        expect("not found" in browser.error("Runtime.evaluate", {"expression": "1"}, session=session), "closed session still answers")
    finally:
        browser.close()


@check
def auto_attach_holds_the_page():
    browser = Connection(http_json(CDP_PORT, "/json/version")["webSocketDebuggerUrl"])
    try:
        browser.call("Target.setAutoAttach", {"autoAttach": True, "waitForDebuggerOnStart": True, "flatten": True})
        browser.events.clear()
        target = browser.call("Target.createTarget", {"url": f"{PAGES}/second.html"})["targetId"]
        attached = browser.wait_event("Target.attachedToTarget", lambda p: p["targetInfo"]["targetId"] == target)
        equal(attached["waitingForDebugger"], True, "new page waits for the client")
        session = attached["sessionId"]
        browser.call("Page.enable", session=session)
        browser.call("Runtime.enable", session=session)
        browser.call("Page.addScriptToEvaluateOnNewDocument", {"source": "window.early = document.readyState"}, session=session)
        time.sleep(0.5)
        if not REFERENCE:
            # Chrome holds the page with the address already set.
            value = browser.call("Runtime.evaluate", {"expression": "location.href", "returnByValue": True}, session=session)
            equal(value["result"]["value"], "about:blank", "the page has not started loading")
        browser.call("Runtime.runIfWaitingForDebugger", session=session)
        browser.wait_event("Page.loadEventFired", session=session)
        value = browser.call("Runtime.evaluate", {"expression": "[document.title, window.early]", "returnByValue": True}, session=session)
        if not REFERENCE:
            # Chrome has started on the address by the time it holds the page.
            equal(value["result"]["value"], ["Second page", "loading"], "script installed before the first load ran")
        browser.call("Target.closeTarget", {"targetId": target})

        # The way client libraries do it: an empty page, set up, then sent somewhere.
        target = browser.call("Target.createTarget", {"url": "about:blank"})["targetId"]
        attached = browser.wait_event("Target.attachedToTarget", lambda p: p["targetInfo"]["targetId"] == target)
        equal(attached["waitingForDebugger"], True, "new empty page waits for the client")
        session = attached["sessionId"]
        browser.call("Page.enable", session=session)
        browser.call("Page.addScriptToEvaluateOnNewDocument", {"source": "window.early = document.readyState"}, session=session)
        browser.call("Runtime.runIfWaitingForDebugger", session=session)
        browser.call("Page.navigate", {"url": f"{PAGES}/second.html"}, session=session)
        browser.wait_event("Page.loadEventFired", session=session)
        value = browser.call("Runtime.evaluate", {"expression": "[document.title, window.early]", "returnByValue": True}, session=session)
        equal(value["result"]["value"], ["Second page", "loading"], "script installed while the page was held")
        browser.call("Target.closeTarget", {"targetId": target})
    finally:
        browser.close()


@check
def browser_contexts_are_isolated():
    browser = Connection(http_json(CDP_PORT, "/json/version")["webSocketDebuggerUrl"])
    try:
        context = browser.call("Target.createBrowserContext")["browserContextId"]
        expect(context in browser.call("Target.getBrowserContexts")["browserContextIds"], "context not listed")
        inside = browser.call("Target.createTarget", {"url": f"{PAGES}/second.html", "browserContextId": context})["targetId"]
        outside = browser.call("Target.createTarget", {"url": f"{PAGES}/second.html"})["targetId"]
        sessions = {}
        for name, target in (("inside", inside), ("outside", outside)):
            sessions[name] = browser.call("Target.attachToTarget", {"targetId": target, "flatten": True})["sessionId"]
        deadline = time.monotonic() + 10
        for session in sessions.values():
            while time.monotonic() < deadline:
                state = browser.call("Runtime.evaluate", {"expression": "document.readyState + location.pathname", "returnByValue": True}, session=session)
                if state["result"]["value"] == "complete/second.html":
                    break
                time.sleep(0.1)
        browser.call("Runtime.evaluate", {"expression": "document.cookie = 'where=inside; path=/'"}, session=sessions["inside"])
        seen = browser.call("Runtime.evaluate", {"expression": "document.cookie", "returnByValue": True}, session=sessions["outside"])
        equal(seen["result"]["value"], "", "cookie leaked out of its context")
        cookies = browser.call("Storage.getCookies", {"browserContextId": context})["cookies"]
        equal([c["name"] for c in cookies], ["where"], "cookies of the context")
        browser.call("Target.closeTarget", {"targetId": outside})
        browser.call("Target.disposeBrowserContext", {"browserContextId": context})
        time.sleep(0.3)
        expect(all(info["targetId"] != inside for info in browser.call("Target.getTargets")["targetInfos"]), "page outlived its context")
    finally:
        browser.close()


@check
def popups_become_targets():
    with Page(f"{PAGES}/dialogs.html") as page:
        page.browser.call("Target.setDiscoverTargets", {"discover": True})
        page.browser.events.clear()
        quad = page.cdp.call("DOM.getContentQuads", {"objectId": page.handle("document.getElementById('open')")})["quads"][0]
        page.click(quad[0] + 5, quad[1] + 5)
        created = page.browser.wait_event("Target.targetCreated")["targetInfo"]
        equal(created.get("openerId"), page.target, "opener of the popup")
        changed = page.browser.wait_event("Target.targetInfoChanged", lambda p: p["targetInfo"]["targetId"] == created["targetId"] and "popup=1" in p["targetInfo"]["url"])
        expect(changed, "popup did not navigate")
        equal(page.evaluate("!!window.popup"), True, "window.open returned a window")
        page.browser.call("Target.closeTarget", {"targetId": created["targetId"]})


# -- things that go wrong ---------------------------------------------------


@zebkit_only
def refuses_requests_from_web_pages():
    # What script in a web page could send to this port must have no effect.
    import urllib.error
    import urllib.request

    def status(path, method="GET", headers=None):
        request = urllib.request.Request(f"http://127.0.0.1:{CDP_PORT}{path}", method=method, headers=headers or {})
        try:
            with urllib.request.urlopen(request, timeout=10) as response:
                return response.status
        except urllib.error.HTTPError as error:
            return error.code

    before = len(http_json(CDP_PORT, "/json/list"))
    equal(status("/json/new?about:blank"), 405, "GET /json/new")
    equal(status("/json/list", headers={"Origin": "http://evil.example"}), 403, "a request with a web origin")
    equal(status("/json/new?about:blank", "PUT", {"Origin": "http://evil.example"}), 403, "PUT with a web origin")
    # A page the check before this one closed may still have been listed.
    expect(len(http_json(CDP_PORT, "/json/list")) <= before, "a refused request opened a page")
    try:
        connection = Connection(f"ws://127.0.0.1:{CDP_PORT}/devtools/browser/0000")
        connection.call("Browser.getVersion", timeout=3)
    except (ProtocolError, OSError, TimeoutError):
        return
    raise AssertionError("a browser socket with the wrong id was accepted")


@check
def hostile_ids_are_harmless():
    with Page(f"{PAGES}/index.html") as page:
        body = page.cdp.call("DOM.describeNode", {"objectId": page.handle("document.body")})["node"]["backendNodeId"]
        for value in (2**31 - 1, 2**31 - 2, -1, 0, -(2**31)):
            page.cdp.error("DOM.describeNode", {"backendNodeId": value})
            page.cdp.error("DOM.resolveNode", {"nodeId": value})
            page.cdp.error("DOM.resolveNode", {"backendNodeId": body, "executionContextId": value or 987654})
            if value:
                page.cdp.error("Runtime.evaluate", {"expression": "1", "contextId": value})
            page.cdp.error("Runtime.callFunctionOn", {"functionDeclaration": "function(){}", "objectId": f'{{"injectedScriptId":{value},"id":1}}'})
            page.cdp.error("DOM.getBoxModel", {"objectId": f'{{"injectedScriptId":{value},"id":1}}'})
        page.cdp.error("Target.attachToTarget", {"targetId": ""})
        page.cdp.error("Page.createIsolatedWorld", {"frameId": ""})
        equal(page.evaluate("1 + 1"), 2, "the page after the bad ids")


@check
def input_answers_keep_their_order():
    # Clients send press and release together and track the buttons by the
    # order of the answers.
    with Page(f"{PAGES}/index.html") as page:
        sent = []
        for _ in range(25):
            sent.append(page.cdp.post("Input.dispatchMouseEvent", {"type": "mouseMoved", "x": 200, "y": 105}))
            sent.append(page.cdp.post("Input.dispatchMouseEvent", {"type": "mousePressed", "x": 200, "y": 105, "button": "left", "clickCount": 1}))
            sent.append(page.cdp.post("Input.dispatchMouseEvent", {"type": "mouseReleased", "x": 200, "y": 105, "button": "left", "clickCount": 1}))
        page.cdp.events.clear()
        page.cdp.reply(sent[-1])
        answered = [message["id"] for message in page.cdp.events if message.get("id") in sent] + [sent[-1]]
        equal(answered, sent, "order of the answers")
        equal(page.evaluate("window.clicks"), 25, "clicks")


@check
def stop_loading_ends_the_navigation():
    with Page(f"{PAGES}/index.html") as page:
        # An address that does not answer: the navigation stays pending.
        pending = page.cdp.post("Page.navigate", {"url": "http://10.255.255.1/"})
        time.sleep(0.3)
        page.cdp.call("Page.stopLoading")
        result = page.cdp.reply(pending, timeout=5)
        equal(result["result"].get("errorText"), "net::ERR_ABORTED", "answer to the stopped navigation")
        page.goto(f"{PAGES}/second.html")
        equal(page.evaluate("document.title"), "Second page", "the page after stopping")


@check
def crash_and_recover():
    with Page(f"{PAGES}/index.html") as page:
        waiting = page.cdp.post("Runtime.evaluate", {"expression": "new Promise(() => {})", "awaitPromise": True})
        crash = page.cdp.post("Page.crash")
        page.cdp.wait_event("Inspector.targetCrashed", timeout=10)
        if not REFERENCE:
            # Chrome leaves these three unanswered. Here a client is told.
            expect("error" not in page.cdp.reply(crash, timeout=5), "Page.crash failed")
            answer = page.cdp.reply(waiting, timeout=5)
            expect("error" in answer, f"a command the dead page was running was answered with {answer}")
            equal(page.cdp.error("Runtime.evaluate", {"expression": "1"}), "Target crashed", "a command sent to the dead page")
        page.goto(f"{PAGES}/second.html")
        equal(page.evaluate("document.title"), "Second page", "the page after the crash")
        page.cdp.post("Page.crash")
        page.cdp.wait_event("Inspector.targetCrashed", timeout=10)
        page.cdp.take_events("Page.lifecycleEvent")
        page.cdp.post("Page.reload")
        page.cdp.wait_event("Page.lifecycleEvent", lambda p: p["name"] == "load")
        equal(page.evaluate("document.title"), "Second page", "the page reloaded after a second crash")


@zebkit_only
def a_client_that_leaves_blocks_nothing():
    with Page(f"{PAGES}/dialogs.html") as page:
        page.cdp.post("Runtime.evaluate", {"expression": "setTimeout(() => { alert('x'); window.afterAlert = true }, 0)"})
        page.cdp.wait_event("Page.javascriptDialogOpening")
        page.cdp.close()
        # Nobody answered the dialog. The script behind it must go on.
        page.cdp = Connection(f"ws://127.0.0.1:{CDP_PORT}/devtools/page/{page.target}")
        page.cdp.call("Runtime.enable")
        equal(page.cdp.call("Runtime.evaluate", {"expression": "window.afterAlert", "returnByValue": True}, timeout=5)["result"].get("value"), True, "script after the dialog")


@check
def file_input_refuses_other_elements():
    with Page(f"{PAGES}/index.html") as page, tempfile.NamedTemporaryFile(suffix=".txt") as file:
        message = page.cdp.error("DOM.setFileInputFiles", {"objectId": page.handle("document.getElementById('box')"), "files": [file.name]})
        equal(message, "Node is not a file input element", "error for a button")
        equal(page.evaluate("window.clicks"), 0, "clicks on the button")


@check
def viewport_override_is_undone():
    with Page(f"{PAGES}/index.html") as page:
        before = page.evaluate("[innerWidth, innerHeight]")
        page.cdp.call("Emulation.setDeviceMetricsOverride", {"width": 500, "height": 400, "deviceScaleFactor": 1, "mobile": False})
        equal(page.evaluate("[innerWidth, innerHeight]"), [500, 400], "overridden size")
        page.cdp.call("Emulation.clearDeviceMetricsOverride")
        deadline = time.monotonic() + 3
        while page.evaluate("[innerWidth, innerHeight]") != before and time.monotonic() < deadline:
            time.sleep(0.05)
        equal(page.evaluate("[innerWidth, innerHeight]"), before, "size after clearing the override")


# -- what was found by comparing with Chrome -------------------------------


@check
def isolated_worlds_are_separate():
    with Page(f"{PAGES}/index.html") as page:
        world = page.cdp.call("Page.createIsolatedWorld", {"frameId": page.target, "worldName": "apart"})["executionContextId"]

        def in_world(expression):
            result = page.cdp.call("Runtime.evaluate", {"expression": expression, "contextId": world, "returnByValue": True})
            expect("exceptionDetails" not in result, f"{expression} threw in the world: {result}")
            return result["result"].get("value")

        in_world("window.secret = 1")
        equal(page.evaluate("typeof secret + ' ' + typeof events"), "undefined object", "the page does not see the world's variables")
        equal(in_world("typeof secret + ' ' + typeof events"), "number undefined", "the world does not see the page's variables")
        in_world("document.getElementById('box').dataset.seen = 'yes'")
        equal(page.evaluate("document.getElementById('box').dataset.seen"), "yes", "both see the same document")

        # A node is carried from one world to another by its backend id.
        handle = page.cdp.call("Runtime.evaluate", {"expression": "document.getElementById('box')", "contextId": world})["result"]["objectId"]
        backend = page.cdp.call("DOM.describeNode", {"objectId": handle})["node"]["backendNodeId"]

        def call(object_id, function):
            return page.cdp.call("Runtime.callFunctionOn", {"objectId": object_id, "functionDeclaration": function, "returnByValue": True})["result"]["value"]

        main = page.cdp.call("DOM.resolveNode", {"backendNodeId": backend})["object"]["objectId"]
        equal(call(main, "function() { return this.id + ' ' + typeof secret + ' ' + typeof events }"), "box undefined object", "resolved in the main world")
        back = page.cdp.call("DOM.resolveNode", {"backendNodeId": backend, "executionContextId": world})["object"]["objectId"]
        equal(call(back, "function() { return this.id + ' ' + typeof secret + ' ' + typeof events }"), "box number undefined", "resolved in the isolated world")

        equal(page.cdp.call("Page.createIsolatedWorld", {"frameId": page.target, "worldName": "apart"})["executionContextId"], world, "the same name is the same world")
        code, message = page.cdp.failure("Page.createIsolatedWorld", {"frameId": "nope"})
        equal((code, message), (-32602, "No frame for given id found"), "a world for a frame that is not there")

        # A world made this way belongs to its document.
        page.cdp.events.clear()
        page.goto(f"{PAGES}/second.html")
        page.cdp.drain()
        names = sorted(c["context"]["name"] for c in page.cdp.take_events("Runtime.executionContextCreated"))
        equal(names, [""], "contexts of the next document")
        equal(page.cdp.error("Runtime.evaluate", {"expression": "1", "contextId": world}), "Cannot find context with specified id", "the world after its document went")


@check
def bindings_follow_their_world():
    with Page(f"{PAGES}/index.html") as page:
        world = page.cdp.call("Page.createIsolatedWorld", {"frameId": page.target, "worldName": "apart"})["executionContextId"]
        page.cdp.call("Runtime.addBinding", {"name": "everywhere"})
        page.cdp.call("Runtime.addBinding", {"name": "apartOnly", "executionContextName": "apart"})
        equal(page.evaluate("[typeof everywhere, typeof apartOnly]"), ["function", "undefined"], "bindings in the page")
        value = page.cdp.call("Runtime.evaluate", {"expression": "[typeof everywhere, typeof apartOnly]", "contextId": world, "returnByValue": True})
        equal(value["result"]["value"], ["function", "function"], "bindings in the world")
        page.cdp.events.clear()
        page.cdp.call("Runtime.evaluate", {"expression": "apartOnly('a'); everywhere('b')", "contextId": world})
        page.evaluate("everywhere('c')")
        page.cdp.drain()
        calls = [(p["name"], p["payload"], p["executionContextId"] == world) for p in page.cdp.take_events("Runtime.bindingCalled")]
        equal(calls, [("apartOnly", "a", True), ("everywhere", "b", True), ("everywhere", "c", False)], "calls and where they came from")
        page.goto(f"{PAGES}/second.html")
        equal(page.evaluate("[typeof everywhere, typeof apartOnly]"), ["function", "undefined"], "bindings in the next document")


@check
def malformed_messages_are_answered():
    with Page(f"{PAGES}/index.html") as page:
        equal(page.cdp.send_raw("{nope")["error"]["code"], -32700, "text that is not JSON")
        equal(page.cdp.send_raw("[1]")["error"]["code"], -32600, "JSON that is not an object")
        equal(page.cdp.send_raw('{"method": "Page.enable"}')["error"]["code"], -32600, "a message without an id")
        answer = page.cdp.send_raw('{"id": 77001}')
        equal((answer.get("id"), answer["error"]["code"], answer["error"]["message"]), (77001, -32600, "Message must have string 'method' property"), "a message without a method")
        answer = page.cdp.send_raw('{"id": 77002, "method": "Runtime.evaluate", "params": {"expression": "1"}, "sessionId": "nope"}')
        equal((answer["error"]["code"], answer["error"]["message"], "sessionId" in answer), (-32001, "Session with given id not found.", False), "a session that does not exist")
        answer = page.cdp.send_raw('{"id": 77003, "method": "Runtime.evaluate", "params": {}}')
        equal((answer["error"]["code"], answer["error"]["message"]), (-32602, "Invalid parameters"), "a mandatory parameter left out")
        expect(answer["error"]["data"].startswith("Failed to deserialize params.expression - BINDINGS: mandatory field missing"), answer)
        answer = page.cdp.send_raw('{"id": 77004, "method": "Runtime.evaluate", "params": {"expression": 5}}')
        expect(answer["error"]["data"].startswith("Failed to deserialize params.expression - BINDINGS: string value expected"), answer)
        answer = page.cdp.send_raw('{"id": 77005, "method": "Input.dispatchMouseEvent", "params": {"type": "mouseMoved", "x": "1", "y": 2}}')
        expect(answer["error"]["data"].startswith("Failed to deserialize params.x - BINDINGS: double value expected"), answer)
        answer = page.cdp.send_raw('{"id": 77006, "method": "Nonsense.method"}')
        equal(answer["error"]["code"], -32601, "a method that does not exist")
        equal(page.evaluate("1 + 1"), 2, "the connection after all that")


@check
def errors_are_chromes():
    with Page(f"{PAGES}/index.html") as page:
        cases = [
            ("Runtime.releaseObject", {"objectId": "nope"}, -32000, "Invalid remote object id"),
            ("Runtime.getProperties", {"objectId": "nope"}, -32000, "Invalid remote object id"),
            ("Runtime.callFunctionOn", {"objectId": "nope", "functionDeclaration": "function(){}"}, -32000, "Invalid remote object id"),
            ("Runtime.callFunctionOn", {"functionDeclaration": "function(){}"}, -32602, "Either objectId or executionContextId or uniqueContextId must be specified"),
            ("Runtime.evaluate", {"expression": "1", "contextId": 987654}, -32000, "Cannot find context with specified id"),
            ("DOM.describeNode", {"backendNodeId": 99999}, -32000, "No node found for given backend id"),
            ("DOM.describeNode", {}, -32000, "Either nodeId, backendNodeId or objectId must be specified"),
            ("DOM.getBoxModel", {"objectId": page.handle("document.createElement('p')")}, -32000, "Could not compute box model."),
            ("DOM.getContentQuads", {"objectId": page.handle("({})")}, -32000, "Object id doesn't reference a Node"),
            ("DOM.focus", {"objectId": page.handle("document.getElementById('log')")}, -32000, "Element is not focusable"),
            ("Page.navigate", {"url": "nonsense"}, -32000, "Cannot navigate to invalid URL"),
            ("Page.navigateToHistoryEntry", {"entryId": 9999}, -32602, "No entry with passed id"),
            ("Page.handleJavaScriptDialog", {"accept": True}, -32602, "No dialog is showing"),
            ("Page.removeScriptToEvaluateOnNewDocument", {"identifier": "999"}, -32000, "Script not found"),
            ("Page.captureScreenshot", {"format": "bmp"}, -32602, "Invalid image format"),
            ("Input.dispatchMouseEvent", {"type": "nope", "x": 1, "y": 1}, -32602, "Unexpected event type 'nope'"),
            ("Input.dispatchKeyEvent", {"type": "nope"}, -32602, "Unexpected event type 'nope'"),
            ("Network.setCookie", {"name": "a", "value": "b"}, -32602, "At least one of the url or domain needs to be specified"),
            ("Network.deleteCookies", {"name": "a"}, -32602, "At least one of the url and domain needs to be specified"),
        ]
        for method, params, code, message in cases:
            equal(page.cdp.failure(method, params, standard="nope" not in str(params) and "bmp" not in str(params)), (code, message), f"{method} {params}")
        browser_cases = [
            ("Target.attachToTarget", {"targetId": "nope", "flatten": True}, -32602, "No target with given id found"),
            ("Target.closeTarget", {"targetId": "nope"}, -32602, "No target with given id found"),
            ("Target.activateTarget", {"targetId": "nope"}, -32602, "No target with given id found"),
            ("Target.getTargetInfo", {"targetId": "nope"}, -32602, "No target with given id found"),
            ("Target.detachFromTarget", {"sessionId": "nope"}, -32602, "No session with given id"),
            ("Target.disposeBrowserContext", {"browserContextId": "nope"}, -32000, "Failed to find context with id nope"),
            ("Target.createTarget", {"url": "about:blank", "browserContextId": "nope"}, -32000, "Failed to find browser context with id nope"),
            ("Browser.getWindowForTarget", {"targetId": "nope"}, -32000, "No target with given id"),
            ("Page.enable", {}, -32601, "'Page.enable' wasn't found"),
        ]
        for method, params, code, message in browser_cases:
            equal(page.browser.failure(method, params), (code, message), f"{method} {params}")


@check
def objects_are_described_as_chrome_does():
    with Page(f"{PAGES}/index.html") as page:
        cases = [
            ("document.getElementById('box')", ("object", "node", "HTMLButtonElement", "button#box")),
            ("Object.assign(document.createElement('div'), {className: 'a b', title: 't'})", ("object", "node", "HTMLDivElement", "div.a.b")),
            ("document.getElementById('box').firstChild", ("object", "node", "Text", "#text")),
            ("document", ("object", "node", "HTMLDocument", "#document")),
            ("[1, 2, 3]", ("object", "array", "Array", "Array(3)")),
            ("new Uint8Array(3)", ("object", "typedarray", "Uint8Array", "Uint8Array(3)")),
            ("new Map([[1, 2]])", ("object", "map", "Map", "Map(1)")),
            ("new Set([1, 2])", ("object", "set", "Set", "Set(2)")),
            ("Promise.resolve(1)", ("object", "promise", "Promise", "Promise")),
            ("/a/g", ("object", "regexp", "RegExp", "/a/g")),
            ("(function foo(a) { return a })", ("function", None, "Function", "function foo(a) { return a }")),
            ("(class A {})", ("function", None, "Function", "class A {}")),
            ("Symbol('s')", ("symbol", None, None, "Symbol(s)")),
            ("'text'", ("string", None, None, None)),
            ("true", ("boolean", None, None, None)),
            ("7", ("number", None, None, "7")),
            ("window", ("object", None, "Window", "Window")),
            ("({a: 1})", ("object", None, "Object", "Object")),
        ]
        for expression, expected in cases:
            result = page.cdp.call("Runtime.evaluate", {"expression": expression})["result"]
            equal((result["type"], result.get("subtype"), result.get("className"), result.get("description")), expected, expression)
        properties = page.cdp.call("Runtime.getProperties", {"objectId": page.handle("({a: 1})"), "ownProperties": True})
        equal([p["name"] for p in properties["result"]], ["a"], "own properties")
        equal([p["name"] for p in properties["internalProperties"]], ["[[Prototype]]"], "internal properties")
        preview = page.cdp.call("Runtime.evaluate", {"expression": "({a: 1, b: [1, 2]})", "generatePreview": True})["result"]["preview"]
        equal((preview["overflow"], [(p["name"], p["type"], p.get("value")) for p in preview["properties"]]),
              (False, [("a", "number", "1"), ("b", "object", "Array(2)")]), "preview")
        root = page.cdp.call("DOM.getDocument")["root"]
        equal(["children" in child for child in root["children"]], [False, True], "getDocument goes two levels deep by default")
        root = page.cdp.call("DOM.getDocument", {"depth": 1})["root"]
        equal(["children" in child for child in root["children"]], [False, False], "getDocument with a depth of one")
        value = page.cdp.call("Runtime.callFunctionOn", {"objectId": page.handle("document"), "functionDeclaration": "function(x) { return x }", "arguments": [{"unserializableValue": "-0"}], "returnByValue": True})
        equal(value["result"].get("unserializableValue"), "-0", "negative zero")


@check
def lifecycle_is_replayed_to_a_late_listener():
    with Page(f"{PAGES}/second.html") as page:
        page.cdp.wait_event("Page.lifecycleEvent", lambda p: p["name"] == "networkIdle")
        late = Connection(f"ws://127.0.0.1:{CDP_PORT}/devtools/page/{page.target}")
        try:
            late.call("Page.enable")
            late.call("Page.setLifecycleEventsEnabled", {"enabled": True})
            names = []
            while "networkIdle" not in names:
                names.append(late.wait_event("Page.lifecycleEvent", timeout=5)["name"])
            equal(names, ["commit", "DOMContentLoaded", "load", "networkAlmostIdle", "networkIdle"], "what a late listener is told")
        finally:
            late.close()


@check
def contexts_are_cleared_with_the_document():
    with Page(f"{PAGES}/index.html") as page:
        page.cdp.drain()
        created = page.cdp.take_events("Runtime.executionContextCreated")
        page.cdp.events.clear()
        page.goto(f"{PAGES}/second.html")
        page.cdp.drain()
        expect(page.cdp.take_events("Runtime.executionContextsCleared"), "no executionContextsCleared for a new document")
        # The frames that went with the document may be reported; the page's own context is not.
        main = [c["context"]["id"] for c in created if c["context"]["auxData"]["frameId"] == page.target]
        gone = [p["executionContextId"] for p in page.cdp.take_events("Runtime.executionContextDestroyed")]
        expect(not set(main) & set(gone), f"the main frame's context was reported one by one as well: {gone}")


@check
def the_document_changing_its_own_address_is_reported():
    with Page(f"{PAGES}/index.html") as page:
        page.cdp.events.clear()
        page.evaluate("location.hash = '#part'")
        event = page.cdp.wait_event("Page.navigatedWithinDocument", timeout=5)
        equal((event["frameId"], event["url"], event["navigationType"]), (page.target, f"{PAGES}/index.html#part", "fragment"), "a fragment")
        page.evaluate("history.pushState({}, '', '/pushed?x=1')")
        event = page.cdp.wait_event("Page.navigatedWithinDocument", timeout=5)
        equal((event["url"], event["navigationType"]), (f"{PAGES}/pushed?x=1", "historyApi"), "the history API")
        page.goto(f"{PAGES}/second.html")
        page.cdp.drain(0.5)
        equal(page.cdp.take_events("Page.navigatedWithinDocument"), [], "a new document is not one of these")


@check
def failed_requests_are_named_as_in_chrome():
    with Page(f"{PAGES}/index.html") as page:
        page.cdp.events.clear()
        page.evaluate(f"fetch('http://127.0.0.1:{OTHER_PORT + 7}/nothing').catch(() => 0)")
        failed = page.cdp.wait_event("Network.loadingFailed", timeout=10)
        equal((failed["type"], failed["errorText"], failed["canceled"]), ("Fetch", "net::ERR_CONNECTION_REFUSED", False), "a refused connection")
        equal(page.cdp.failure("Network.getResponseBody", {"requestId": "nope"}), (-32000, "No resource with given identifier found"), "the body of no request")


def main():
    failures = 0
    skipped = 0
    for function in CHECKS:
        if ONLY and function.__name__ not in ONLY:
            continue
        if REFERENCE and getattr(function, "zebkit_only", False):
            skipped += 1
            print(f"skip  {function.__name__} (zebkit only)")
            continue
        started = time.monotonic()
        try:
            function()
        except Exception as error:  # noqa: BLE001 - every failure is reported
            failures += 1
            print(f"FAIL  {function.__name__}: {error}")
            if os.environ.get("CDP_TRACE"):
                traceback.print_exc()
        else:
            print(f"ok    {function.__name__} ({time.monotonic() - started:.1f}s)")
    print(f"{len([f for f in CHECKS if not ONLY or f.__name__ in ONLY]) - failures - skipped} passed, {failures} failed, {skipped} skipped")
    for problem, count in sorted(NONSTANDARD.items()):
        print(f"NONSTANDARD  {problem} ({count}x)")
    print(f"{len(NONSTANDARD)} departures from the standard protocol definition")
    return 1 if failures or NONSTANDARD else 0


if __name__ == "__main__":
    sys.exit(main())
