"""What a CDP client gets of <zebkit/page.h>, as Puppeteer and Playwright
would use it, from tools/zebkit-run serving CDP:

- dialogs held for the client (Page.javascriptDialogOpening) and answered
  by it (Page.handleJavaScriptDialog), Page.javascriptDialogClosed;
- Page.printToPDF, as base64 and as a stream read with IO.read;
- Input.dispatchDragEvent dropping a file; Input.setInterceptDrags and
  Input.dragIntercepted; a drag in the page from Input.dispatchMouseEvent.

    python3 tests/engine/cdp_page.py <zebkit-run> <base URL> <files dir> [engine switches...]

A WebSocket client of its own, on the standard library alone.
"""

import base64
import json
import os
import re
import socket
import struct
import subprocess
import sys
import time

failures = []
checks = 0


def check(ok, what, detail=None):
    global checks
    checks += 1
    print(f"{'ok  ' if ok else 'FAIL'} {what}" + (f": {detail}" if detail is not None else ""), flush=True)
    if not ok:
        failures.append(what)


class Cdp:
    """One WebSocket to the browser target, with flat sessions."""

    def __init__(self, url):
        host, port, path = re.match(r"ws://([^:/]+):(\d+)(/.*)", url).groups()
        self.sock = socket.create_connection((host, int(port)), timeout=30)
        key = base64.b64encode(os.urandom(16)).decode()
        self.sock.sendall((f"GET {path} HTTP/1.1\r\nHost: {host}:{port}\r\nUpgrade: websocket\r\n"
                           f"Connection: Upgrade\r\nSec-WebSocket-Key: {key}\r\n"
                           "Sec-WebSocket-Version: 13\r\n\r\n").encode())
        response = b""
        while b"\r\n\r\n" not in response:
            response += self.sock.recv(4096)
        self.buffer = response.split(b"\r\n\r\n", 1)[1]
        self.next_id = 0
        self.events = []

    def _recv_exact(self, n):
        while len(self.buffer) < n:
            chunk = self.sock.recv(1 << 16)
            if not chunk:
                raise EOFError("the WebSocket closed")
            self.buffer += chunk
        data, self.buffer = self.buffer[:n], self.buffer[n:]
        return data

    def _read_message(self):
        payload = b""
        while True:
            head = self._recv_exact(2)
            fin, opcode, length = head[0] & 0x80, head[0] & 0x0F, head[1] & 0x7F
            if length == 126:
                length = struct.unpack(">H", self._recv_exact(2))[0]
            elif length == 127:
                length = struct.unpack(">Q", self._recv_exact(8))[0]
            payload += self._recv_exact(length)
            if opcode == 8:
                raise EOFError("closed")
            if fin:
                return json.loads(payload)

    def send(self, method, params=None, session=None):
        self.next_id += 1
        message = {"id": self.next_id, "method": method, "params": params or {}}
        if session:
            message["sessionId"] = session
        data = json.dumps(message).encode()
        mask = os.urandom(4)
        head = bytes([0x81])
        if len(data) < 126:
            head += bytes([0x80 | len(data)])
        elif len(data) < 65536:
            head += bytes([0x80 | 126]) + struct.pack(">H", len(data))
        else:
            head += bytes([0x80 | 127]) + struct.pack(">Q", len(data))
        self.sock.sendall(head + mask + bytes(b ^ mask[i % 4] for i, b in enumerate(data)))
        return self.next_id

    def call(self, method, params=None, session=None, timeout=30):
        wanted = self.send(method, params, session)
        end = time.time() + timeout
        while time.time() < end:
            message = self._read_message()
            if message.get("id") == wanted:
                if "error" in message:
                    raise RuntimeError(message["error"].get("message"))
                return message.get("result", {})
            if "method" in message:
                self.events.append(message)
        raise TimeoutError(method)

    def event(self, method, timeout=10):
        end = time.time() + timeout
        while time.time() < end:
            for i, message in enumerate(self.events):
                if message.get("method") == method:
                    return self.events.pop(i)
            self.sock.settimeout(max(0.05, end - time.time()))
            try:
                message = self._read_message()
            except socket.timeout:
                continue
            finally:
                self.sock.settimeout(30)
            if "method" in message:
                self.events.append(message)
            elif "id" in message:
                self.events.append({"method": "#reply", **message})
        return None

    def reply(self, wanted, timeout=10):
        end = time.time() + timeout
        while time.time() < end:
            for i, message in enumerate(self.events):
                if message.get("method") == "#reply" and message.get("id") == wanted:
                    return self.events.pop(i)
            self.sock.settimeout(max(0.05, end - time.time()))
            try:
                message = self._read_message()
            except socket.timeout:
                continue
            finally:
                self.sock.settimeout(30)
            self.events.append(message if "method" in message else {"method": "#reply", **message})
        return None


def main():
    run, base, files = sys.argv[1:4]
    switches = sys.argv[4:]
    engine = subprocess.Popen([run, *switches, "--remote-debugging-port=0", "about:blank"],
                              stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
    url = None
    end = time.time() + 30
    while time.time() < end and url is None:
        line = engine.stderr.readline()
        found = re.search(r"ws://\S+", line)
        if found:
            url = found.group(0)
    check(url is not None, "the engine serves CDP", url)
    if not url:
        engine.kill()
        return 1
    try:
        cdp = Cdp(url)
        target = cdp.call("Target.createTarget", {"url": "about:blank"})["targetId"]
        session = cdp.call("Target.attachToTarget", {"targetId": target, "flatten": True})["sessionId"]
        cdp.call("Page.enable", session=session)
        cdp.call("Runtime.enable", session=session)

        def navigate(url):
            cdp.call("Page.navigate", {"url": url}, session=session)
            cdp.event("Page.loadEventFired", 15)

        def evaluate(expression):
            result = cdp.call("Runtime.evaluate", {"expression": expression, "returnByValue": True,
                                                   "awaitPromise": True}, session=session)
            return result.get("result", {}).get("value")

        navigate(f"{base}/page.html")

        # Dialogs wait for the client and are answered by it.
        pending = cdp.send("Runtime.evaluate", {"expression": "confirm('From CDP?')", "returnByValue": True},
                           session=session)
        opening = cdp.event("Page.javascriptDialogOpening")
        check(opening and opening["params"]["type"] == "confirm" and opening["params"]["message"] == "From CDP?",
              "Page.javascriptDialogOpening", opening and opening["params"])
        check(cdp.reply(pending, 0.5) is None, "the page waits for the client", None)
        cdp.call("Page.handleJavaScriptDialog", {"accept": True}, session=session)
        answered = cdp.reply(pending)
        check(answered and answered["result"]["result"]["value"] is True, "Page.handleJavaScriptDialog: OK",
              answered and answered.get("result"))
        closed = cdp.event("Page.javascriptDialogClosed")
        check(closed and closed["params"]["result"] is True, "Page.javascriptDialogClosed", closed and closed["params"])

        pending = cdp.send("Runtime.evaluate", {"expression": "prompt('Name?', 'x')", "returnByValue": True},
                           session=session)
        cdp.event("Page.javascriptDialogOpening")
        cdp.call("Page.handleJavaScriptDialog", {"accept": True, "promptText": "typed over CDP"}, session=session)
        answered = cdp.reply(pending)
        check(answered and answered["result"]["result"]["value"] == "typed over CDP", "a prompt answered with text",
              answered and answered.get("result"))
        try:
            cdp.call("Page.handleJavaScriptDialog", {"accept": True}, session=session)
            check(False, "no dialog to answer is an error")
        except RuntimeError as error:
            check(True, "no dialog to answer is an error", error)

        # Printing to PDF.
        navigate(f"{base}/print.html")
        pdf = base64.b64decode(cdp.call("Page.printToPDF", {"printBackground": True}, session=session)["data"])
        check(pdf.startswith(b"%PDF-") and len(re.findall(rb"/Type\s*/Page\b", pdf)) == 3,
              "Page.printToPDF: three pages", f"{len(pdf)} bytes")
        result = cdp.call("Page.printToPDF", {"transferMode": "ReturnAsStream", "landscape": True}, session=session)
        stream, data = result.get("stream"), b""
        while stream:
            chunk = cdp.call("IO.read", {"handle": stream, "size": 1 << 16}, session=session)
            data += base64.b64decode(chunk["data"]) if chunk.get("base64Encoded") else chunk["data"].encode("latin-1")
            if chunk.get("eof"):
                cdp.call("IO.close", {"handle": stream}, session=session)
                break
        check(data.startswith(b"%PDF-") and data.rstrip().endswith(b"%%EOF"), "as a stream, read with IO.read",
              f"{len(data)} bytes")
        try:
            cdp.call("Page.printToPDF", {"pageRanges": "9-12"}, session=session)
            check(False, "a range past the end is an error")
        except RuntimeError as error:
            check(True, "a range past the end is an error", error)

        # Drags: a file dropped from the client.
        navigate(f"{base}/page.html")
        path = os.path.join(files, "cdp-dropped.txt")
        with open(path, "w") as f:
            f.write("from CDP")
        data = {"items": [], "files": [path], "dragOperationsMask": 1}
        for kind in ("dragEnter", "dragOver"):
            cdp.call("Input.dispatchDragEvent", {"type": kind, "x": 100, "y": 250, "data": data}, session=session)
        cdp.call("Input.dispatchDragEvent", {"type": "drop", "x": 100, "y": 250, "data": data}, session=session)
        time.sleep(0.3)
        dropped = evaluate("JSON.stringify(dropped)") or ""
        check("cdp-dropped.txt" in dropped and "from CDP" in dropped, "Input.dispatchDragEvent drops a file", dropped)

        # The page's drag intercepted, then dropped back with dispatchDragEvent.
        cdp.call("Input.setInterceptDrags", {"enabled": True}, session=session)

        def mouse(kind, x, y, buttons=0):
            cdp.call("Input.dispatchMouseEvent", {"type": kind, "x": x, "y": y, "button": "left",
                                                  "buttons": buttons, "clickCount": 1}, session=session)

        mouse("mouseMoved", 50, 20)
        mouse("mousePressed", 50, 20, 1)
        mouse("mouseMoved", 50, 30, 1)
        mouse("mouseMoved", 50, 40, 1)
        intercepted = cdp.event("Input.dragIntercepted")
        items = intercepted and intercepted["params"]["data"]["items"]
        check(items and any(i["mimeType"] == "application/x-zebkit-item" and i["data"] == "a" for i in items),
              "Input.dragIntercepted, with the page's data", items)
        if intercepted:
            drag = intercepted["params"]["data"]
            for kind in ("dragEnter", "dragOver", "drop"):
                cdp.call("Input.dispatchDragEvent", {"type": kind, "x": 50, "y": 100, "data": drag}, session=session)
        mouse("mouseReleased", 50, 100)
        time.sleep(0.3)
        check(evaluate("order()") == "bcade", "dropped where the client said: the list reordered",
              evaluate("order() + ' ' + events.join(' ')"))
        cdp.call("Input.setInterceptDrags", {"enabled": False}, session=session)

        # Plain mouse events: content runs the drag in the page.
        evaluate("events = []")
        mouse("mouseMoved", 50, 100)
        mouse("mousePressed", 50, 100, 1)
        for y in (106, 114, 60, 30, 20):
            mouse("mouseMoved", 50, y, 1)
        mouse("mouseReleased", 50, 20)
        time.sleep(0.3)
        check(evaluate("order()") == "abcde", "a drag in the page from Input.dispatchMouseEvent",
              evaluate("order() + ' ' + events.join(' ')"))
    except Exception as error:  # noqa: BLE001
        check(False, "the CDP session", repr(error))
    finally:
        engine.terminate()
        try:
            engine.wait(10)
        except subprocess.TimeoutExpired:
            engine.kill()
    print(f"{checks - len(failures)} of {checks} checks passed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
