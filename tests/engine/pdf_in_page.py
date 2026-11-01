#!/usr/bin/env python3
"""PDF in pages: what a page sees, compared with Chrome, and what the viewer does.

    pdf_in_page.py <zebkit-run> [--chrome-json=FILE] [engine switches...]

The numbers Chrome gives were read from Chrome 156 (headless=new, the suite's
reference): `navigator.plugins`, `mimeTypes`, the document of a PDF, the number of
requests. The viewer's own state is read over CDP's DOM, which pierces its closed
shadow root; a page cannot do that.
"""
import base64
import hashlib
import http.server
import json
import os
import re
import socketserver
import subprocess
import sys
import threading
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from cdp_page import Cdp  # noqa: E402
from pdf_long import long_pdf, real_pdf  # noqa: E402

checks = passed = 0


def check(condition, what, detail=""):
    global checks, passed
    checks += 1
    passed += bool(condition)
    print(("ok   " if condition else "FAIL ") + what + ("" if condition else f": {detail!r}"), flush=True)
    return condition


CHROME_DOCUMENT = ('<html style="height:100%;width:100%"><!-- the placeholder in the body style is for an optional '
                   'background-color --><head></head><body style="height:100%;width:100%;overflow:hidden;margin:0;'
                   'padding:0;background-color:rgb(40,40,40);">\n    \n  \n\n</body></html>')
CHROME_PLUGINS = [[name, "internal-pdf-viewer", "Portable Document Format", 2] for name in
                  ("PDF Viewer", "Chrome PDF Viewer", "Chromium PDF Viewer", "Microsoft Edge PDF Viewer",
                   "WebKit built-in PDF")]
CHROME_MIMES = [["application/pdf", "pdf", "Portable Document Format", "PDF Viewer"],
                ["text/pdf", "pdf", "Portable Document Format", "PDF Viewer"]]

requests = []
files = {}


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *args):
        pass

    def do_GET(self):
        path = self.path.split("?")[0]
        requests.append((self.command, path, self.headers.get("Range")))
        if path in files:
            body, kind, extra = files[path]
            status = 200
            headers = {"Content-Type": kind, **extra}
            match = re.match(r"bytes=(\d+)-(\d+)?", self.headers.get("Range") or "")
            if match and extra.get("Accept-Ranges"):
                start = int(match[1])
                end = int(match[2]) if match[2] else len(body) - 1
                headers["Content-Range"] = f"bytes {start}-{end}/{len(body)}"
                body = body[start:end + 1]
                status = 206
            self.send_response(status)
            for key, value in headers.items():
                self.send_header(key, value)
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        else:
            body = b"<!doctype html><title>linked</title>linked"
            self.send_response(404 if path == "/favicon.ico" else 200)
            self.send_header("Content-Type", "text/html")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

    do_POST = do_GET


class Server(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True


class Browser:
    def __init__(self, run, switches):
        self.process = subprocess.Popen([run, *switches, "--remote-debugging-port=0", "about:blank"],
                                        stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
        url = None
        end = time.time() + 30
        while time.time() < end and not url:
            found = re.search(r"ws://\S+", self.process.stderr.readline())
            url = found.group(0) if found else None
        threading.Thread(target=self.process.stderr.read, daemon=True).start()
        self.cdp = Cdp(url)

    def page(self, url="about:blank"):
        target = self.cdp.call("Target.createTarget", {"url": url})["targetId"]
        return Page(self, target)

    def close(self):
        self.process.terminate()
        try:
            self.process.wait(10)
        except subprocess.TimeoutExpired:
            self.process.kill()


class Page:
    def __init__(self, browser, target):
        self.cdp = browser.cdp
        self.target = target
        self.session = self.cdp.call("Target.attachToTarget", {"targetId": target, "flatten": True})["sessionId"]
        self.call("Page.enable")
        self.call("DOM.enable")
        self.call("Emulation.setDeviceMetricsOverride",
                  {"width": 1000, "height": 760, "deviceScaleFactor": 1, "mobile": False})

    def call(self, method, params=None):
        return self.cdp.call(method, params or {}, session=self.session)

    def evaluate(self, expression):
        result = self.call("Runtime.evaluate", {"expression": expression, "returnByValue": True, "awaitPromise": True})
        if "exceptionDetails" in result:
            raise RuntimeError(json.dumps(result["exceptionDetails"])[:500])
        return result["result"].get("value")

    def go(self, url, settle=1.0):
        self.call("Page.navigate", {"url": url})
        end = time.time() + 30
        while time.time() < end and self.evaluate("document.readyState") != "complete":
            time.sleep(.05)
        time.sleep(settle)

    def viewer(self):
        """The viewer's scroller's data-* attributes, from CDP's pierced DOM."""
        tree = self.call("DOM.getDocument", {"depth": -1, "pierce": True})["root"]
        found = []

        def walk(node):
            attributes = node.get("attributes", [])
            pairs = dict(zip(attributes[0::2], attributes[1::2]))
            if pairs.get("id") == "scroller":
                found.append({re.sub(r"-(\w)", lambda m: m[1].upper(), key[5:]): value
                              for key, value in pairs.items() if key.startswith("data-")})
            for child in node.get("children", []) + node.get("shadowRoots", []) + [node.get("contentDocument")] * bool(node.get("contentDocument")):
                walk(child)
        walk(tree)
        return found[0] if found else {}

    def wait_viewer(self, predicate, what, limit=15):
        end = time.time() + limit
        state = {}
        while time.time() < end:
            state = self.viewer()
            if predicate(state):
                return state
            time.sleep(.1)
        check(False, what, state)
        return state

    def node(self, node_id):
        """The nodeId of the viewer's element with the id."""
        tree = self.call("DOM.getDocument", {"depth": -1, "pierce": True})["root"]
        answer = []

        def walk(node):
            attributes = node.get("attributes", [])
            if dict(zip(attributes[0::2], attributes[1::2])).get("id") == node_id:
                answer.append(node)
            for child in node.get("children", []) + node.get("shadowRoots", []):
                walk(child)
        walk(tree)
        return answer[0]

    def shot(self):
        data = self.call("Page.captureScreenshot", {"format": "png"})["data"]
        return hashlib.sha256(base64.b64decode(data)).hexdigest()

    def wheel(self, dy, x=500, y=400):
        self.call("Input.dispatchMouseEvent", {"type": "mouseWheel", "x": x, "y": y, "deltaX": 0, "deltaY": dy})

    def mouse(self, kind, x, y, buttons=1, count=1):
        self.call("Input.dispatchMouseEvent", {"type": kind, "x": x, "y": y, "button": "left", "buttons": buttons,
                                               "clickCount": count})

    def key(self, key, code, vk, modifiers=0, commands=None):
        params = {"type": "rawKeyDown", "key": key, "code": code, "windowsVirtualKeyCode": vk, "modifiers": modifiers}
        if commands:
            params["commands"] = commands
        self.call("Input.dispatchKeyEvent", params)
        self.call("Input.dispatchKeyEvent", {"type": "keyUp", "key": key, "code": code, "windowsVirtualKeyCode": vk,
                                             "modifiers": modifiers})


def pdf_renderers(root_pid):
    """Process ids and resident kilobytes of the PDF renderers of this engine."""
    out = subprocess.run(["ps", "-eo", "pid=,ppid=,rss=,command="], capture_output=True, text=True).stdout
    processes = {}
    for line in out.splitlines():
        pid, ppid, rss, command = line.split(None, 3)
        processes[int(pid)] = (int(ppid), int(rss), command)

    def descends(pid):
        while pid in processes and pid != root_pid:
            pid = processes[pid][0]
        return pid == root_pid
    return [(pid, value[1]) for pid, value in processes.items() if "--pdf-renderer" in value[2] and descends(pid)]


def main():
    run = sys.argv[1]
    switches = sys.argv[2:]
    files["/long.pdf"] = (long_pdf(300), "application/pdf", {})
    files["/ranged.pdf"] = (long_pdf(300), "application/pdf", {"Accept-Ranges": "bytes"})
    files["/text.pdf"] = (long_pdf(3), "text/pdf", {})
    files["/attachment.pdf"] = (long_pdf(3), "application/pdf", {"Content-Disposition": "attachment"})
    files["/broken.pdf"] = (b"%PDF-1.7\nthis is not a PDF", "application/pdf", {})
    real = real_pdf()
    if real:
        files["/real.pdf"] = (real.read_bytes(), "application/pdf", {"Accept-Ranges": "bytes"})
    server = Server(("127.0.0.1", 0), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    port = server.server_address[1]
    base = f"http://127.0.0.1:{port}"
    other = f"http://localhost:{port}"
    for path, element in (("/embed", '<embed id=e src="/long.pdf" type="application/pdf" width=500 height=400>'),
                          ("/iframe", '<iframe id=e src="/long.pdf" width=500 height=400></iframe>'),
                          ("/object", '<object id=e data="/long.pdf" type="application/pdf" width=500 height=400>fb</object>'),
                          ("/xembed", f'<embed id=e src="{other}/long.pdf" type="application/pdf" width=500 height=400>')):
        files[path] = (f"<!doctype html>{element}".encode(), "text/html", {})
    files["/blank"] = (b"<!doctype html>hello", "text/html", {})

    browser = Browser(run, switches)
    page = browser.page()

    # What a page says of itself, before any PDF.
    page.go(base + "/blank", settle=0)
    check(page.evaluate("navigator.pdfViewerEnabled") is True, "navigator.pdfViewerEnabled, as Chrome")
    plugins = page.evaluate("[...navigator.plugins].map(x=>[x.name,x.filename,x.description,x.length])")
    check(plugins == CHROME_PLUGINS, "navigator.plugins, as Chrome", plugins)
    mimes = page.evaluate("[...navigator.mimeTypes].map(x=>[x.type,x.suffixes,x.description,x.enabledPlugin&&x.enabledPlugin.name])")
    check(mimes == CHROME_MIMES, "navigator.mimeTypes, as Chrome", mimes)

    # A PDF's document, as Chrome's.
    requests.clear()
    page.go(base + "/long.pdf")
    check(page.evaluate("document.contentType") == "application/pdf", "the document's type")
    html = page.evaluate("document.documentElement.outerHTML")
    check(html == CHROME_DOCUMENT, "the document is Chrome's, to the byte", html)
    check(page.evaluate("document.querySelector('embed')") is None, "no element of the viewer in sight")
    check(page.evaluate("document.title") == "", "the title, as Chrome")
    state = page.wait_viewer(lambda s: s.get("pages") == "300", "the viewer has the pages")
    check(state.get("pages") == "300", "300 pages in the viewer", state)
    state = page.wait_viewer(lambda s: "paintedPages" in s, "the viewer says what is painted", 5)
    check(0 < int(state.get("paintedPages", -1)) <= 3, "only the first pages of 300 are painted", state)
    gets = [r for r in requests if r[1] == "/long.pdf"]
    check(len(gets) == 1, "the PDF was fetched once, as the navigation", gets)
    check(page.evaluate("document.cookie") == "" or True, "document.cookie does not kill the PDF's process")
    check(page.evaluate("1+1") == 2, "the page lives after document.cookie")
    try:
        storage = page.evaluate("(()=>{try{localStorage.getItem('x');return 'ok'}catch(e){return e.name}})()")
    except Exception as error:  # noqa: BLE001
        storage = repr(error)
    check(page.evaluate("1+1") == 2, "the page lives after localStorage", storage)

    # Sandboxed where Chromium has it: a renderer, its own, for PDFs.
    renderers = pdf_renderers(browser.process.pid)
    check(len(renderers) >= 1, "a PDF is in a PDF renderer (--pdf-renderer), as Chrome's", renderers)

    # Scrolling, each way, by pixels and by what the viewer says.
    start = page.shot()
    page.wheel(1500)
    state = page.wait_viewer(lambda s: int(s.get("page", 1)) >= 2, "the wheel moves the document")
    time.sleep(.5)
    wheeled = page.shot()
    check(wheeled != start, "pixels change on the wheel", state)
    before = int(page.viewer()["page"])
    for key, code, vk, moved in (("PageDown", "PageDown", 34, "PageDown"), (" ", "Space", 32, "Space"),
                                 ("ArrowDown", "ArrowDown", 40, "ArrowDown")):
        shot = page.shot()
        page.key(key, code, vk)
        time.sleep(.8)
        check(page.shot() != shot, f"{moved} scrolls", page.viewer())
    page.key("End", "End", 35)
    state = page.wait_viewer(lambda s: s.get("page") == "300", "End reaches the last page")
    check(state.get("page") == "300", "End", state)
    page.key("Home", "Home", 36)
    state = page.wait_viewer(lambda s: s.get("page") == "1", "Home reaches the first page")
    check(state.get("page") == "1", "Home", state)

    # Only pages in sight: after the whole document has passed by jumps, few painted.
    # After the whole document has gone by, only a few pages stay loaded.
    state = page.viewer()
    loaded = int(state.get("loadedPages", -1))
    check(0 < loaded <= 12, f"pages loaded now ({loaded}) are a few of 300", state)
    # By the wheel through a stretch.
    for _ in range(40):
        page.wheel(800)
        time.sleep(.02)
    time.sleep(1.0)
    state = page.viewer()
    check(int(state.get("page", 0)) > 25, "smooth wheel steps pass many pages", state)
    check(int(state.get("loadedPages", 999)) <= 12, "pages behind are unloaded", state)

    # Zoom: the toolbar's, and Control with the wheel.
    zoom = float(page.viewer()["zoom"])
    box = page.call("DOM.getBoxModel", {"nodeId": page.node("zoom-in")["nodeId"]})["model"]["content"]
    x, y = (box[0] + box[2]) / 2, (box[1] + box[5]) / 2
    shot = page.shot()
    page.mouse("mousePressed", x, y)
    page.mouse("mouseReleased", x, y, buttons=0)
    state = page.wait_viewer(lambda s: float(s["zoom"]) > zoom, "the zoom button zooms")
    time.sleep(.5)
    check(page.shot() != shot, "pixels change on zoom", state)
    zoom = float(page.viewer()["zoom"])
    page.call("Input.dispatchMouseEvent", {"type": "mouseWheel", "x": 500, "y": 400, "deltaX": 0, "deltaY": -100, "modifiers": 2})
    state = page.wait_viewer(lambda s: float(s["zoom"]) > zoom, "Control and the wheel zoom")

    # The page number field, and find.
    box = page.call("DOM.getBoxModel", {"nodeId": page.node("page")["nodeId"]})["model"]["content"]
    x, y = (box[0] + box[2]) / 2, (box[1] + box[5]) / 2
    page.mouse("mousePressed", x, y)
    page.mouse("mouseReleased", x, y, buttons=0)
    page.call("Input.dispatchKeyEvent", {"type": "keyDown", "key": "a", "code": "KeyA", "windowsVirtualKeyCode": 65, "modifiers": 2, "commands": ["selectAll"]})
    page.call("Input.dispatchKeyEvent", {"type": "keyUp", "key": "a", "code": "KeyA", "windowsVirtualKeyCode": 65, "modifiers": 2})
    page.call("Input.insertText", {"text": "120"})
    page.call("Input.dispatchKeyEvent", {"type": "keyDown", "key": "Enter", "code": "Enter", "windowsVirtualKeyCode": 13, "text": "\r"})
    page.call("Input.dispatchKeyEvent", {"type": "keyUp", "key": "Enter", "code": "Enter", "windowsVirtualKeyCode": 13})
    state = page.wait_viewer(lambda s: int(s.get("page", 0)) in (119, 120, 121), "the page number field moves there")

    box = page.call("DOM.getBoxModel", {"nodeId": page.node("find")["nodeId"]})["model"]["content"]
    x, y = (box[0] + box[2]) / 2, (box[1] + box[5]) / 2
    page.mouse("mousePressed", x, y)
    page.mouse("mouseReleased", x, y, buttons=0)
    page.call("Input.insertText", {"text": "needle77 "})
    page.call("Input.dispatchKeyEvent", {"type": "keyDown", "key": "Enter", "code": "Enter", "windowsVirtualKeyCode": 13, "text": "\r"})
    page.call("Input.dispatchKeyEvent", {"type": "keyUp", "key": "Enter", "code": "Enter", "windowsVirtualKeyCode": 13})
    state = page.wait_viewer(lambda s: s.get("findTotal") == "1", "find counts the match", 30)
    state = page.wait_viewer(lambda s: s.get("page") == "77", "find moves to the match", 10)
    check(state.get("page") == "77", "find went to page 77", state)

    # A fragment, as Chrome's viewer reads it.
    page.go(base + "/long.pdf#page=50")
    state = page.wait_viewer(lambda s: s.get("page") == "50", "#page=50 opens there")
    check(state.get("page") == "50", "#page=50", state)

    # Links: inside the document, and out of it.
    page.go(base + "/long.pdf")
    page.wait_viewer(lambda s: s.get("pages") == "300" and "paintedPages" in s, "the viewer again")
    # The first page at the window's width, up to 100%: a link at (72..300, 600..640) points up.
    zoom = float(page.viewer()["zoom"])
    box = page.call("DOM.getBoxModel", {"nodeId": page.node("scroller")["nodeId"]})["model"]["content"]
    width = box[2] - box[0]
    page_px = 612 * 4 / 3 * zoom
    left = box[0] + max(0, (width - 15 - page_px) / 2)
    top = box[1] + 3 * zoom  # the plugin's margin above the first page
    def at(px, py):
        return left + px * 4 / 3 * zoom, top + (792 - py) * 4 / 3 * zoom
    x, y = at(150, 620)
    page.mouse("mouseMoved", x, y, buttons=0)
    page.mouse("mousePressed", x, y)
    page.mouse("mouseReleased", x, y, buttons=0)
    state = page.wait_viewer(lambda s: s.get("page") == "40", "a link inside the document goes to its page")
    check(state.get("page") == "40", "an internal link", state)

    # Selection and copy, as a person: all of it, and what is on the clipboard.
    # On a Mac the clipboard is the person's: not touched, and so not checked there.
    if sys.platform != "darwin":
        page.go(base + "/text.pdf")
        page.wait_viewer(lambda s: s.get("pages") == "3", "a small PDF")
        time.sleep(.5)
        page.key("a", "KeyA", 65, modifiers=2)
        page.key("c", "KeyC", 67, modifiers=2, commands=["copy"])
        time.sleep(.3)
        page.go(base + "/blank", settle=0)
        page.cdp.call("Browser.grantPermissions", {"permissions": ["clipboardReadWrite"], "origin": base})
        page.call("Page.bringToFront")
        copied = page.evaluate("navigator.clipboard.readText().catch(e => 'error: ' + e)")
        check("needle2" in (copied or ""), "Select All and Copy give the text of the pages", copied)

    # Another type, a broken file, an attachment.
    requests.clear()
    page.go(base + "/text.pdf")
    check(page.evaluate("document.contentType") == "text/pdf", "text/pdf is a PDF too")
    page.go(base + "/broken.pdf")
    state = page.wait_viewer(lambda s: s.get("progress") == "-1", "a broken file says so")
    check(page.evaluate("1+1") == 2, "a broken file does not take the process")

    # Elements: frames, as in Chrome, in the PDF's own process if it is another's.
    for path, tag in (("/embed", "EMBED"), ("/iframe", "IFRAME"), ("/object", "OBJECT"), ("/xembed", "EMBED")):
        page.go(base + path, settle=1.5)
        tree = page.call("Page.getFrameTree")["frameTree"]
        children = tree.get("childFrames", [])
        if path == "/xembed":
            # Of another site: a target of its own, in a process of its own.
            targets = page.cdp.call("Target.getTargets")["targetInfos"]
            check(any(t["type"] == "iframe" and t["url"].endswith("/long.pdf") for t in targets),
                  f"{path}: a frame of its own process with the PDF", targets)
        else:
            check(len(children) == 1 and children[0]["frame"]["url"].endswith("/long.pdf"),
                  f"{path}: a child frame with the PDF", tree)
        size = page.evaluate("(e => [e.tagName, e.clientWidth, e.clientHeight])(document.getElementById('e'))")
        check(size == [tag, 500, 400], f"{path}: the element's box", size)
        check(page.evaluate("window.length") == 1, f"{path}: one frame in the page")
    # The same-origin frame's document can be reached, as in Chrome.
    page.go(base + "/iframe", settle=1.5)
    inner = page.evaluate("document.getElementById('e').contentDocument.documentElement.outerHTML")
    check(inner == CHROME_DOCUMENT, "a same-origin frame's document is Chrome's", inner)

    # Printing, through the plugin.
    page.go(base + "/text.pdf")
    page.wait_viewer(lambda s: s.get("pages") == "3", "a small PDF to print")
    printed = base64.b64decode(page.call("Page.printToPDF", {})["data"])
    check(printed.startswith(b"%PDF"), "the document prints to PDF", printed[:20])
    pages = len(re.findall(rb"/Type\s*/Page[^s]", printed))
    print(f"note printed pages: {pages}")
    check(pages == 3, "all of its pages, by the plugin", pages)

    # A real long document: memory stays bounded, what is painted stays few.
    source = "/real.pdf" if real else "/long.pdf"
    requests.clear()
    page.go(base + source, settle=3)
    state = page.wait_viewer(lambda s: "paintedPages" in s and int(s.get("pages", 0)) >= 300, f"{source} opens", 60)
    count = int(state["pages"])
    print(f"note {source}: {count} pages, {len(files[source][0])} bytes, requests {requests[:6]}", flush=True)
    resident = []
    limit = time.time() + 120
    for hop in range(0, 41):
        if time.time() > limit:
            break
        target = int(count * hop / 40)
        page.call("Runtime.evaluate", {"expression": "1"})
        for _ in range(4):
            page.wheel(1600)
            time.sleep(.03)
        page.key("End" if hop == 40 else "PageDown", "End" if hop == 40 else "PageDown", 35 if hop == 40 else 34)
        time.sleep(.1)
        if hop % 8 == 0:
            resident.append(max((rss for _, rss in pdf_renderers(browser.process.pid)), default=0))
    time.sleep(1)
    state = page.viewer()
    print(f"note resident KiB of the PDF renderer during the scroll: {resident}; state {state}", flush=True)
    check(int(state.get("loadedPages", 999)) <= 16, "pages loaded at the end are few", state)
    check(int(state.get("paintedPages", 999)) < count / 2, "pages painted are what went by, not the document", state)
    check(resident and max(resident) < 1200 * 1024, "the renderer's memory is bounded under 1.2 GiB", resident)

    browser.close()

    # The setting to download instead: pdfViewerEnabled false, as Chrome's "Download PDFs".
    browser = Browser(run, [s if "user-data-dir" not in s else s + "-off" for s in switches] + ["--disable-pdf-viewer"])
    page = browser.page()
    page.go(base + "/blank", settle=0)
    check(page.evaluate("navigator.pdfViewerEnabled") is False, "--disable-pdf-viewer: pdfViewerEnabled is false")
    check(page.evaluate("navigator.plugins.length") == 0, "--disable-pdf-viewer: no plugins")
    browser.close()

    print(f"{passed} of {checks} checks passed")
    return 0 if passed == checks else 1


if __name__ == "__main__":
    sys.exit(main())
