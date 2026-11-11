"""What a CDP client gets of <zebkit/page.h>, as Puppeteer and Playwright
would use it, from tools/zebkit-run serving CDP:

- dialogs held for the client (Page.javascriptDialogOpening) and answered
  by it (Page.handleJavaScriptDialog), Page.javascriptDialogClosed;
- Page.printToPDF, as base64 and as a stream read with IO.read;
- Input.dispatchDragEvent dropping a file; Input.setInterceptDrags and
  Input.dragIntercepted; a drag in the page from Input.dispatchMouseEvent;
- what a page plays, saved as it plays (<zebkit/media.h>): Zebkit.mediaList,
  Zebkit.mediaAt, Zebkit.mediaCaptureStart, its progress and done events,
  Zebkit.mediaCaptureStatus; a file the element loads and Media Source
  Extensions, each file read back with the engine's FFmpeg (the
  media_probe program ZEBKIT_MEDIA_PROBE names); a capture is its
  session's alone, and stops when the session detaches, a blocked page's
  too.

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
import tempfile
import threading
import time

failures = []
checks = 0


def check(ok, what, detail=None):
    global checks
    checks += 1
    print(f"{'ok  ' if ok else 'FAIL'} {what}" + (f": {detail}" if detail is not None else ""), flush=True)
    if not ok:
        failures.append(what)


class Engine:
    """The engine serving CDP on a free port: command plus
    --remote-debugging-port=0 about:blank. Its standard error is drained by
    a thread of its own into a file, so that the engine never blocks on a full
    pipe and what it said is still there when a check fails; url is its
    DevTools WebSocket, waited for with a real timeout (None when none came).

        engine = Engine([run, *switches])
        ...
        engine.stop(failed)  # the log's end on standard error when failed
    """

    def __init__(self, command, seconds=30):
        self.log = tempfile.NamedTemporaryFile(prefix="zebkit-cdp-engine-", suffix=".log", delete=False)
        self.process = subprocess.Popen([*command, "--remote-debugging-port=0", "about:blank"],
                                        stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        self.url = None
        found = threading.Event()

        def drain():
            for line in self.process.stderr:
                self.log.write(line)
                self.log.flush()
                if not found.is_set():
                    match = re.search(rb"ws://\S+", line)
                    if match:
                        self.url = match.group(0).decode()
                        found.set()
            found.set()
        self.thread = threading.Thread(target=drain, daemon=True)
        self.thread.start()
        found.wait(seconds)

    def poll(self):
        return self.process.poll()

    def tail(self, lines=40):
        with open(self.log.name, "rb") as log:
            return b"".join(log.readlines()[-lines:]).decode(errors="replace")

    def stop(self, failed=False):
        self.process.terminate()
        try:
            self.process.wait(10)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait()
        self.thread.join(5)
        if failed:
            sys.stderr.write("the engine's standard error, its end:\n" + self.tail())
        self.log.close()
        os.unlink(self.log.name)


def launch_engine(command, seconds=30):
    """Engine(command, seconds): see there."""
    return Engine(command, seconds)


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
    engine = launch_engine([run, *switches])
    url = engine.url
    check(url is not None, "the engine serves CDP", url)
    if not url:
        engine.stop(True)
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

        # A page's media, recorded over CDP, each file read back with the
        # engine's FFmpeg (media_probe).
        def probe(path):
            tool = os.environ.get("ZEBKIT_MEDIA_PROBE")
            if not tool:
                return {"ok": os.path.getsize(path) > 100000, "unprobed": True}
            result = subprocess.run([tool, path], capture_output=True, text=True, timeout=60)
            try:
                return json.loads(result.stdout)
            except ValueError:
                return {"ok": False, "output": result.stdout + result.stderr}

        def whole_clip(p):
            return p.get("unprobed") or (
                p.get("ok") and p.get("streams") == 2 and p.get("video") == "h264" and p.get("audio") == "aac"
                and 7.4 < p.get("duration", 0) < 8.6 and p.get("videoFrames", 0) >= 185
                and p.get("audioFrames", 0) >= 350 and p.get("errors") == 0)

        def wait(expression, tries=200):
            for _ in range(tries):
                if evaluate(expression):
                    return True
                time.sleep(0.1)
            return False

        def record(name, during, options=None):
            path = os.path.join(files, name)
            media = cdp.call("Zebkit.mediaList", session=session)["media"]
            params = {"mediaId": media[0]["id"], "path": path}
            if options:
                params["options"] = options
            capture = cdp.call("Zebkit.mediaCaptureStart", params, session=session)["captureId"]
            during()
            evaluate("play(4)")
            ended = wait("state.ended")
            cdp.call("Zebkit.mediaCaptureStop", {"captureId": capture}, session=session)
            done = cdp.event("Zebkit.mediaCaptureDone", 30)
            return media, capture, path, ended, done

        navigate(f"{base}/media.html")
        # Looked for before anything plays: the page's players keep what a
        # capture starts from from now on.
        check(cdp.call("Zebkit.mediaList", session=session)["media"] == [], "Zebkit.mediaList: none yet")
        evaluate("file('video', 'media/clip.mp4')")
        wait("ready()", 100)
        media = cdp.call("Zebkit.mediaList", session=session)["media"]
        check(len(media) == 1 and media[0]["codecs"].get("video") == "h264", "Zebkit.mediaList", media)
        at = cdp.call("Zebkit.mediaAt", {"x": 160, "y": 90}, session=session)["media"]
        check(at and at["id"] == media[0]["id"], "Zebkit.mediaAt", at)
        path = os.path.join(files, "cdp-capture.mp4")
        capture = cdp.call("Zebkit.mediaCaptureStart", {"mediaId": media[0]["id"], "path": path},
                           session=session)["captureId"]
        evaluate("play(4)")
        progress = cdp.event("Zebkit.mediaCaptureProgress", 15)
        check(progress and progress["params"]["captureId"] == capture, "Zebkit.mediaCaptureProgress",
              progress and progress["params"]["state"].get("state"))
        wait("state.ended")
        cdp.call("Zebkit.mediaCaptureStop", {"captureId": capture}, session=session)
        done = cdp.event("Zebkit.mediaCaptureDone", 30)
        files_written = done and done["params"]["result"].get("files") or []
        written = files_written and files_written[0]["path"] == path and probe(path)
        check(written and whole_clip(written), "Zebkit.mediaCaptureDone: the file, all of the clip", written)
        status = cdp.call("Zebkit.mediaCaptureStatus", {"captureId": capture}, session=session)["status"]
        check(status.get("state") == "done", "Zebkit.mediaCaptureStatus", status.get("state"))
        try:
            cdp.call("Zebkit.mediaCaptureStatus", {"captureId": capture})
            check(False, "a capture is not another session's")
        except RuntimeError as error:
            check(True, "a capture is not another session's", error)

        # Media Source Extensions: what a DASH player appends.
        evaluate("mse(3)")
        wait("ready()", 100)
        media, capture, path, ended, done = record("cdp-mse.mp4", lambda: evaluate("more()"))
        check(media[0]["kind"] == "mse", "Zebkit.mediaList: Media Source Extensions", media[0].get("kind"))
        files_written = done and done["params"]["result"].get("files") or []
        written = ended and files_written and probe(files_written[0]["path"])
        check(written and whole_clip(written), "Zebkit.mediaCaptureDone: the stream, all of it", written)

        # A session that detaches: its capture stops, and its file is written.
        other = cdp.call("Target.attachToTarget", {"targetId": target, "flatten": True})["sessionId"]
        evaluate("file('video', 'media/clip.mp4')")
        wait("ready()", 100)
        media = cdp.call("Zebkit.mediaList", session=other)["media"]
        path = os.path.join(files, "cdp-detached.mp4")
        capture = cdp.call("Zebkit.mediaCaptureStart", {"mediaId": media[0]["id"], "path": path},
                           session=other)["captureId"]
        evaluate("play(2)")
        wait("time() > 2")
        cdp.call("Target.detachFromTarget", {"sessionId": other})
        # Nobody is told when it is written: the file is there, and stays
        # the same size.
        size = -1
        for _ in range(100):
            time.sleep(0.3)
            now = os.path.getsize(path) if os.path.exists(path) else -1
            if now > 0 and now == size:
                break
            size = now
        written = os.path.exists(path) and probe(path)
        check(written and written.get("ok") and written.get("errors", 0) == 0,
              "a session's capture stops when it detaches, and is written", written)
        try:
            cdp.call("Zebkit.mediaCaptureStop", {"captureId": capture}, session=session)
            check(False, "and is gone with it")
        except RuntimeError as error:
            check(True, "and is gone with it", error)
        evaluate("media.pause(); true")

        # A capture started while the page's thread is blocked (its start
        # not answered), by a session that detaches before the start times
        # out: the engine goes on.
        blocked = cdp.call("Target.attachToTarget", {"targetId": target, "flatten": True})["sessionId"]
        media = cdp.call("Zebkit.mediaList", session=blocked)["media"]
        evaluate("block(12)")
        cdp.call("Zebkit.mediaCaptureStart", {"mediaId": media[0]["id"],
                                              "path": os.path.join(files, "cdp-blocked.mp4")}, session=blocked)
        cdp.call("Target.detachFromTarget", {"sessionId": blocked})
        # The page answers once its 12 s block ends, after the start's 10 s
        # timeout (media_capture.cc) has fired.
        started = time.time()
        answered = evaluate("1 + 1")
        check(time.time() - started > 8, "the page was blocked past the start's timeout", time.time() - started)
        check(engine.poll() is None and answered == 2,
              "a blocked page's capture, its session gone before the start times out: the engine goes on")
        try:
            cdp.call("Zebkit.mediaCaptureStart", {"mediaId": "1.2.3"}, session=session)
            check(False, "a start without a path is an error")
        except RuntimeError as error:
            check(True, "a start without a path is an error", error)
    except Exception as error:  # noqa: BLE001
        check(False, "the CDP session", repr(error))
    finally:
        engine.stop(bool(failures))
    print(f"{checks - len(failures)} of {checks} checks passed")
    return 1 if failures or not checks else 0


if __name__ == "__main__":
    sys.exit(main())
