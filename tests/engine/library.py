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
at once; calls from another thread; exactly one outcome each. The code cache
(its file in the profile, a changed module never served a stale one, a
damaged file refused, codeCache false); state kept across an idle shrink;
instances (calls in parallel, state per instance, deadlines, cancel, close,
an instance out of memory while the others go on); host functions (progress
during a call, data and bytes asked of the application, its error, an answer
from another thread, one never given, exactly one answer, a call at load).
With the bundles of examples/js-libraries (build.sh, engine-test.sh --fetch):
markdown-it and ECharts, the facades js-bundle.mjs --wrapper generates for
markdown-it (the generated Python one used), --list, and a CommonJS package
bundled without an entry. Exit 0 when every check passes.
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
work = tempfile.mkdtemp(prefix="zebkit-library-")
args = args or ["--headless", "--no-sandbox"]
# The profile, which the code cache is kept in.
profile = next((a.split("=", 1)[1] for a in args if a.startswith("--user-data-dir=")), None)
if profile is None:
    profile = os.path.join(work, "profile")
    args.append("--user-data-dir=" + profile)
assert zebkit.init([sys.argv[0]] + args, settings={"logging": {"destination": "none"}}) == 0


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
            (MAIN, {"instances": 0}, "instances"),
            (MAIN, {"instances": 1.5}, "instances"),
            (MAIN, {"codeCache": "yes"}, "codeCache"),
            (MAIN, {"host": "progress"}, "host"),
            (MAIN, {"host": [""]}, "host"),
            (MAIN, {"width": 300}, "page")]:
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

# ---- the code cache ---------------------------------------------------------------

import hashlib  # noqa: E402


def cache_file(module):
    """The profile's code cache file of the module at @module."""
    name = hashlib.sha256(os.path.realpath(module).encode()).hexdigest()[:32].upper()
    return os.path.join(profile, "Library Code Cache", name + ".v8")


# Enough code that V8 caches something, and a value that tells versions apart.
BODY = "".join("export function f%d(x) { return x * %d + %d; }\n" % (i, i, i) for i in range(200))
with part("the code cache: kept in the profile after close, used by the next load, keyed by content"):
    folder = library("cached", {"index.mjs": BODY + "export const version = () => 'one';\n"})
    module = os.path.join(folder, "index.mjs")
    first = L.load(folder)
    assert outcome(first.call("version")) == ("ok", "one")
    assert outcome(first.call("f7", 3)) == ("ok", 28)
    first.close()
    assert wait(lambda: os.path.isfile(cache_file(module)), 10), cache_file(module)
    with open(cache_file(module), "rb") as f:
        head = f.read(64)
    assert head.startswith(b"zebkit library code cache 1 "), head
    again = L.load(folder)
    assert outcome(again.call("version")) == ("ok", "one") and outcome(again.call("f9", 1)) == ("ok", 18)
    again.close()
    # The same length, other content: never the old code.
    with open(module, "w", encoding="utf-8") as f:
        f.write(BODY + "export const version = () => 'two';\n")
    changed = L.load(folder)
    assert outcome(changed.call("version")) == ("ok", "two")
    changed.close()
with part("the code cache: a damaged file is refused and the module compiles"):
    assert wait(lambda: os.path.isfile(cache_file(module)), 10)
    with open(cache_file(module), "rb") as f:
        text = f.read()
    at = text.index(b"\n") + 1
    with open(cache_file(module), "wb") as f:
        f.write(text[:at] + bytes(len(text) - at))
    # Not in this process's memory: a module of its own text.
    with open(module, "w", encoding="utf-8") as f:
        f.write(BODY + "export const version = () => 'six';\n")
    damaged = L.load(folder)
    assert outcome(damaged.call("version")) == ("ok", "six")
    damaged.close()
with part("codeCache false: nothing kept in the profile"):
    plain = library("uncached", {"index.mjs": BODY + "export const v = () => 3;\n"})
    off = L.load(plain, {"codeCache": False})
    assert outcome(off.call("v")) == ("ok", 3)
    off.close()
    wait(lambda: False, 0.5)
    assert not os.path.exists(cache_file(os.path.join(plain, "index.mjs")))
with part("an idle library gives memory back and keeps its state"):
    idle = L.load(MAIN)
    assert outcome(idle.call("count")) == ("ok", 1)
    wait(lambda: False, 2.6)  # past the idle shrink
    assert outcome(idle.call("count")) == ("ok", 2)
    idle.close()

# ---- instances ----------------------------------------------------------------------

POOL = library("pool", {"index.mjs": r"""
let counter = 0;
export const count = () => ++counter;
// Busy for ms milliseconds: the thread is taken, as a long computation's is.
export function busy(ms) { const end = Date.now() + ms; while (Date.now() < end) {} return ms; }
export const countAfter = (ms) => { busy(ms); return ++counter; };
export const spin = () => { for (;;) {} };
export const later = (value, ms) => new Promise((resolve) => setTimeout(() => resolve(value), ms));
export function grow() { const keep = []; for (;;) keep.push(new Array(100000).fill(Math.random())); }
"""})


def wall(lib_, calls, seconds=30):
    started = time.monotonic()
    futures = [lib_.call(*c) for c in calls]
    assert settle(*futures, seconds=seconds)
    return time.monotonic() - started, futures


with part("instances: 4 calls of 400 ms of work run in parallel on 4 instances, one after another on 1"):
    one, four = L.load(POOL), L.load(POOL, {"instances": 4})
    assert settle(one.ready, four.ready)
    assert four.ready.result()["functions"] == one.ready.result()["functions"]
    took_one, _ = wall(one, [("busy", 400)] * 4)
    took_four, futures = wall(four, [("busy", 400)] * 4)
    assert [f.result() for f in futures] == [400] * 4
    assert took_one >= 1.55 and took_four < 1.2, (took_one, took_four)
with part("instances: state is each instance's own"):
    # Each instance counts its own calls: 8 of 100 ms on 4 instances count to 2 or so each.
    _, futures = wall(four, [("countAfter", 100)] * 8)
    counts = sorted(f.result() for f in futures)
    assert counts[-1] < 8 and counts.count(1) >= 2, counts
with part("instances: cancel of a waiting call; the others run"):
    running = [four.call("busy", 200) for _ in range(4)]
    queued = four.call("count", function=counted("pool-queued"))
    four.cancel(queued)
    assert settle(*running) and wait(lambda: told["pool-queued"])
    assert told["pool-queued"][0][1].code == L.CANCELLED
with part("instances: a script past its deadline ends its instance; the others take the calls"):
    doomed = L.load(POOL, {"instances": 2})
    spinning = doomed.call("spin", timeout_ms=300)
    after = [doomed.call("later", i, 20) for i in range(6)]
    assert outcome(spinning)[0] == "TIMEOUT"
    assert [outcome(f) for f in after] == [("ok", i) for i in range(6)]
    # The last instance ended: the library has ended.
    assert outcome(doomed.call("spin", timeout_ms=300))[0] == "TIMEOUT"
    assert outcome(doomed.call("count"))[0] == "GONE"
    doomed.close()
with part("instances: one out of memory ends alone; the other goes on"):
    hungry = L.load(POOL, {"instances": 2, "heapLimitMB": 32})
    assert settle(hungry.ready)
    grown = hungry.call("grow", timeout_ms=-1)
    waiting = hungry.call("later", "still", 300)
    assert outcome(grown, seconds=60)[0] == "OUT_OF_MEMORY"
    assert outcome(waiting) == ("ok", "still")
    assert outcome(hungry.call("count")) == ("ok", 1)
    hungry.close()
with part("instances: close with calls running on each: all GONE, once"):
    closing = L.load(POOL, {"instances": 3})
    assert settle(closing.ready)
    keys = ["pool-close-%d" % i for i in range(5)]
    for i, key in enumerate(keys):
        closing.call("busy" if i % 2 else "later", 5000 if i % 2 else 1, *([] if i % 2 else [5000]),
                     function=counted(key))
    wait(lambda: False, 0.2)
    closing.close()
    assert wait(lambda: all(told[k] for k in keys))
    wait(lambda: False, 0.3)
    assert all(len(told[k]) == 1 and told[k][0][1].code == L.GONE for k in keys), {k: told[k] for k in keys}
one.close()
four.close()

# ---- host functions -------------------------------------------------------------------

HOST = library("host", {"index.mjs": r"""
import host from 'zebkit:host';
import {call, functions} from 'zebkit:host';
// At load: the application is asked already.
const config = await host.call('config');
export const loadedWith = () => config;
export const names = () => functions;
export async function work(steps) {
  for (let i = 1; i <= steps; ++i) await host.call('progress', i, steps);
  return 'done';
}
export async function ask(question, bytes) {
  const answer = await call('lookup', question, bytes);
  return {answer, isBytes: answer.data instanceof Uint8Array, length: answer.data.length};
}
export async function failing() {
  try { await host.call('fails', 'x'); return 'resolved'; }
  catch (error) { return {name: error.name, message: error.message}; }
}
export async function unanswered() {
  try { await host.call('never'); return 'resolved'; }
  catch (error) { return {name: error.name}; }
}
export async function unknown() {
  try { await host.call('nothing'); return 'resolved'; }
  catch (error) { return {name: error.name, message: error.message}; }
}
export const threaded = (n) => host.call('threaded', n);
export const raw = () => host.call('raw');
"""})

progress = []
pending_never = []
raw_requests = []


def lookup(question, data):
    return {"question": question, "data": bytes(reversed(data))}


def threaded(n):
    future = zebkit.automation.Future()
    threading.Thread(target=lambda: (time.sleep(0.05), future.set_result(n * 3))).start()
    return future


def never():
    future = zebkit.automation.Future()
    pending_never.append(future)
    return future


def fails(value):
    raise KeyError("no such key: " + value)


hosted = L.load(HOST, {"timeoutMs": 2000}, host={
    "config": lambda: {"theme": "dark"},
    "progress": lambda i, n: progress.append((i, n)),
    "lookup": lookup,
    "fails": fails,
    "never": never,
    "threaded": threaded,
})
with part("host: asked at load (a top-level await), and the names the library sees"):
    assert outcome(hosted.ready)[0] == "ok", outcome(hosted.ready)
    assert outcome(hosted.call("loadedWith")) == ("ok", {"theme": "dark"})
    kind, value = outcome(hosted.call("names"))
    assert kind == "ok" and sorted(value) == ["config", "fails", "lookup", "never", "progress", "threaded"], value
with part("host: progress told during a long call, in order"):
    assert outcome(hosted.call("work", 5)) == ("ok", "done")
    assert progress == [(i, 5) for i in range(1, 6)], progress
with part("host: data and bytes asked of the application"):
    kind, value = outcome(hosted.call("ask", "why", b"\x01\x02\x03"))
    assert kind == "ok" and value["answer"]["question"] == "why" and value["isBytes"] and value["length"] == 3, value
with part("host: the application's error is a rejected promise in the library"):
    kind, value = outcome(hosted.call("failing"))
    assert kind == "ok" and value == {"name": "KeyError", "message": "no such key: x"}, value
with part("host: an answer from another thread"):
    assert outcome(hosted.call("threaded", 7)) == ("ok", 21)
with part("host: a name the load did not give is a TypeError at once"):
    kind, value = outcome(hosted.call("unknown"))
    assert kind == "ok" and value["name"] == "TypeError" and "nothing" in value["message"], value
with part("host: never answered: a TimeoutError at the library's timeoutMs; a late answer is refused"):
    started = time.monotonic()
    kind, value = outcome(hosted.call("unanswered", timeout_ms=-1))
    assert kind == "ok" and value == {"name": "TimeoutError"} and 1.8 < time.monotonic() - started < 6, \
        (kind, value, time.monotonic() - started)
    pending_never[0].set_result("too late")  # answered after its deadline: nothing happens
with part("host: C: an answer is taken once (1), then refused (0)"):
    def raw_host(library_, request, name, text, parts_, count, data):
        raw_requests.append(request)
        first_ = L._library_answer(library_, request, L.OK, b'"first"', None, 0)
        second = L._library_answer(library_, request, L.OK, b'"second"', None, 0)
        raw_requests.append((first_, second))
    raw_callback = L._HOST(raw_host)
    direct = L.load(HOST, {"host": ["config", "raw"]})
    L._library_set_host(direct._raw, raw_callback, None)
    assert outcome(direct.call("raw")) == ("ok", "first")
    assert raw_requests[1] == (1, 0), raw_requests
    direct.close()
with part("host: closed with a request unanswered: the call is GONE, a late answer does nothing"):
    closing = L.load(HOST, host={"config": lambda: 1, "never": never})
    waiting = closing.call("unanswered")
    wait(lambda: len(pending_never) > 1, 5)
    closing.close()
    assert outcome(waiting)[0] == "GONE"
    pending_never[-1].set_result(1)
hosted.close()

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
    with part("js-bundle.mjs: a CommonJS package bundled without an entry, its names read without running it"):
        package = library("cjs-package", {
            "package.json": json.dumps({"name": "cjs-sample", "version": "1.0.0", "main": "index.js",
                                        "types": "index.d.ts"}),
            "index.js": "'use strict';\nmodule.exports = require('./lib.js');\n",
            "lib.js": "exports.add = (a, b) => a + b;\nmodule.exports.greet = function (name) { return 'hi ' + name; };\n"
                      "exports.version = '1.0';\n",
            "index.d.ts": "/** Their sum. */\nexport declare function add(a: number, b: number): number;\n"})
        out = os.path.join(work, "cjs-out")
        run = subprocess.run([node, os.path.join(ROOT, "tools", "js-bundle.mjs"), package, out, "--esbuild", esbuild_dir,
                              "--work", os.path.join(work, "cjs-work"), "--offline"],
                             capture_output=True, text=True, timeout=120)
        assert run.returncode == 0 and "cjs-sample is CommonJS: add, greet, version" in run.stderr, run.stderr
        cjs = L.load(out)
        kind, value = outcome(cjs.ready)
        assert kind == "ok" and sorted(value["functions"]) == ["add", "greet"], value
        assert outcome(cjs.call("add", 2, 3)) == ("ok", 5) and outcome(cjs.call("greet", "zebkit")) == ("ok", "hi zebkit")
        cjs.close()
        listed = subprocess.run([node, os.path.join(ROOT, "tools", "js-bundle.mjs"), "--list", out],
                                capture_output=True, text=True, timeout=60)
        assert listed.returncode == 0 and listed.stdout.splitlines() == [
            "add(a: number, b: number): number  // Their sum.", "greet(...args)"], listed.stdout
else:
    skip("js-bundle.mjs", "no node or no esbuild in %s (engine-test.sh --fetch)" % esbuild_dir)
if node and os.path.isfile(os.path.join(markdown, "exports.json")):
    with part("js-bundle.mjs --list and --wrapper: markdown-it's functions, the facades in examples/js-libraries"):
        def bundler(*arguments):
            run = subprocess.run([node, os.path.join(ROOT, "tools", "js-bundle.mjs"), *arguments],
                                 capture_output=True, text=True, timeout=60)
            assert run.returncode == 0, run.stderr
            return run.stdout
        assert bundler("--list", markdown).splitlines()[0] == \
            "render(text: string, options?: {rich?: boolean}): string  // The HTML of a Markdown document.", \
            bundler("--list", markdown)
        here = os.path.join(ROOT, "examples", "js-libraries", "markdown")
        for language, name in (("rust", "markdown.rs"), ("python", "markdown.py")):
            with open(os.path.join(here, name), encoding="utf-8") as f:
                assert bundler("--wrapper", language, markdown, "--name", "Markdown") == f.read(), \
                    "%s is not what --wrapper %s makes: build.sh says how to make it again" % (name, language)
        go_facade = bundler("--wrapper", "go", markdown, "--name", "Markdown", "--package", "markdown")
        assert "func (m *Markdown) RenderMany(texts []string, done func([]string, error)) error {" in go_facade
        gofmt = shutil.which("gofmt")
        if gofmt:
            checked = subprocess.run([gofmt, "-l"], input=go_facade, capture_output=True, text=True, timeout=60)
            assert checked.returncode == 0 and not checked.stdout.strip(), checked.stdout + checked.stderr
        assert "call(name: 'render', text: string, options?: {rich?: boolean}): Promise<string>;" in \
            bundler("--wrapper", "ts", markdown, "--name", "Markdown")
    with part("the generated Python facade of markdown-it (examples/js-libraries/markdown/markdown.py)"):
        sys.path.insert(0, os.path.join(ROOT, "examples", "js-libraries", "markdown"))
        from markdown import Markdown  # noqa: E402
        md = Markdown(markdown)
        assert outcome(md.render("# Title")) == ("ok", "<h1>Title</h1>\n")
        kind, html = outcome(md.render("a -- b https://example.com", {"rich": True}))
        assert kind == "ok" and "\u2013" in html and 'href="https://example.com"' in html, html
        assert outcome(md.render_inline("*x*")) == ("ok", "<em>x</em>")
        assert outcome(md.render_many(["a", "b"])) == ("ok", ["<p>a</p>\n", "<p>b</p>\n"])
        md.close()
else:
    skip("js-bundle.mjs --wrapper", "no node, or no markdown bundle with exports.json (engine-test.sh --fetch)")

server.close()
shutil.rmtree(work, ignore_errors=True)
zebkit.shutdown()
sys.exit(finish())
