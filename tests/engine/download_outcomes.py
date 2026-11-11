"""How downloads end (<zebkit/hooks.h>, zebkit_view_on_download_progress):
completed, with the file's final path; cancelled by the application and by
the download hook's answer; failed on the network (a response cut off) and
on the file system (a directory it may not write to), each with its
category, Chromium's interrupt reason as code and name, and a message; and
the same reports over CDP (Zebkit.setDownloadEvents,
Zebkit.downloadProgress) under the guid CDP knows the download by.

    python3 tests/engine/download_outcomes.py [engine switches...]

Exit 0 when every check passes.
"""

import http.server
import os
import shutil
import sys
import tempfile
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from harness import Server, check, ev, finish, skip, wait, zebkit  # noqa: E402
from cdp_page import Cdp  # noqa: E402

BODY = b"zebkit download " * 128
release = threading.Event()


class Pages(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def do_GET(self):
        path = self.path.split("?")[0]
        if path == "/":
            body = b"<!doctype html><title>Downloads</title>"
            self.send_response(200)
            self.send_header("Content-Type", "text/html")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        self.send_response(200)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Disposition", "attachment; filename=\"%s.bin\"" % path.strip("/"))
        if path in ("/whole", "/denied", "/refused", "/cdp"):
            self.send_header("Content-Length", str(len(BODY)))
            self.end_headers()
            self.wfile.write(BODY)
        elif path == "/held":
            # Some bytes, then nothing until the test is done.
            self.send_header("Content-Length", str(len(BODY) * 1000))
            self.end_headers()
            self.wfile.write(BODY)
            self.wfile.flush()
            release.wait(30)
        elif path == "/cut":
            # Chunked, cut off in the middle of a chunk.
            self.send_header("Transfer-Encoding", "chunked")
            self.end_headers()
            self.wfile.write(b"%x\r\n%s\r\n" % (len(BODY), BODY))
            self.wfile.write(b"%x\r\n%s" % (len(BODY), BODY[:10]))
            self.wfile.flush()
            time.sleep(.2)
            self.connection.shutdown(2)
            self.close_connection = True
        else:
            self.send_error(404)

    def log_message(self, *args):
        pass


def start(view, path):
    ev(view, "(() => { const a = document.createElement('a'); a.href = '%s'; a.download = ''; "
             "document.body.append(a); a.click(); return 1; })()" % path)


def in_thread(function, ready, seconds=30):
    box = []

    def run():
        try:
            box.append((function(), None))
        except Exception as error:  # noqa: BLE001
            box.append((None, error))
            ready.set()
    threading.Thread(target=run, daemon=True).start()
    return box


def main():
    if zebkit.init(["download-outcomes"] + sys.argv[1:]) != 0:
        print("FAIL zebkit.init()")
        return 1
    work = tempfile.mkdtemp(prefix="zebkit-downloads-")
    locked = os.path.join(work, "locked")
    os.mkdir(locked)
    os.chmod(locked, 0o500)
    server = Server(Pages)
    view = zebkit.View(server.base + "/", 600, 400)
    # The page itself, not the blank document before it.
    wait(lambda: ev(view, "location.pathname + document.readyState") == "/complete")

    reports = []
    view.on_download_progress(reports.append)
    asked = {}

    def decide(request):
        name = request.metadata["suggestedName"]
        asked[request.metadata["downloadId"]] = name
        if name == "refused.bin":
            request.respond("cancel")
        elif name == "denied.bin":
            request.respond("save", os.path.join(locked, name))
        else:
            request.respond("save", os.path.join(work, name))
    view.on_download(decide)

    def last(name, outcomes=("complete", "cancelled", "failed")):
        ids = [i for i, n in asked.items() if n == name]
        done = [r for r in reports if ids and r.get("downloadId") == ids[0] and r.get("outcome") in outcomes]
        return done[-1] if done else None

    # Completed: its saved file, no error.
    start(view, "/whole")
    wait(lambda: last("whole.bin"), 15)
    whole = last("whole.bin") or {}
    saved = whole.get("savedPath") or ""
    check(whole.get("outcome") == "complete" and whole.get("state") == "complete" and whole.get("error") is None,
          "complete, without an error", whole or (asked, reports[-3:]))
    check(saved == os.path.join(work, "whole.bin") and os.path.isfile(saved)
          and open(saved, "rb").read() == BODY, "savedPath is the whole file", saved)
    check(bool(whole.get("guid")) and whole.get("mimeType") == "application/octet-stream", "guid and mimeType", whole)
    progress = [r for r in reports if r.get("outcome") == "in-progress"]
    check(all(r.get("savedPath") is None and r.get("error") is None for r in progress),
          "reports in progress have no savedPath or error")

    # Cancelled by the application while in progress.
    start(view, "/held")
    wait(lambda: any(r.get("outcome") == "in-progress" and r.get("receivedBytes", 0) > 0 and
                     asked.get(r.get("downloadId")) == "held.bin" for r in reports), 15)
    held_id = next((i for i, n in asked.items() if n == "held.bin"), "")
    check(view.cancel_download(held_id), "zebkit_view_cancel_download()")
    wait(lambda: last("held.bin"), 10)
    held = last("held.bin") or {}
    error = held.get("error") or {}
    check(held.get("outcome") == "cancelled" and error.get("category") == "cancelled" and
          error.get("code") == 40 and error.get("name") == "USER_CANCELED" and error.get("message") == "Cancelled"
          and held.get("savedPath") is None, "cancelled by the application", held)
    release.set()

    # Cancelled by the hook's answer.
    start(view, "/refused")
    wait(lambda: last("refused.bin"), 10)
    refused = last("refused.bin") or {}
    check(refused.get("outcome") == "cancelled" and (refused.get("error") or {}).get("category") == "cancelled",
          "cancelled by the download hook", refused)

    # Failed on the network: the response cut off.
    start(view, "/cut")
    wait(lambda: last("cut.bin"), 15)
    cut = last("cut.bin") or {}
    error = cut.get("error") or {}
    check(cut.get("outcome") == "failed" and error.get("category") == "network" and
          error.get("name", "").startswith("NETWORK_") and 20 <= error.get("code", 0) <= 24 and
          bool(error.get("message")), "failed on the network", cut)

    # Failed on the file system: a directory it may not write to.
    if os.geteuid() == 0:
        skip("a file-system failure", "root writes to any directory")
    else:
        start(view, "/denied")
        wait(lambda: last("denied.bin"), 15)
        denied = last("denied.bin") or {}
        error = denied.get("error") or {}
        check(denied.get("outcome") == "failed" and error.get("category") == "file" and
              error.get("name", "").startswith("FILE_") and error.get("message"), "failed on the file system", denied)

    # Over CDP: the page's session gets the same reports.
    zebkit.cdp_listen("127.0.0.1", 0)
    target = view.target_id()
    url = zebkit.cdp_url()
    enabled = threading.Event()

    def over_cdp():
        cdp = Cdp(url)
        session = cdp.call("Target.attachToTarget", {"targetId": target, "flatten": True})["sessionId"]
        cdp.call("Zebkit.setDownloadEvents", {"enabled": True}, session)
        enabled.set()
        events = []
        end = time.time() + 20
        while time.time() < end:
            event = cdp.event("Zebkit.downloadProgress", 5)
            if event:
                events.append(event["params"])
                if event["params"].get("done"):
                    break
        return events
    box = in_thread(over_cdp, enabled)
    wait(lambda: enabled.is_set(), 15)
    start(view, "/cdp")
    wait(lambda: box, 25)
    events, error = box[0] if box else (None, "no answer")
    if check(error is None and events, "Zebkit.downloadProgress events", error):
        done = events[-1]
        mine = last("cdp.bin") or {}
        check(done.get("outcome") == "complete" and done.get("savedPath") == os.path.join(work, "cdp.bin"),
              "Zebkit.downloadProgress: complete, with its savedPath", done)
        check(done.get("guid") == mine.get("guid") and done.get("downloadId") == mine.get("downloadId"),
              "the same download as the C API's, under CDP's guid", (done.get("guid"), mine.get("guid")))

    view.close()
    server.close()
    os.chmod(locked, 0o700)
    shutil.rmtree(work, ignore_errors=True)
    zebkit.shutdown()
    return finish()


if __name__ == "__main__":
    sys.exit(main())
