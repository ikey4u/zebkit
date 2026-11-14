"""JavaScript libraries in a page of their own: the backend "page" of
<zebkit/library.h> (docs/JS_LIBRARIES.md, "Backend page"), through the Python
binding and, for what a binding never sends, the C functions through ctypes.

    python3 tests/engine/library_page.py [library bundles dir] [engine switches...]

The fixture tests/engine/library_page_fixture measures text in a canvas, lays
out HTML and SVG, and tries every way out of its page. Checked: the same
values and codes as the isolate's (JSON and parts, thrown errors, bad
arguments and results); a hidden page whose timers and requestAnimationFrame
are not throttled, which says it is visible; no user activation, no popup; its
own files served, nothing outside its folder; images, CSS, fonts, workers,
fetch(), WebSockets, frames, navigation, beacons, hints, downloads and forms
to an origin not granted all refused, a local server counting every
connection proving none arrived; fetch() to a granted origin, without the
application's cookies; two instances' storage apart; both deadlines (a page
that waits, one that spins and is stopped), the load's deadline, the heap's
limit, a renderer killed (GONE); cancel, a full queue, close with calls
pending; width and height. With the bundles of examples/js-libraries
(build.sh, engine-test.sh --fetch): Mermaid's diagram to SVG. Exit 0 when
every check passes.
"""

import ctypes
import http.server
import json
import os
import shutil
import signal
import socketserver
import subprocess
import sys
import tempfile
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from harness import check, finish, part, skip, wait, zebkit  # noqa: E402

import zebkit.library as L  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
FIXTURE = os.path.join(HERE, "library_page_fixture")
args = sys.argv[1:]
bundles = args.pop(0) if args and not args[0].startswith("-") else os.path.join(HERE, "..", "..", ".cache", "js-libraries")
assert zebkit.init([sys.argv[0]] + (args or ["--headless", "--no-sandbox"]),
                   settings={"logging": {"destination": "none"}}) == 0

work = tempfile.mkdtemp(prefix="zebkit-library-page-")
PAGE = {"backend": "page"}


def page(options=None):
    return dict(PAGE, **(options or {}))


def settle(*futures, seconds=30):
    return wait(lambda: all(f.done() for f in futures), seconds)


def outcome(future, seconds=30):
    """("ok", value) or (code name, error)."""
    if not settle(future, seconds=seconds):
        return ("pending", None)
    try:
        return ("ok", future.result())
    except L.LibraryError as error:
        return (error.code_name, error)


def value_of(future, seconds=30):
    kind, value = outcome(future, seconds)
    assert kind == "ok", (kind, value and getattr(value, "message", value))
    return value


told = {}


def counted(key):
    told[key] = []
    return lambda value, error: told[key].append((value, error))


def library(name, files):
    folder = os.path.join(work, name)
    for path, text in files.items():
        os.makedirs(os.path.dirname(os.path.join(folder, path)), exist_ok=True)
        with open(os.path.join(folder, path), "w", encoding="utf-8") as f:
            f.write(text)
    return folder


# ---- a server that counts every connection and request -------------------------

class Counting(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        self.server.requests.append(self.command + " " + self.path)
        if self.path == "/redirect-out":
            self.send_response(302)
            self.send_header("Location", "http://localhost:%d/hello" % self.server.server_address[1])
            self.end_headers()
            return
        body = {"/hello": "hello from the server",
                "/cookie": self.headers.get("Cookie") or "no cookie",
                "/set-cookie": "set"}.get(self.path, "other")
        data = body.encode()
        self.send_response(200)
        self.send_header("Content-Type", "text/plain")
        # The page's origin, which may send credentials: they show there are none.
        self.send_header("Access-Control-Allow-Origin", self.headers.get("Origin") or "*")
        self.send_header("Access-Control-Allow-Credentials", "true")
        if self.path == "/set-cookie":
            self.send_header("Set-Cookie", "app=1; Path=/")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    do_POST = do_GET

    def log_message(self, *args):
        pass


class CountingServer(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True

    def __init__(self):
        self.connections = 0
        self.requests = []
        super().__init__(("127.0.0.1", 0), Counting)
        threading.Thread(target=self.serve_forever, kwargs={"poll_interval": .02}, daemon=True).start()
        self.base = "http://127.0.0.1:%d" % self.server_address[1]

    def verify_request(self, request, client_address):
        self.connections += 1
        return True


# ---- the renderers of this process --------------------------------------------

def renderers():
    """The PIDs of this process's renderers (descendants with --type=renderer),
    or None where they cannot be listed."""
    table = {}
    if os.path.isdir("/proc/self"):
        for name in os.listdir("/proc"):
            if not name.isdigit():
                continue
            try:
                with open("/proc/%s/stat" % name) as f:
                    ppid = int(f.read().rsplit(")", 1)[1].split()[1])
                with open("/proc/%s/cmdline" % name, "rb") as f:
                    command = f.read().replace(b"\0", b" ").decode(errors="replace")
            except OSError:
                continue
            table[int(name)] = (ppid, command)
    elif sys.platform == "darwin":
        out = subprocess.run(["ps", "-A", "-o", "pid=,ppid=,command="], capture_output=True, text=True).stdout
        for line in out.splitlines():
            fields = line.split(None, 2)
            if len(fields) == 3:
                table[int(fields[0])] = (int(fields[1]), fields[2])
    elif os.name == "nt":
        try:
            out = subprocess.run(["powershell", "-NoProfile", "-Command",
                                  "Get-CimInstance Win32_Process | Select-Object ProcessId,ParentProcessId,CommandLine"
                                  " | ConvertTo-Json -Compress"], capture_output=True, text=True, timeout=60).stdout
            rows = json.loads(out or "[]")
        except (OSError, ValueError, subprocess.SubprocessError):
            return None
        for row in rows if isinstance(rows, list) else [rows]:
            table[int(row["ProcessId"])] = (int(row["ParentProcessId"] or 0), row.get("CommandLine") or "")
    else:
        return None

    def ours(pid):
        for _ in range(16):
            if pid == os.getpid():
                return True
            if pid not in table:
                return False
            pid = table[pid][0]
        return False
    return {pid for pid, (_, command) in table.items() if "--type=renderer" in command and ours(pid)}


# ---- load ---------------------------------------------------------------------

with part("options: width and height are the page's; refused for the isolate; instances and host refused for the page; bad values refused"):
    for options, expect in [({"width": 300}, "page"), (page({"width": 0}), "width"),
                            (page({"height": "tall"}), "height"), ({"backend": "dom"}, "backend"),
                            (page({"instances": 2}), "instances"), (page({"host": ["progress"]}), "host")]:
        try:
            L.load(FIXTURE, options)
            raise AssertionError("loaded with %r" % options)
        except zebkit.Error as error:
            assert expect in str(error), (expect, str(error))

before = renderers()
lib = L.load(FIXTURE, page({"width": 640, "height": 480}))
with part("ready: the exported functions", fatal=True):
    kind, value = outcome(lib.ready)
    assert kind == "ok" and {"echo", "measure", "layout", "svg", "timers"} <= set(value["functions"]), (kind, value)

with part("its page: zebkit-library://<id>, the size asked for, no zebkit object, the runner gone"):
    info = value_of(lib.call("page"))
    assert info["protocol"] == "zebkit-library:" and len(info["origin"]) > len("zebkit-library://") + 16, info
    assert (info["width"], info["height"]) == (640, 480), info
    assert info["zebkit"] == "undefined" and info["call"] == "undefined" and info["scripts"] == 0, info

with part("hidden, but told it is visible: timers not throttled, requestAnimationFrame runs"):
    assert info["visibility"] == "visible" and info["hidden"] is False, info
    value = value_of(lib.call("timers", 100))
    # A hidden page's nested timers wait about a second each; its frames stop.
    assert value["nested"] < 1500 and value["frames"] >= 10, value

with part("no user activation in the page; window.open refused; alert() does not block"):
    value = value_of(lib.call("activation"))
    assert value == {"isActive": False, "hasBeenActive": False, "opened": None}, value

# ---- what a page has ----------------------------------------------------------

with part("a canvas measures text: numbers, wider for more text"):
    a = value_of(lib.call("measure", "Hello", "16px sans-serif"))
    b = value_of(lib.call("measure", "Hello, world", "16px sans-serif"))
    c = value_of(lib.call("measure", "Hello", "32px sans-serif"))
    assert 10 < a["width"] < b["width"] and abs(c["width"] - 2 * a["width"]) < 2 and a["ascent"] > 5, (a, b, c)
with part("the system's fonts: serif, sans-serif and monospace measure apart"):
    value = value_of(lib.call("fonts"))
    assert value["check"] and len({round(value[k], 1) for k in ("serif", "sans", "mono")}) >= 2 and \
        min(value["serif"], value["sans"], value["mono"]) > 20, value
with part("layout and getComputedStyle"):
    value = value_of(lib.call("layout", 200))
    assert value["width"] == 220 and value["height"] > 20 and value["padding"] == "10px" and value["font"] == "20px", value
with part("SVG drawn through the DOM, its text measured (getBBox)"):
    value = value_of(lib.call("svg", "Zebra 斑马"))
    assert value["width"] > 50 and value["markup"].startswith("<svg") and "Zebra 斑马" in value["markup"], value
with part("a PNG from an OffscreenCanvas, as a part"):
    data = value_of(lib.call("png", 16))
    assert isinstance(data, bytes) and data[:8] == b"\x89PNG\r\n\x1a\n" and len(data) > 50, data[:16]

# ---- values -----------------------------------------------------------------

with part("JSON echoed, a relative import used, undefined is null, non-ASCII text"):
    obj = {"a": [1, 2.5, None, True, False], "b": {"c": "d"}, "e": "", "n": -0.125}
    assert value_of(lib.call("echo", obj, 7)) == [obj, 7]
    assert value_of(lib.call("double", 21)) == 42
    assert value_of(lib.call("nothing")) is None
    s = "日本語 · Ελληνικά · emoji 🦓🎉 · nul\u0000 · ß"
    value = value_of(lib.call("text", s))
    assert value["same"] == s and value["length"] == len(s.encode("utf-16-le")) // 2, value
with part("parts both ways: an empty one, a 1 MiB round trip"):
    assert value_of(lib.call("sizes", b"", b"\x01\x02\xff", bytearray(b"z" * 1000))) == \
        [[0, -1, -1], [3, 1, 255], [1000, 122, 122]]
    value = value_of(lib.call("bytesOut", 300))
    assert value["b"] == bytes(i & 255 for i in range(300)) and value["s"] == "x", value
    big = os.urandom(1 << 20)
    assert value_of(lib.call("roundTrip", big)) == big
for function, expect in [("thrower", ("RangeError", "too far")), ("rejecter", ("TypeError", "rejected here"))]:
    with part("THROWN: " + function):
        kind, error = outcome(lib.call(function))
        assert kind == "THROWN" and (error.name, error.message) == expect and function in error.stack, \
            (kind, error and vars(error))
with part("NO_FUNCTION, and BAD_RESULT for a BigInt and a DOM node"):
    assert outcome(lib.call("missing"))[0] == "NO_FUNCTION"
    assert outcome(lib.call("badBigInt"))[0] == "BAD_RESULT"
    assert outcome(lib.call("badNode"))[0] == "BAD_RESULT"


def raw_call(library_, function, text, parts=()):
    """zebkit_library_call() with the JSON as given: what a binding never sends."""
    array = (L._Part * len(parts))()
    buffers = [ctypes.create_string_buffer(p, len(p)) for p in parts]
    for i, p in enumerate(parts):
        array[i].data, array[i].size = ctypes.cast(buffers[i], ctypes.c_void_p).value, len(p)
    key, future = L._expect(None)
    number = L._library_call(library_._raw, function.encode(), text, array, len(parts), 0, L._told_callback,
                             ctypes.c_void_p(key))
    if not number:
        L._forget(key)
    return number, future


for name, text, parts in [("not JSON", b"[1,", ()), ("not an array", b'{"a":1}', ()),
                          ("a $part out of range", b'[{"$part":1}]', (b"a",))]:
    with part("BAD_ARGUMENTS: " + name):
        number, future = raw_call(lib, "echo", text, parts)
        assert number > 0 and outcome(future)[0] == "BAD_ARGUMENTS"
with part("a function name with a newline and quotes is only a name"):
    number, future = raw_call(lib, 'echo\n"x', b"[1]")
    assert outcome(future)[0] == "NO_FUNCTION"

# ---- its own files ---------------------------------------------------------------

with part("its own files: fetch() of a file of its folder, import() of a module"):
    value = value_of(lib.call("ownFile"))
    assert value["status"] == 200 and json.loads(value["body"]) == {"own": "file", "n": 3}, value
    assert value_of(lib.call("ownImport")) == 10
outside = library("outside", {"secret.mjs": "export const secret = 42;", "secret.json": "{\"secret\": 1}"})
linked = library("linked", {"index.mjs": open(os.path.join(FIXTURE, "index.mjs"), encoding="utf-8").read(),
                            "lib/helper.mjs": "export const twice = (n) => n * 2;\n"})
try:
    os.symlink(os.path.join(outside, "secret.json"), os.path.join(linked, "link.json"))
    os.symlink(os.path.join(outside, "secret.mjs"), os.path.join(linked, "link.mjs"))
    has_links = True
except (OSError, NotImplementedError):
    has_links = False
if has_links:
    with part("a link inside the folder to a file outside it is not served (404); an import of it fails"):
        escaped = L.load(linked, PAGE)
        value = value_of(escaped.call("getText", "./link.json"))
        assert value.get("status") == 404, value
        assert value_of(escaped.call("outsideImport", "./link.mjs")) == "refused"
        value = value_of(escaped.call("getText", "./%2e%2e/outside/secret.json"))
        assert value.get("status") == 404, value
        escaped.close()
else:
    skip("a link to a file outside the folder", "no symbolic links here")
with part("a one-file library: that file alone is served"):
    single = os.path.join(library("single", {"one.mjs": "export const hi = async () => (await fetch('./two.json')).status;",
                                             "two.json": "{}"}), "one.mjs")
    one = L.load(single, PAGE)
    assert value_of(one.call("hi")) == 404
    one.close()
with part("a load that fails: a syntax error is LOAD_FAILED, and its calls"):
    broken = L.load(library("broken", {"index.mjs": "export const = ;"}), PAGE)
    after = broken.call("anything")
    kind, error = outcome(broken.ready)
    assert kind == "LOAD_FAILED" and "SyntaxError" in str(error), (kind, str(error))
    assert outcome(after)[0] == "LOAD_FAILED"
    broken.close()

# ---- the network ----------------------------------------------------------------

server = CountingServer()
url = server.base + "/hello"
with part("refused, and nothing reaches the server: images, CSS, fonts, workers, fetch, WebSocket, frames"):
    results = {
        "image": value_of(lib.call("image", url)),
        "css": value_of(lib.call("css", url)),
        "css @import": value_of(lib.call("cssImport", url)),
        "font": value_of(lib.call("font", url)),
        "worker": value_of(lib.call("worker", url)),
        "blob worker": value_of(lib.call("blobWorker", url)),
        "own worker": value_of(lib.call("worker", "./lib/helper.mjs")),
        "shared worker": value_of(lib.call("sharedWorker", url)),
        "fetch": value_of(lib.call("getText", url)),
        "fetch no-cors": value_of(lib.call("getText", url, {"mode": "no-cors"})),
        "WebSocket": value_of(lib.call("webSocket", "ws://127.0.0.1:%d/socket" % server.server_address[1])),
        "frame": value_of(lib.call("frame", url)),
        "beacon": value_of(lib.call("beacon", url)),
        "hints": value_of(lib.call("hints", url)),
        "download": value_of(lib.call("download", url)),
        "form": value_of(lib.call("form", url)),
    }
    assert results["image"] == "error" and results["css"] == "error" and results["font"] in ("error", "timeout"), results
    assert all(results[k] != "message fetched" and not str(results[k]).startswith("message")
               for k in ("worker", "blob worker", "own worker", "shared worker")), results
    assert "error" in results["fetch"] and "error" in results["fetch no-cors"], results
    assert results["WebSocket"] in ("error", "timeout") or results["WebSocket"].startswith("threw"), results
    assert results["frame"] != "loaded hello from the server", results
with part("navigation away refused: the page stays and answers"):
    value_of(lib.call("navigate", url))
    wait(lambda: False, 0.5)
    assert value_of(lib.call("count")) >= 1
    assert value_of(lib.call("page"))["protocol"] == "zebkit-library:"
with part("the server saw no connection at all"):
    wait(lambda: False, 1.0)
    assert server.connections == 0 and server.requests == [], (server.connections, server.requests)
with part("a granted origin: fetch() reaches it, without the application's cookies; a redirect elsewhere fails"):
    # The application's own profile has a cookie for the server.
    view = zebkit.View(server.base + "/set-cookie") if hasattr(zebkit, "View") else None
    if view is not None:
        wait(lambda: len(server.requests) >= 1, 10)
    net = L.load(FIXTURE, page({"network": [server.base]}))
    value = value_of(net.call("getText", url))
    assert value == {"status": 200, "body": "hello from the server", "redirected": False}, value
    value = value_of(net.call("getText", server.base + "/cookie", {"credentials": "include"}))
    assert value.get("body") == "no cookie", value
    value = value_of(net.call("getText", server.base + "/redirect-out"))
    assert "error" in value, value
    assert value_of(net.call("image", server.base + "/image")) == "error"  # not an image, but it arrived
    assert any(r.endswith("/image") for r in server.requests), server.requests
    net.close()
    if view is not None:
        view.close()

# ---- instances apart ------------------------------------------------------------

with part("two instances: storage of their own"):
    other = L.load(FIXTURE, PAGE)
    assert value_of(lib.call("setItem", "k", "first")) is True
    assert value_of(other.call("getItem", "k")) == [None, None]
    assert value_of(lib.call("getItem", "k")) == ["first", "first"]
    assert value_of(lib.call("indexed", "k", "v1")) is None
    assert value_of(other.call("indexed", "k")) is None
    assert value_of(lib.call("indexed", "k")) == "v1"
    other.close()

# ---- deadlines, the heap, a renderer gone ------------------------------------------

with part("a call waiting past its deadline is TIMEOUT, the next one runs, the library lives"):
    waiting = lib.call("never", timeout_ms=300)
    after = lib.call("double", 4)
    kind, error = outcome(waiting)
    assert kind == "TIMEOUT" and "waited" in error.message, (kind, error and error.message)
    assert value_of(after) == 8
with part("a page that spins past its deadline is stopped: TIMEOUT, then GONE"):
    spinner = L.load(FIXTURE, PAGE)
    assert settle(spinner.ready)
    spinning = spinner.call("spin", timeout_ms=500)
    queued = spinner.call("double", 1)
    started = time.monotonic()
    kind, error = outcome(spinning)
    assert kind == "TIMEOUT" and time.monotonic() - started < 10, (kind, error and error.message)
    assert outcome(queued)[0] == "GONE" and outcome(spinner.call("double", 2))[0] == "GONE"
    spinner.close()
with part("a load that spins past its deadline fails"):
    spin_load = L.load(library("spin-load", {"index.mjs": "for (;;) {}"}), page({"timeoutMs": 500}))
    kind, error = outcome(spin_load.ready)
    assert kind == "LOAD_FAILED" and "deadline" in str(error), (kind, str(error))
    spin_load.close()
with part("the heap's limit: OUT_OF_MEMORY, then GONE"):
    hungry = L.load(FIXTURE, page({"heapLimitMB": 64}))
    assert settle(hungry.ready)
    kind, error = outcome(hungry.call("grow", timeout_ms=-1), seconds=90)
    assert kind in ("OUT_OF_MEMORY", "GONE"), (kind, error and error.message)
    if kind != "OUT_OF_MEMORY":
        check(False, "the heap's limit: told OUT_OF_MEMORY", (kind, error and error.message))
    assert outcome(hungry.call("echo", 1))[0] == "GONE"
    hungry.close()
assert value_of(lib.call("echo", "still here")) == ["still here"]

if before is None:
    skip("a renderer killed: GONE", "this system's processes are not listed here")
else:
    with part("a renderer killed: the call running and those queued GONE, the library ended"):
        known = renderers()
        victim = L.load(FIXTURE, PAGE)
        assert settle(victim.ready)
        new = renderers() - known
        assert len(new) == 1, (known, new)
        running = victim.call("never", timeout_ms=-1)
        queued = victim.call("double", 3)
        wait(lambda: False, 0.2)
        os.kill(new.pop(), getattr(signal, "SIGKILL", signal.SIGTERM))
        assert outcome(running)[0] == "GONE" and outcome(queued)[0] == "GONE"
        assert outcome(victim.call("double", 4))[0] == "GONE"
        victim.close()

# ---- cancel, queue, close ------------------------------------------------------------

with part("cancel: a queued call and a running one are CANCELLED; the next runs after"):
    slow = lib.call("later", "slow", 300)
    queued = lib.call("double", 5)
    lib.cancel(queued)
    lib.cancel(slow)
    assert outcome(queued)[0] == "CANCELLED" and outcome(slow)[0] == "CANCELLED"
    assert value_of(lib.call("double", 6)) == 12
with part("a full queue: QUEUE_FULL"):
    small = L.load(FIXTURE, page({"queue": 1}))
    first = small.call("later", 1, 300)
    wait(lambda: False, 0.2)
    second = small.call("double", 1)
    third = small.call("double", 2)
    assert outcome(third)[0] == "QUEUE_FULL" and value_of(first) == 1 and value_of(second) == 2
    small.close()
with part("close with calls pending: all GONE, exactly once"):
    closing = L.load(FIXTURE, PAGE)
    assert settle(closing.ready)
    closing.call("later", 1, 2000, function=counted("close-running"))
    for i in range(3):
        closing.call("double", i, function=counted("close-%d" % i))
    wait(lambda: False, 0.1)
    closing.close()
    keys = ["close-running"] + ["close-%d" % i for i in range(3)]
    assert wait(lambda: all(told[k] for k in keys))
    wait(lambda: False, 0.3)
    assert all(len(told[k]) == 1 and told[k][0][1].code == L.GONE for k in keys), {k: told[k] for k in keys}
with part("closed while loading: ready is GONE"):
    gone = L.load(FIXTURE, PAGE)
    gone.close()
    assert outcome(gone.ready)[0] == "GONE"
with part("many instances at once, each its own page"):
    many = [L.load(FIXTURE, PAGE) for _ in range(4)]
    futures = [m.call("double", i) for i, m in enumerate(many)]
    assert [value_of(f) for f in futures] == [0, 2, 4, 6]
    origins = {value_of(m.call("page"))["origin"] for m in many}
    assert len(origins) == 4, origins
    for m in many:
        m.close()

if before is not None:
    with part("closed instances' renderers go"):
        lib.close()
        assert wait(lambda: not (renderers() - before), 15), renderers() - before
else:
    lib.close()

# ---- a real library: Mermaid, from examples/js-libraries ------------------------------

diagram = os.path.join(bundles, "diagram")
if os.path.isfile(os.path.join(diagram, "index.mjs")):
    with part("Mermaid (examples/js-libraries/diagram): a flowchart to SVG, offline"):
        mermaid = L.load(diagram, PAGE)
        svg = value_of(mermaid.call("renderSvg", "graph TD\n  A[Start 开始] --> B{Choose}\n  B -->|yes| C[Done]\n"),
                       seconds=60)
        assert svg.startswith("<svg") and "Start 开始" in svg and svg.count("<g") > 3, svg[:300]
        mermaid.close()
else:
    skip("Mermaid", "no bundle in %s: examples/js-libraries/build.sh (engine-test.sh --fetch)" % bundles)

server.shutdown()
server.server_close()
shutil.rmtree(work, ignore_errors=True)
zebkit.shutdown()
sys.exit(finish())
