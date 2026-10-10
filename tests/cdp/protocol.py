#!/usr/bin/env python3
"""Raw Chrome DevTools Protocol checks: protocol.py CDP_PORT PAGE_PORT OTHER_PORT

Talks to a running cdphost the way a CDP client would and checks the answers
against what Chrome gives. Pages are tests/cdp/pages, served on PAGE_PORT and
again on OTHER_PORT (a different origin). Run through scripts/cdp.sh.
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
from cdpclient import Connection, ProtocolError, http_json  # noqa: E402

CDP_PORT, PAGE_PORT, OTHER_PORT = (int(argument) for argument in sys.argv[1:4])
ONLY = sys.argv[4:]
PAGES = f"http://127.0.0.1:{PAGE_PORT}"
OTHER = f"http://localhost:{OTHER_PORT}"

CHECKS = []


def check(function):
    CHECKS.append(function)
    return function


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
        params = {"type": "keyDown", "key": key, "modifiers": modifiers}
        if text:
            params["text"] = text
        self.cdp.call("Input.dispatchKeyEvent", params)
        self.cdp.call("Input.dispatchKeyEvent", {"type": "keyUp", "key": key, "modifiers": modifiers})


# -- discovery and access -------------------------------------------------


@check
def http_discovery():
    version = http_json(CDP_PORT, "/json/version")
    expect(version["webSocketDebuggerUrl"].startswith(f"ws://127.0.0.1:{CDP_PORT}/devtools/browser/"), version)
    equal(version["Protocol-Version"], "1.3", "protocol version")
    pages = http_json(CDP_PORT, "/json/list")
    expect(pages and pages[0]["type"] == "page", pages)
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
        expect("wasn't found" in page.cdp.error("Nonsense.method"), "wrong error text")
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
            "functionDeclaration": "function(a, b, c) { return [this.tagName, a, b.id, c] }\n//# sourceURL=test",
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
        page.goto(f"{PAGES}/index.html")
        equal(page.evaluate("window.injected"), "yes", "script on new document")
        page.cdp.drain()
        created = [c["context"] for c in page.cdp.take_events("Runtime.executionContextCreated")]
        frames = page.cdp.call("Page.getFrameTree")["frameTree"]
        child = frames["childFrames"][0]["frame"]["id"]
        for frame in (page.target, child):
            kinds = sorted((c["auxData"]["type"], c["name"]) for c in created if c["auxData"]["frameId"] == frame)
            expect(("default", "") in kinds and ("isolated", "helper") in kinds, f"contexts of frame {frame}: {kinds}")
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
        for name in ("init", "commit", "DOMContentLoaded", "load", "networkIdle"):
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
        equal(metrics["cssVisualViewport"]["clientWidth"], 800, "visual viewport width")
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
        value = page.cdp.call("Runtime.evaluate", {"expression": "innerClicks", "contextId": world, "returnByValue": True})
        equal(value["result"]["value"], 1, "click landed in the frame")

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
        expect(version["product"].startswith("Zebkit/") and version["userAgent"], version)
        browser.call("Target.setDiscoverTargets", {"discover": True})
        existing = browser.wait_event("Target.targetCreated")
        equal(existing["targetInfo"]["type"], "page", "existing page announced")
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
        value = browser.call("Runtime.evaluate", {"expression": "location.href", "returnByValue": True}, session=session)
        equal(value["result"]["value"], "about:blank", "the page has not started loading")
        browser.call("Runtime.runIfWaitingForDebugger", session=session)
        browser.wait_event("Page.loadEventFired", session=session)
        value = browser.call("Runtime.evaluate", {"expression": "[document.title, window.early]", "returnByValue": True}, session=session)
        equal(value["result"]["value"], ["Second page", "loading"], "script installed before the first load ran")
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


@check
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
    equal(len(http_json(CDP_PORT, "/json/list")), before, "pages after the refused requests")
    try:
        connection = Connection(f"ws://127.0.0.1:{CDP_PORT}/devtools/browser/0000")
        connection.call("Browser.getVersion", timeout=3)
    except (ProtocolError, OSError, TimeoutError):
        return
    raise AssertionError("a browser socket with the wrong id was accepted")


@check
def hostile_ids_are_harmless():
    with Page(f"{PAGES}/index.html") as page:
        for value in (2**31 - 1, 2**31 - 2, -1, 0, -(2**31)):
            page.cdp.error("DOM.describeNode", {"backendNodeId": value})
            page.cdp.error("DOM.resolveNode", {"nodeId": value})
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
        page.cdp.call("Page.crash")
        page.cdp.wait_event("Inspector.targetCrashed", timeout=10)
        answer = page.cdp.reply(waiting, timeout=5)
        expect("error" in answer, f"a command the dead page was running was answered with {answer}")
        equal(page.cdp.error("Runtime.evaluate", {"expression": "1"}), "Target crashed", "a command sent to the dead page")
        page.goto(f"{PAGES}/second.html")
        equal(page.evaluate("document.title"), "Second page", "the page after the crash")
        page.cdp.call("Page.crash")
        page.cdp.wait_event("Inspector.targetCrashed", timeout=10)
        page.cdp.call("Page.reload")
        page.cdp.wait_event("Page.lifecycleEvent", lambda p: p["name"] == "load")
        equal(page.evaluate("document.title"), "Second page", "the page reloaded after a second crash")


@check
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
        equal(page.evaluate("[innerWidth, innerHeight]"), before, "size after clearing the override")


def main():
    failures = 0
    for function in CHECKS:
        if ONLY and function.__name__ not in ONLY:
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
    print(f"{len([f for f in CHECKS if not ONLY or f.__name__ in ONLY]) - failures} passed, {failures} failed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
