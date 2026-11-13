"""JavaScript libraries as normal libraries (<zebkit/library.h>,
docs/JS_LIBRARIES.md), through the Python binding and, for what a binding
never sends, the C functions through ctypes.

    python3 tests/engine/library.py [library bundles dir] [engine switches...]

Load errors; calls with JSON, non-ASCII text and binary parts; each code: a
thrown error and a rejected promise, no function, bad arguments and results,
both deadlines (a script that never ends stops the library, a call waiting on
a timer is TIMEOUT and the next runs), cancel of a waiting and of a running
call, a full queue, heap exhaustion; imports that leave the folder; fetch()
refused without a grant and allowed with one (a local server), no cookies, a
redirect to another origin refused; close with calls pending; many libraries
at once; calls from another thread; exactly one outcome each. With the bundles
of examples/js-libraries (build.sh, engine-test.sh --fetch): markdown-it and
ECharts. Exit 0 when every check passes.
"""

import ctypes
import http.server
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from harness import Server, check, finish, part, skip, wait, zebkit  # noqa: E402

import zebkit.library as L  # noqa: E402

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
args = sys.argv[1:]
bundles = args.pop(0) if args and not args[0].startswith("-") else os.path.join(ROOT, ".cache", "js-libraries")
assert zebkit.init([sys.argv[0]] + (args or ["--headless", "--no-sandbox"]),
                   settings={"logging": {"destination": "none"}}) == 0

work = tempfile.mkdtemp(prefix="zebkit-library-")


def library(name, files):
    """A folder of files: {relative path: text}."""
    folder = os.path.join(work, name)
    for path, text in files.items():
        os.makedirs(os.path.dirname(os.path.join(folder, path)), exist_ok=True)
        with open(os.path.join(folder, path), "w", encoding="utf-8") as f:
            f.write(text)
    return folder


def settle(*futures, seconds=20):
    return wait(lambda: all(f.done() for f in futures), seconds)


def outcome(future, seconds=20):
    """("ok", value) or (code name, error)."""
    if not settle(future, seconds=seconds):
        return ("pending", None)
    try:
        return ("ok", future.result())
    except L.LibraryError as error:
        return (error.code_name, error)


# Outcomes by key, counted: each is told exactly once.
told = {}


def counted(key):
    told[key] = []
    return lambda value, error: told[key].append((value, error))


MAIN = library("main", {"index.mjs": r"""
import {twice} from './lib/helper.mjs';
export const echo = (...args) => args;
export const double = (n) => twice(n);
export function thrower() { const e = new RangeError('too far'); throw e; }
export async function rejecter() { await null; throw new TypeError('rejected here'); }
export const throwString = () => { throw 'just a string'; };
export const nothing = () => undefined;
export const later = (value, ms) => new Promise((resolve) => setTimeout(() => resolve(value), ms));
export const never = () => new Promise(() => {});
export const spin = () => { for (;;) {} };
export const bytesOut = (n) => { const b = new Uint8Array(n); for (let i = 0; i < n; ++i) b[i] = i & 255; return {b, s: 'x'}; };
export const sizes = (...parts) => parts.map((p) => p instanceof Uint8Array ? [p.length, p[0] ?? -1, p[p.length - 1] ?? -1] : null);
export const roundTrip = (bytes) => bytes;
export const badFunction = () => ({f() {}});
export const badBigInt = () => 1n;
export const badNaN = () => [NaN];
export const badMap = () => new Map();
export const badCycle = () => { const a = {}; a.self = a; return a; };
export const badPart = () => ({$part: 0});
export const deep = (n) => { let v = 1; for (let i = 0; i < n; ++i) v = [v]; return v; };
export const text = (s) => ({length: s.length, upper: s.toUpperCase(), same: s});
export function grow() { const keep = []; for (;;) keep.push(new Array(100000).fill(Math.random())); }
let counter = 0;
export const count = () => ++counter;
export async function get(url, init) {
  const r = await fetch(url, init);
  return {status: r.status, body: await r.text(), redirected: r.redirected, type: r.headers.get('content-type')};
}
export function web() {
  const u = new URL('../a/b?x=1&y=%C3%BC#h', 'https://example.org:8443/p/q');
  u.searchParams.append('z', 'a b');
  const signal = AbortSignal.abort();
  return {
    href: u.href, host: u.host, origin: u.origin, y: u.searchParams.get('y'),
    text: new TextDecoder().decode(new TextEncoder().encode('日本語 €𝄞')),
    stream: (() => { const d = new TextDecoder(), b = new TextEncoder().encode('é'); return d.decode(b.subarray(0, 1), {stream: true}) + d.decode(b.subarray(1)); })(),
    b64: atob(btoa('zebkit')), clone: structuredClone({d: [1, {e: 2}]}),
    uuid: /^[0-9a-f-]{36}$/.test(crypto.randomUUID()), random: crypto.getRandomValues(new Uint8Array(8)).length,
    now: performance.now() >= 0, intl: new Intl.DateTimeFormat('en-US', {timeZone: 'UTC'}).format(new Date(0)),
    aborted: signal.aborted, reason: signal.reason.name, wasm: WebAssembly.validate(new Uint8Array([0, 97, 115, 109, 1, 0, 0, 0])),
    noZebkit: typeof zebkit, noRequire: typeof require, noDocument: typeof document, noProcess: typeof process,
  };
}
""", "lib/helper.mjs": "export const twice = (n) => n * 2;\n"})

# ---- load errors ------------------------------------------------------------

with part("load errors: bad path, folder without index.mjs, bad options"):
    for path, options, expect in [
            (os.path.join(work, "missing"), None, "no library"),
            (library("empty", {"other.mjs": "export const x = 1;"}), None, "index.mjs"),
            (MAIN, {"timeoutMs": "soon"}, "timeoutMs"),
            (MAIN, {"what": 1}, "unknown option"),
            (MAIN, {"network": ["not a url"]}, "network"),
            (MAIN, {"network": ["https://example.com/path"]}, "network"),
            (MAIN, {"heapLimitMB": 1}, "heapLimitMB"),
            (MAIN, {"backend": "page"}, "page")]:
        try:
            L.load(path, options)
            raise AssertionError("loaded: %r %r" % (path, options))
        except zebkit.Error as error:
            assert expect in str(error), (expect, str(error))

for name, files, expect in [
        ("syntax", {"index.mjs": "export const = ;"}, "SyntaxError"),
        ("throws", {"index.mjs": "throw new Error('at load');"}, "at load"),
        ("rejects", {"index.mjs": "await Promise.reject(new Error('awaited'));"}, "awaited"),
        ("missing-import", {"index.mjs": "import './nowhere.mjs';"}, "no such file")]:
    with part("load fails: " + name):
        lib = L.load(library(name, files))
        after = lib.call("anything")
        kind, error = outcome(lib.ready)
        assert kind == "LOAD_FAILED" and expect in str(error), (kind, str(error))
        kind, error = outcome(after)
        assert kind == "LOAD_FAILED", (kind, error)
        lib.close()

with part("a load past its deadline (a top level that never ends) fails"):
    lib = L.load(library("spin-load", {"index.mjs": "for (;;) {}"}), {"timeoutMs": 300})
    kind, error = outcome(lib.ready)
    assert kind == "LOAD_FAILED" and "deadline" in str(error), (kind, str(error))
    lib.close()

# ---- imports confined to the folder ------------------------------------------

outside = library("outside", {"secret.mjs": "export const secret = 42;"})
cases = [
    ("parent", {"index.mjs": "import {secret} from '../outside/secret.mjs'; export const s = () => secret;"}, "outside"),
    ("bare", {"index.mjs": "import fs from 'fs'; export const f = () => fs;"}, "./ or ../"),
    ("absolute", {"index.mjs": "import {secret} from '%s'; export const s = () => secret;" %
                  os.path.join(outside, "secret.mjs").replace("\\", "/")}, "./ or ../"),
]
for name, files, expect in cases:
    with part("a forbidden import fails the load: " + name):
        lib = L.load(library("import-" + name, files))
        kind, error = outcome(lib.ready)
        assert kind == "LOAD_FAILED" and expect in str(error), (kind, str(error))
        lib.close()


def make_link(target, link, kind):
    """A link at link to target, of kind "symlink" or (Windows) "junction":
    None, or why this machine cannot make one. A copy or a hard link would be
    a file inside the folder, no escape, so neither stands in for a link."""
    try:
        if kind == "junction":
            import _winapi
            _winapi.CreateJunction(target, link)
        else:
            os.symlink(target, link, target_is_directory=os.path.isdir(target))
    except (OSError, NotImplementedError, AttributeError) as error:
        return "no %s: %s" % (kind, error)
    return None


def leads_to(link, target):
    """Really a way out: the link resolves to the target."""
    return os.path.normcase(os.path.realpath(link)) == os.path.normcase(os.path.realpath(target))


link_cases = [("a file", "symlink", os.path.join(outside, "secret.mjs"), "link.mjs", "./link.mjs"),
              ("a folder", "symlink", outside, "dir", "./dir/secret.mjs")]
if os.name == "nt":
    link_cases.append(("a folder (a junction)", "junction", outside, "junction", "./junction/secret.mjs"))
for what, kind, target, name, specifier in link_cases:
    title = "a %s inside the folder to %s outside it is refused" % ("junction" if kind == "junction" else "link", what)
    folder = library("import-link-" + name, {
        "index.mjs": "import {secret} from '%s'; export const s = () => secret;" % specifier})
    why = make_link(target, os.path.join(folder, name), kind)
    if why:
        skip(title, why)
        continue
    with part(title):
        assert leads_to(os.path.join(folder, name), target), (name, os.path.realpath(os.path.join(folder, name)))
        lib = L.load(folder)
        kind_, error = outcome(lib.ready)
        assert kind_ == "LOAD_FAILED" and "outside" in str(error), (kind_, str(error))
        lib.close()
# The library itself through a link: its folder is where the link leads, and a
# file there is inside it.
real = library("linked-real", {"index.mjs": "import {v} from './v.mjs'; export const get = () => v;",
                               "v.mjs": "export const v = 7;"})
via = os.path.join(work, "linked-via")
why = make_link(real, via, "symlink")
if why and os.name == "nt":
    why = make_link(real, via, "junction")
if why:
    skip("a library loaded through a link to its folder", why)
else:
    with part("a library loaded through a link to its folder works: its folder is the real one"):
        assert leads_to(via, real), os.path.realpath(via)
        lib = L.load(via)
        assert outcome(lib.call("get")) == ("ok", 7)
        lib.close()
with part("a one-file library loads, and imports nothing"):
    single = os.path.join(library("single", {"one.mjs": "export const hi = () => 'hi';"}), "one.mjs")
    lib = L.load(single)
    assert outcome(lib.call("hi")) == ("ok", "hi")
    lib.close()
    lib = L.load(os.path.join(library("single-import", {"one.mjs": "import './two.mjs';", "two.mjs": ""}), "one.mjs"))
    kind, error = outcome(lib.ready)
    assert kind == "LOAD_FAILED" and "one file" in str(error), (kind, str(error))
    lib.close()

# ---- calls -------------------------------------------------------------------

lib = L.load(MAIN)
with part("ready: the exported functions", fatal=True):
    kind, value = outcome(lib.ready)
    assert kind == "ok" and {"echo", "double", "spin", "web"} <= set(value["functions"]), (kind, value)
with part("JSON echoed, a relative import used, undefined is null"):
    value = {"a": [1, 2.5, None, True, False], "b": {"c": "d"}, "e": "", "n": -0.125}
    assert outcome(lib.call("echo", value, 7)) == ("ok", [value, 7])
    assert outcome(lib.call("double", 21)) == ("ok", 42)
    assert outcome(lib.call("nothing")) == ("ok", None)
with part("non-ASCII text both ways"):
    s = "日本語 · Ελληνικά · emoji 🦓🎉 · nul\u0000 · ß"
    kind, value = outcome(lib.call("text", s))
    assert kind == "ok" and value["same"] == s and value["length"] == len(s.encode("utf-16-le")) // 2 and \
        value["upper"] == s.upper().replace("SS", "SS"), value
with part("parts both ways: bytes in arguments, Uint8Arrays in results, 1 MiB round trip"):
    kind, value = outcome(lib.call("sizes", b"", b"\x01\x02\xff", bytearray(b"z" * 1000)))
    assert kind == "ok" and value == [[0, -1, -1], [3, 1, 255], [1000, 122, 122]], value
    kind, value = outcome(lib.call("bytesOut", 300))
    assert kind == "ok" and value["b"] == bytes(i & 255 for i in range(300)) and value["s"] == "x", value
    big = os.urandom(1 << 20)
    assert outcome(lib.call("roundTrip", big)) == ("ok", big)
with part("Web APIs: URL, text, base64, structuredClone, crypto, Intl, AbortSignal, WebAssembly; no zebkit"):
    kind, value = outcome(lib.call("web"))
    assert kind == "ok", value
    assert value["href"] == "https://example.org:8443/a/b?x=1&y=%C3%BC&z=a+b#h" and value["host"] == "example.org:8443" \
        and value["origin"] == "https://example.org:8443" and value["y"] == "ü", value
    assert value["text"] == "日本語 €𝄞" and value["stream"] == "é" and value["b64"] == "zebkit", value
    assert value["clone"] == {"d": [1, {"e": 2}]} and value["uuid"] and value["random"] == 8 and value["now"], value
    assert value["intl"] == "1/1/1970" and value["aborted"] and value["reason"] == "AbortError" and value["wasm"], value
    assert [value[k] for k in ("noZebkit", "noRequire", "noDocument", "noProcess")] == ["undefined"] * 4, value

for function, expect in [("thrower", ("RangeError", "too far")), ("rejecter", ("TypeError", "rejected here")),
                         ("throwString", ("", "just a string"))]:
    with part("THROWN: " + function):
        kind, error = outcome(lib.call(function))
        assert kind == "THROWN" and (error.name, error.message) == expect, (kind, error and vars(error))
        if expect[0]:
            assert function in error.stack, error.stack
with part("NO_FUNCTION"):
    kind, error = outcome(lib.call("missing"))
    assert kind == "NO_FUNCTION", kind
for function in ["badFunction", "badBigInt", "badNaN", "badMap", "badCycle", "badPart"]:
    with part("BAD_RESULT: " + function):
        kind, error = outcome(lib.call(function))
        assert kind == "BAD_RESULT", (kind, error)
with part("depth: 199 nested arrays pass, 300 are BAD_RESULT"):
    assert outcome(lib.call("deep", 150))[0] == "ok"
    assert outcome(lib.call("deep", 300))[0] == "BAD_RESULT"


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
                          ("a $part with another key", b'[{"$part":0,"x":1}]', (b"a",)),
                          ("a $part out of range", b'[{"$part":1}]', (b"a",)),
                          ("nested deeper than 200", b"[" * 300 + b"]" * 300, ())]:
    with part("BAD_ARGUMENTS: " + name):
        number, future = raw_call(lib, "echo", text, parts)
        assert number > 0
        kind, error = outcome(future)
        assert kind == "BAD_ARGUMENTS", (kind, error)
with part("C: NULL arguments are [], a NULL function is refused (0) and never told"):
    number, future = raw_call(lib, "echo", None)
    assert outcome(future) == ("ok", [])
    key, never_told = L._expect(None)
    assert L._library_call(lib._raw, None, None, None, 0, 0, L._told_callback, ctypes.c_void_p(key)) == 0
    wait(lambda: False, 0.3)
    assert not never_told.done()
    L._forget(key)
with part("an outcome is told after the call returns, on the engine's thread"):
    seen = []
    lib.call("double", 1, function=lambda value, error: seen.append((value, threading.get_ident())))
    assert seen == []
    assert wait(lambda: seen) and seen[0] == (2, threading.get_ident())

with part("calls run one at a time, in order"):
    futures = [lib.call("later", i, 30 - i * 5) for i in range(5)] + [lib.call("count") for _ in range(3)]
    assert settle(*futures)
    assert [f.result() for f in futures[:5]] == list(range(5))
    assert [f.result() for f in futures[5:]] == [1, 2, 3]

with part("from another thread: calls and their outcomes"):
    results = []

    def worker():
        for i in range(20):
            lib.call("double", i, function=lambda value, error: results.append(value))
    threads = [threading.Thread(target=worker) for _ in range(4)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    assert wait(lambda: len(results) == 80), len(results)
    assert sorted(results) == sorted(2 * i for i in range(20) for _ in range(4))

# ---- cancel, deadlines, queue -------------------------------------------------

with part("cancel: a waiting call never runs, told CANCELLED once"):
    first = lib.call("later", "first", 300)
    queued = lib.call("count", function=counted("queued"))
    lib.cancel(queued)
    lib.cancel(queued)
    assert outcome(first) == ("ok", "first")
    wait(lambda: False, 0.2)
    assert len(told["queued"]) == 1 and told["queued"][0][1].code == L.CANCELLED, told["queued"]
    before = outcome(lib.call("count"))[1]
    assert before == 4, before  # the cancelled count() never ran
with part("cancel: a running call is told at once; the next runs once it settles"):
    running = lib.call("later", "late", 400, function=counted("running"))
    after = lib.call("echo", "after")
    started = time.monotonic()
    wait(lambda: False, 0.05)
    lib.cancel(running)
    assert wait(lambda: told["running"], 5)
    assert told["running"][0][1].code == L.CANCELLED and time.monotonic() - started < 0.35, told["running"]
    assert outcome(after) == ("ok", ["after"])
    assert time.monotonic() - started >= 0.35
    wait(lambda: False, 0.2)
    assert len(told["running"]) == 1
with part("Future.cancel() cancels the call"):
    future = lib.call("never", timeout_ms=200)
    assert future.cancel() and future.done()
    # Cancelled while it runs: the next one starts at its deadline.
    assert outcome(lib.call("echo", 1)) == ("ok", [1])
with part("a waiting call past its deadline is TIMEOUT; the queue moves on; its late value is dropped"):
    slow = lib.call("later", "too late", 500, timeout_ms=150, function=counted("slow"))
    nxt = lib.call("echo", "next")
    assert outcome(nxt) == ("ok", ["next"])
    assert told["slow"] and told["slow"][0][1].code == L.TIMEOUT, told["slow"]
    wait(lambda: False, 0.6)
    assert len(told["slow"]) == 1
with part("a full queue: QUEUE_FULL"):
    small = L.load(MAIN, {"queue": 2})
    assert settle(small.ready)
    # One runs, two wait, the rest are refused.
    futures = [small.call("later", 0, 300)]
    wait(lambda: False, 0.1)  # taken from the queue, running
    futures += [small.call("later", i, 10) for i in range(1, 5)]
    codes = [outcome(f)[0] for f in futures]
    assert codes == ["ok", "ok", "ok", "QUEUE_FULL", "QUEUE_FULL"], codes
    small.close()
with part("maxBytes: arguments and results past it"):
    small = L.load(MAIN, {"maxBytes": 4096})
    assert outcome(small.call("echo", "x" * 5000))[0] == "BAD_ARGUMENTS"
    assert outcome(small.call("bytesOut", 5000))[0] == "BAD_RESULT"
    assert outcome(small.call("echo", "fine")) == ("ok", ["fine"])
    small.close()
with part("a running script past its deadline: TIMEOUT, the library ends, the rest GONE"):
    doomed = L.load(MAIN)
    started = time.monotonic()
    spinning = doomed.call("spin", timeout_ms=300)
    queued = doomed.call("echo", "queued")
    assert outcome(spinning)[0] == "TIMEOUT"
    assert 0.25 < time.monotonic() - started < 5, time.monotonic() - started
    assert outcome(queued)[0] == "GONE"
    assert outcome(doomed.call("echo", "later"))[0] == "GONE"
    doomed.close()
with part("the library's default deadline (timeoutMs) applies"):
    doomed = L.load(MAIN, {"timeoutMs": 200})
    assert outcome(doomed.call("spin"))[0] == "TIMEOUT"
    doomed.close()
with part("heap exhaustion: OUT_OF_MEMORY, the library ends, the process lives"):
    hungry = L.load(MAIN, {"heapLimitMB": 32})
    assert outcome(hungry.call("grow", timeout_ms=-1), seconds=60)[0] == "OUT_OF_MEMORY"
    assert outcome(hungry.call("echo", 1))[0] == "GONE"
    hungry.close()
    assert outcome(lib.call("echo", "still here")) == ("ok", ["still here"])

# ---- fetch ---------------------------------------------------------------------


class Pages(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path == "/redirect-out":
            self.send_response(302)
            self.send_header("Location", "http://localhost:%d/hello" % self.server.server_address[1])
            self.end_headers()
            return
        if self.path == "/redirect-in":
            self.send_response(302)
            self.send_header("Location", "/hello")
            self.end_headers()
            return
        body = {"/hello": "hello from the server",
                "/set-cookie": "set",
                "/cookie": self.headers.get("Cookie") or "no cookie"}.get(self.path)
        if body is None:
            self.send_error(404)
            return
        data = body.encode()
        self.send_response(200)
        self.send_header("Content-Type", "text/plain")
        if self.path == "/set-cookie":
            self.send_header("Set-Cookie", "kept=1; Path=/")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_POST(self):
        data = self.rfile.read(int(self.headers.get("Content-Length", 0)))
        reply = json.dumps({"got": data.decode(), "type": self.headers.get("Content-Type")}).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(reply)))
        self.end_headers()
        self.wfile.write(reply)

    def log_message(self, *args):
        pass


server = Server(Pages)
with part("fetch() refused without a grant"):
    kind, error = outcome(lib.call("get", server.base + "/hello"))
    assert kind == "THROWN" and error.name == "TypeError" and "not an origin the library was given" in error.message, \
        (kind, error and vars(error))
with part("fetch() allowed to a granted origin: GET, POST, a redirect inside it"):
    net = L.load(MAIN, {"network": [server.base]})
    kind, value = outcome(net.call("get", server.base + "/hello"))
    assert kind == "ok" and value["status"] == 200 and value["body"] == "hello from the server" and \
        value["type"] == "text/plain", (kind, value)
    kind, value = outcome(net.call("get", server.base + "/post", {"method": "POST", "body": "ü=1",
                                                                  "headers": {"Content-Type": "text/x-test"}}))
    assert kind == "ok" and json.loads(value["body"]) == {"got": "ü=1", "type": "text/x-test"}, (kind, value)
    kind, value = outcome(net.call("get", server.base + "/redirect-in"))
    assert kind == "ok" and value["redirected"] and value["body"] == "hello from the server", (kind, value)
    kind, value = outcome(net.call("get", server.base + "/nothing"))
    assert kind == "ok" and value["status"] == 404, (kind, value)
with part("fetch(): a redirect to an origin not granted fails"):
    kind, error = outcome(net.call("get", server.base + "/redirect-out"))
    assert kind == "THROWN" and "not granted" in error.message, (kind, error and vars(error))
with part("fetch(): no cookies kept or sent"):
    assert outcome(net.call("get", server.base + "/set-cookie"))[0] == "ok"
    kind, value = outcome(net.call("get", server.base + "/cookie"))
    assert kind == "ok" and value["body"] == "no cookie", (kind, value)
    net.close()

# ---- close, many ----------------------------------------------------------------

with part("close with calls pending: all GONE, exactly once; then calls raise"):
    closing = L.load(MAIN)
    assert settle(closing.ready)
    running = closing.call("later", 1, 2000, function=counted("close-running"))
    queued = [closing.call("echo", i, function=counted("close-%d" % i)) for i in range(3)]
    wait(lambda: False, 0.1)
    closing.close()
    keys = ["close-running"] + ["close-%d" % i for i in range(3)]
    assert wait(lambda: all(told[k] for k in keys))
    wait(lambda: False, 0.3)
    assert all(len(told[k]) == 1 and told[k][0][1].code == L.GONE for k in keys), {k: told[k] for k in keys}
    try:
        closing.call("echo", 1)
        raise AssertionError("a call after close was taken")
    except zebkit.Error:
        pass
    closing.cancel(running)  # nothing, once closed
with part("closed while loading: ready is GONE"):
    gone = L.load(MAIN)
    gone.close()
    assert outcome(gone.ready)[0] == "GONE"
with part("closed while a script spins: it is stopped, the calls GONE"):
    spinner = L.load(MAIN)
    spinning = spinner.call("spin", timeout_ms=-1)
    wait(lambda: False, 0.2)
    spinner.close()
    assert outcome(spinning)[0] == "GONE"
    assert outcome(lib.call("echo", "alive")) == ("ok", ["alive"])
with part("32 libraries at once, each with its own state"):
    many = [L.load(MAIN) for _ in range(32)]
    futures = [m.call("count") for m in many for _ in range(3)]
    assert settle(*futures, seconds=60)
    assert [f.result() for f in futures] == [1, 2, 3] * 32
    for m in many:
        m.close()
lib.close()

# ---- the examples' bundles -------------------------------------------------------

markdown, charts = os.path.join(bundles, "markdown"), os.path.join(bundles, "charts")
if os.path.isfile(os.path.join(markdown, "index.mjs")):
    with part("examples/js-libraries: markdown-it renders Markdown to HTML"):
        md = L.load(markdown)
        kind, html = outcome(md.call("render", "# Title\n\nSome *text* 日本語 and `code`."))
        assert kind == "ok" and html == "<h1>Title</h1>\n<p>Some <em>text</em> 日本語 and <code>code</code>.</p>\n", html
        kind, html = outcome(md.call("render", "a -- b https://example.com", {"rich": True}))
        assert kind == "ok" and "–" in html and 'href="https://example.com"' in html, html
        md.close()
else:
    skip("markdown-it", "no bundle in %s: examples/js-libraries/build.sh (engine-test.sh --fetch)" % bundles)
if os.path.isfile(os.path.join(charts, "index.mjs")):
    with part("examples/js-libraries: ECharts renders a chart to SVG, as text and as a part"):
        ec = L.load(charts)
        spec = {"width": 400, "height": 300, "option": {
            "title": {"text": "Sales 销量"}, "xAxis": {"type": "category", "data": ["Mon", "Tue", "Wed"]},
            "yAxis": {"type": "value"}, "series": [{"type": "bar", "data": [120, 200, 150]}]}}
        kind, svg = outcome(ec.call("renderSvg", spec))
        assert kind == "ok" and svg.startswith("<svg") and 'width="400"' in svg and "销量" in svg and \
            svg.count("<path") >= 3, (kind, svg[:200])
        kind, data = outcome(ec.call("renderSvgBytes", spec))
        # The same chart; ECharts numbers its classes per instance (zr0-cls-0, zr1-cls-2).
        assert kind == "ok" and isinstance(data, bytes) and \
            re.sub(r"zr\d+-cls-\d+", "", data.decode()) == re.sub(r"zr\d+-cls-\d+", "", svg), (kind, data[:100])
        ec.close()
else:
    skip("ECharts", "no bundle in %s: examples/js-libraries/build.sh (engine-test.sh --fetch)" % bundles)

# js-bundle.mjs refuses what a library cannot have, with the esbuild the
# bundles were made with (offline).
esbuild_dir = os.path.join(bundles, "work", "node_modules")
node = shutil.which("node")
if node and os.path.isdir(os.path.join(esbuild_dir, "esbuild")):
    with part("js-bundle.mjs names a Node.js built-in it cannot bundle, and bundles with --empty"):
        entry = os.path.join(library("bundle-fs", {"entry.mjs": "import fs from 'node:fs';\nexport const f = () => typeof fs;\n"}),
                             "entry.mjs")
        out = os.path.join(work, "bundle-fs-out")
        run = subprocess.run([node, os.path.join(ROOT, "tools", "js-bundle.mjs"), entry, out, "--esbuild", esbuild_dir,
                              "--work", os.path.join(work, "bundle-work")], capture_output=True, text=True, timeout=120)
        assert run.returncode == 1 and "node:fs (a Node.js built-in)" in run.stderr, (run.returncode, run.stderr)
        run = subprocess.run([node, os.path.join(ROOT, "tools", "js-bundle.mjs"), entry, out, "--esbuild", esbuild_dir,
                              "--work", os.path.join(work, "bundle-work"), "--empty", "node:fs"],
                             capture_output=True, text=True, timeout=120)
        assert run.returncode == 0, run.stderr
        bundled = L.load(out)
        assert outcome(bundled.call("f")) == ("ok", "object")
        bundled.close()
else:
    skip("js-bundle.mjs", "no node or no esbuild in %s (engine-test.sh --fetch)" % esbuild_dir)

server.close()
shutil.rmtree(work, ignore_errors=True)
zebkit.shutdown()
sys.exit(finish())
