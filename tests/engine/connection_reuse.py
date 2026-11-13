"""Connection reuse (docs/CONNECTION_REUSE.md) through the Python binding: the
HTTP cache on disk under the profile, kept across a restart, in memory or off
as the startup setting httpCache says, emptied by zebkit_clear_http_cache();
zebkit_preconnect() and the startup setting preconnect, whose connections a
local server counts; zebkit_fetch_in() with a context's cookies and
connections. Each engine is a run of this script of its own (--child).

    python3 tests/engine/connection_reuse.py [engine switches...]
"""

import http.server
import json
import os
import pathlib
import subprocess
import sys
import tempfile
import threading

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

from harness import check, finish, must_wait, part, program, pump  # noqa: E402

GIF = bytes.fromhex("47494638396101000100800000ffffff00000021f90401000000002c00000000010001000002024401003b")


class Counted(http.server.ThreadingHTTPServer):
    """What reached it: requests by path, and connections accepted."""
    daemon_threads = True

    def __init__(self):
        super().__init__(("127.0.0.1", 0), Handler)
        self.requests = {}
        self.accepted = 0
        self.lock = threading.Lock()

    def get_request(self):
        made = super().get_request()
        with self.lock:
            self.accepted += 1
        return made


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *args):
        pass

    def do_GET(self):
        path = self.path.split("?")[0]
        with self.server.lock:
            self.server.requests[path] = self.server.requests.get(path, 0) + 1
        headers = {"Cache-Control": "no-store"}
        if path == "/page":
            body, kind = b"<!doctype html><title>page</title><img src=/cached.gif>", "text/html"
        elif path == "/cached.gif":
            body, kind = GIF, "image/gif"
            headers = {"Cache-Control": "max-age=3600"}
        elif path == "/set-cookie":
            body, kind = b"<!doctype html><title>cookie</title>", "text/html"
            headers["Set-Cookie"] = "context=yes; Path=/; Max-Age=3600"
        elif path == "/cookie":
            body, kind = (self.headers.get("Cookie") or "").encode(), "text/plain"
        else:
            self.send_error(404)
            return
        self.send_response(200)
        self.send_header("Content-Type", kind)
        self.send_header("Content-Length", str(len(body)))
        for name, value in headers.items():
            self.send_header(name, value)
        self.end_headers()
        self.wfile.write(body)


# ---- a child: one engine ----

def child(mode, base, profile, switches):
    from harness import zebkit
    settings = {"logging": {"destination": "none"}, "profileDirectory": profile}
    if mode == "settings":
        # Refused before anything starts: then a start that works.
        for bad, why in (({"httpCache": 1}, "not an object"), ({"httpCache": {"storage": "tape"}}, "a storage"),
                         ({"httpCache": {"maxSize": -1}}, "a negative size"),
                         ({"httpCache": {"maxSize": 1.5}}, "a size not an integer"),
                         ({"httpCache": {"keep": True}}, "an unknown key"),
                         ({"preconnect": "https://a.test"}, "preconnect not a list"),
                         ({"preconnect": ["ftp://a.test"]}, "a scheme not http or https"),
                         ({"preconnect": ["https://"]}, "no host"), ({"preconnect": [1]}, "not a string")):
            try:
                zebkit.init(["connection-reuse", *switches], settings={**settings, **bad})
                refused = None
            except zebkit.Error as error:
                refused = str(error)
            check(refused is not None, "the settings refuse %s" % why, refused)
        check(not os.path.exists(profile), "and the refused settings made nothing")
        return finish()
    if mode == "memory":
        settings["httpCache"] = {"storage": "memory", "maxSize": 1 << 20}
    elif mode == "off":
        settings["httpCache"] = {"storage": "off"}
    elif mode == "startup-preconnect":
        settings["preconnect"] = [base + "/", base.replace("127.0.0.1", "localhost") + "/"]
    if zebkit.init(["connection-reuse", *switches], settings=settings) != 0:
        print("FAIL zebkit.init()")
        return 1
    loaded = []

    def load(url, context=None):
        view = zebkit.View(url, 400, 300, context=context)
        view.on_load(lambda event, at: loaded.append((view, event, at)))
        must_wait(lambda: (view, zebkit.LOAD_FINISHED, url) in loaded, "the load of " + url)
        return view

    def fetch(url, context=None):
        box = []
        if context is None:
            zebkit.fetch(url, box.append)
        else:
            zebkit.fetch_in(context, url, box.append)
        must_wait(lambda: box, "the answer of " + url)
        return box[0]

    with part(mode):
        if mode in ("disk", "memory", "off", "restart", "restart-memory"):
            load(base + "/page")
            if mode == "off":
                load(base + "/page")
        if mode == "clear":
            load(base + "/page")
            done = []
            check(zebkit.clear_http_cache(lambda: done.append(True)), "clear_http_cache() takes the request")
            must_wait(lambda: done, "the cache cleared")
            load(base + "/page")
            context = zebkit.Context()
            load(base + "/page", context)
            done.clear()
            check(zebkit.clear_http_cache(lambda: done.append(True), context=context),
                  "clear_http_cache() of a context takes the request")
            must_wait(lambda: done, "the context's cache cleared")
            load(base + "/page", context)
        if mode == "preconnect":
            check(all(not zebkit.preconnect(url, count) for url, count in
                      ((base, 0), (base, 7), ("ftp://127.0.0.1/", 1), ("not a URL", 1), ("https://", 1))),
                  "preconnect() refuses a count outside 1 to 6, a scheme not http(s), what is not a URL")
            check(zebkit.preconnect(base + "/anything", 2), "preconnect() to the engine's profile")
            pump(1.5)
            print(json.dumps({"phase": "engine"}), flush=True)
            sys.stdin.readline()
            context = zebkit.Context()
            check(zebkit.preconnect(base, 1, context=context), "preconnect() in a context")
            pump(1.5)
            print(json.dumps({"phase": "context"}), flush=True)
            sys.stdin.readline()
            context.close()
            try:
                zebkit.preconnect(base, 1, context=context)
                closed = None
            except zebkit.Error as error:
                closed = str(error)
            check(closed is not None, "preconnect() in a closed context is refused", closed)
        if mode == "startup-preconnect":
            pump(1.5)
            print(json.dumps({"phase": "started"}), flush=True)
            sys.stdin.readline()
        if mode == "fetch-in":
            context = zebkit.Context()
            load(base + "/set-cookie", context)
            inside = fetch(base + "/cookie", context)
            outside = fetch(base + "/cookie")
            check(inside["status"] == 200 and inside["body"] == b"context=yes",
                  "fetch_in() sends the context's cookies", inside)
            check(outside["status"] == 200 and outside["body"] == b"",
                  "fetch() sends the engine profile's, not the context's", outside)
            bad = fetch("not a URL", context)
            check(bad["status"] == 0 and bad["error"], "fetch_in() of what is not a URL answers an error", bad)
            context.close()
            try:
                zebkit.fetch_in(context, base + "/cookie", lambda response: None)
                closed = None
            except zebkit.Error as error:
                closed = str(error)
            check(closed is not None, "fetch_in() in a closed context is refused", closed)
    zebkit.shutdown()
    return finish()


# ---- the test: the children, and what reached the server ----

def main():
    if len(sys.argv) > 1 and sys.argv[1] == "--child":
        return child(sys.argv[2], sys.argv[3], sys.argv[4], sys.argv[5:])
    switches = [a for a in sys.argv[1:] if not a.startswith("--user-data-dir")]
    server = Counted()
    threading.Thread(target=server.serve_forever, kwargs={"poll_interval": .02}, daemon=True).start()
    base = "http://127.0.0.1:%d" % server.server_address[1]
    command = lambda mode, profile: [sys.executable, "-I", "-B", os.path.abspath(__file__), "--child", mode, base,
                                     str(profile), *switches]
    requests = lambda path: server.requests.get(path, 0)

    def reset():
        with server.lock:
            server.requests.clear()
            server.accepted = 0

    with tempfile.TemporaryDirectory(prefix="zebkit-connection-reuse-") as directory:
        root = pathlib.Path(directory)
        disk, memory = root / "disk", root / "memory"
        program(command("settings", root / "refused"), "startup settings: httpCache and preconnect checked")

        with part("disk: the cache under the profile, kept across a restart"):
            program(command("disk", disk), "a first run")
            assert requests("/cached.gif") == 1, server.requests
            assert (disk / "Cache" / "Cache_Data").is_dir(), sorted(p.name for p in disk.iterdir())
            assert (disk / "Network").is_dir(), sorted(p.name for p in disk.iterdir())
            reset()
            program(command("restart", disk), "the run after")
            assert requests("/page") == 1 and requests("/cached.gif") == 0, server.requests
        with part("memory: no cache on disk, nothing kept across a restart"):
            reset()
            program(command("memory", memory), "a first run")
            program(command("memory", memory), "the run after")
            assert requests("/cached.gif") == 2, server.requests
            assert not (memory / "Cache").exists(), sorted(p.name for p in memory.iterdir())
        with part("off: nothing cached"):
            reset()
            program(command("off", root / "off"), "two views in one run")
            assert requests("/cached.gif") == 2, server.requests
        with part("clear_http_cache(): the engine's profile and a context forget"):
            reset()
            program(command("clear", root / "clear"), "load, clear, load again")
            assert requests("/cached.gif") == 4, server.requests

        def stepped(mode, phases, what):
            """A child that stops at each phase until told to go on: the
            connections the server accepted meanwhile."""
            reset()
            process = subprocess.Popen(command(mode, root / mode), stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                       stderr=subprocess.PIPE, text=True)
            counts = []
            for phase in phases:
                line = process.stdout.readline()
                while line and not line.startswith("{"):
                    print("  | " + line.rstrip(), flush=True)
                    line = process.stdout.readline()
                if not line:
                    break
                assert json.loads(line)["phase"] == phase, line
                with server.lock:
                    counts.append((server.accepted, sum(server.requests.values())))
                process.stdin.write("\n")
                process.stdin.flush()
            out, err = process.communicate(timeout=60)
            ok = process.returncode == 0 and "FAIL" not in out
            for line in out.splitlines():
                print("  | " + line, flush=True)
            if not ok:
                print(err[-3000:], flush=True)
            check(ok, what)
            return counts

        with part("preconnect(): connections, no request"):
            counts = stepped("preconnect", ("engine", "context"), "preconnect() in the engine and a context")
            assert counts[0] == (2, 0) and counts[1] == (3, 0), counts
        with part("the startup setting preconnect: a connection to each, no request"):
            counts = stepped("startup-preconnect", ("started",), "preconnect at startup")
            assert counts[0] == (2, 0), counts
        with part("fetch_in(): the context's cookies and connections"):
            reset()
            program(command("fetch-in", root / "fetch-in"), "fetch_in() in a context")
    server.shutdown()
    return finish()


if __name__ == "__main__":
    sys.exit(main())
