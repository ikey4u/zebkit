"""What the Python engine tests share: their checks, waiting with the engine
pumped, evaluating in a view, the activation a page's gated call needs, a local
page server on a free port, the engine started as a program of its own with a
CDP client of it, programs run to their end, and the summary line with the
exit code.

    HERE = os.path.dirname(os.path.abspath(__file__))
    sys.path.insert(0, HERE)
    from harness import zebkit, check, wait, ...  # noqa: E402

Importing it puts the Python binding on sys.path (tests run as `python3 -I`);
the binding, and with it the engine's library, is loaded on the first use of
harness.zebkit: a test that only starts programs never loads it.

The lines a test prints:
    ok   <what>             a check passed
    FAIL <what>: <detail>   a check failed
    skip <what>: <why>      a part not run here (counted, never a pass)
    note: <what>            said only: a part another run of the same test covers
    narrowed: <what>        a ZEBKIT_TEST_* knob made the test do less
    N skipped: ...          before the summary, when something was skipped
    N of M checks passed    the summary, last

finish() is the exit code: 0 only when there were checks and none failed.
Under ENGINE_TEST_STRICT=1 a skip or a narrowed run fails too, unless one of
ENGINE_TEST_EXPECTED_SKIPS's lines (extended regular expressions, set by
scripts/check.sh) matches "<test>: <what>: <why>" (or "<test>: narrowed: ...");
<test> is ENGINE_TEST_NAME, which scripts/engine-test.sh sets, or the script's
name. An exception in a callback of the binding's, which the binding reports
through sys.excepthook and carries on, is a failure too.
"""

import base64
import http.server
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

HERE = os.path.dirname(os.path.abspath(__file__))
_binding = os.path.join(HERE, "..", "..", "bindings", "python")
if _binding not in sys.path:
    sys.path.insert(0, _binding)


def _zebkit():
    import zebkit
    return zebkit


def __getattr__(name):
    # `from harness import zebkit`: the binding, loaded when first asked for.
    if name == "zebkit":
        return _zebkit()
    raise AttributeError(name)


# A page's user activation reaches the browser on a pipe of its own, after the
# evaluation that gave it; a gated request that comes first is refused there.
ACTIVATION_DELAY_MS = 200
# How long a negative check waits for something that must not happen.
SETTLE = 0.5
# How often wait() pumps and looks.
POLL = 0.005

STRICT = os.environ.get("ENGINE_TEST_STRICT") == "1"
TEST_NAME = os.environ.get("ENGINE_TEST_NAME") or os.path.splitext(os.path.basename(sys.argv[0]))[0]
EXPECTED_SKIPS = [re.compile(line) for line in os.environ.get("ENGINE_TEST_EXPECTED_SKIPS", "").splitlines()
                  if line.strip()]

failures = []
skipped = []
checks = 0


def expected_skip(entry):
    """Whether ENGINE_TEST_EXPECTED_SKIPS accepts a skip, "<test>: <what>: <why>"."""
    return any(pattern.search(entry) for pattern in EXPECTED_SKIPS)


# The binding reports an exception in a callback through sys.excepthook and goes
# on (zebkit/__init__.py, _guard); ctypes, one it raises unguarded, through
# sys.unraisablehook. Either is a failed check here.
_excepthook = sys.excepthook
_unraisablehook = sys.unraisablehook


def _callback_failed(what):
    global checks
    checks += 1
    print("FAIL " + what, flush=True)
    failures.append(what)


def _on_exception(kind, value, traceback):
    _excepthook(kind, value, traceback)
    _callback_failed("an exception in a callback: %r" % (value,))


def _on_unraisable(unraisable):
    _unraisablehook(unraisable)
    _callback_failed("an exception that could not be raised: %r" % (unraisable.exc_value,))


sys.excepthook = _on_exception
sys.unraisablehook = _on_unraisable


def check(ok, what, detail=None):
    """One check: an "ok   " or "FAIL " line; returns ok."""
    global checks
    checks += 1
    print(("ok   " if ok else "FAIL ") + what + (": %r" % (detail,) if detail is not None else ""), flush=True)
    if not ok:
        failures.append(what)
    return ok


class part:
    """A check made of a block of asserts and must_wait()s, for a part of a test
    that cannot go on once something in it is wrong:

        with part("the proxy's challenge"):
            ...

    one "ok   " line when the block ends, a "FAIL " line with the exception when
    it raises. The exception goes no further (the test goes on with its next
    part) unless fatal=True, for a part the rest depends on."""

    def __init__(self, what, fatal=False):
        self.what, self.fatal = what, fatal

    def __enter__(self):
        return self

    def __exit__(self, kind, value, traceback):
        if kind is not None and not issubclass(kind, Exception):
            return False  # KeyboardInterrupt, SystemExit
        check(kind is None, self.what, None if kind is None else _described(value))
        return not self.fatal


def _described(error):
    text = "%s: %s" % (type(error).__name__, error) if str(error) else type(error).__name__
    return text if len(text) < 2000 else text[:2000] + "..."


def skip(what, why):
    """A part not run on this machine: a "skip " line, never a pass."""
    print("skip %s: %s" % (what, why), flush=True)
    skipped.append(what)
    if STRICT and not expected_skip("%s: %s: %s" % (TEST_NAME, what, why)):
        failures.append("skipped: " + what)


def note(what):
    """Said, neither a check nor a skip: a part another run of the same test
    covers (an X11 run, of what this display cannot do)."""
    print("note: " + what, flush=True)


def knob(name):
    """ZEBKIT_TEST_<name>, a debugging option that narrows the test; a
    "narrowed:" line when it is set."""
    value = os.environ.get("ZEBKIT_TEST_" + name)
    if value:
        print("narrowed: ZEBKIT_TEST_%s=%s" % (name, value), flush=True)
        if STRICT and not expected_skip("%s: narrowed: ZEBKIT_TEST_%s=%s" % (TEST_NAME, name, value)):
            failures.append("narrowed by ZEBKIT_TEST_" + name)
    return value


def wait(condition, seconds=10):
    """Pump the engine until condition() is true: whether it became true."""
    end = time.monotonic() + seconds
    pump_once = _zebkit().pump
    while True:
        pump_once()
        if condition():
            return True
        if time.monotonic() >= end:
            return False
        time.sleep(POLL)


def must_wait(condition, what, seconds=15):
    """wait(), raising AssertionError naming what when it times out."""
    if not wait(condition, seconds):
        raise AssertionError("timed out waiting for " + what)


def pump(seconds=SETTLE):
    """Run the engine for a while: for negative checks only."""
    wait(lambda: False, seconds)


def ev(view, script, seconds=15):
    """The script's value in view; ("error", message) when it threw or was
    rejected, ("timeout", script) when no answer came."""
    box = []
    view.evaluate(script, lambda value, error=None: box.append((value, error)))
    if not wait(lambda: box, seconds):
        return ("timeout", script)
    return box[0][0] if box[0][1] is None else ("error", box[0][1])


def settled(value):
    """Whether an ev() result is an answer, not a timeout."""
    return not (isinstance(value, tuple) and value[:1] == ("timeout",))


def activated(script):
    """@script run once the browser has the evaluation's user activation: for
    every activation-gated call (fullscreen, pickers, device and display
    requests, locks)."""
    return "new Promise(r => setTimeout(r, %d)).then(() => %s)" % (ACTIVATION_DELAY_MS, script)


def refused(function):
    """The message of the zebkit.Error function() raises, or None."""
    try:
        function()
    except _zebkit().Error as error:
        return str(error)
    return None


class Server:
    """A local page server on a free port of 127.0.0.1, in a thread of its own:
    pages is a handler class, bytes served for every GET as text/html, or a dict
    of path to bytes (or to (bytes, content type))."""

    def __init__(self, pages):
        if isinstance(pages, type):
            handler = pages
        else:
            table = pages

            class handler(http.server.BaseHTTPRequestHandler):
                def do_GET(self):
                    page = table if isinstance(table, bytes) else table.get(self.path.split("?")[0])
                    if page is None:
                        self.send_error(404)
                        return
                    body, kind = page if isinstance(page, tuple) else (page, "text/html")
                    self.send_response(200)
                    self.send_header("Content-Type", kind)
                    self.send_header("Content-Length", str(len(body)))
                    self.end_headers()
                    self.wfile.write(body)

                def log_message(self, *args):
                    pass
        self.server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), handler)
        self.server.daemon_threads = True
        self.port = self.server.server_address[1]
        self.base = "http://127.0.0.1:%d" % self.port
        threading.Thread(target=self.server.serve_forever, kwargs={"poll_interval": .02}, daemon=True).start()

    def close(self):
        self.server.shutdown()
        self.server.server_close()


def finish():
    """The summary line, last; the exit code."""
    if skipped:
        print("%d skipped: %s" % (len(skipped), "; ".join(skipped)), flush=True)
    failed = [f for f in failures if not f.startswith(("skipped: ", "narrowed by "))]
    print("%d of %d checks passed" % (checks - len(failed), checks), flush=True)
    return 1 if failures or not checks else 0


# X11 (xdotool): windows found and focused before anything is typed into them.

def xdotool(*args, seconds=5):
    """xdotool's output, or None when it failed or took longer than seconds."""
    try:
        done = subprocess.run(["xdotool", *args], capture_output=True, text=True, timeout=seconds)
    except subprocess.TimeoutExpired:
        return None
    return done.stdout if done.returncode == 0 else None


def x11_window(name, seconds=5):
    """The visible X11 window named name exactly, waited for with the engine
    pumped (it maps and names the window); None when none came."""
    found = []
    wait(lambda: found.extend((xdotool("search", "--onlyvisible", "--name", "^%s$" % name) or "").split()) or found,
         seconds)
    return found[-1] if found else None


def x11_windows():
    """The visible top-level X11 windows, named or not (menus are not)."""
    return set((xdotool("search", "--onlyvisible", "--maxdepth", "1", "--name", "^") or "").split())


def x11_new_window(before, seconds=5):
    """A visible top-level window that was not in before, waited for (the
    engine pumped); None when none came."""
    found = []
    wait(lambda: found.extend(x11_windows() - set(before)) or found, seconds)
    return found[-1] if found else None


def x11_focused(window, seconds=5):
    """Whether window has the keyboard focus, waited for."""
    return wait(lambda: (xdotool("getwindowfocus") or "").strip() == window, seconds)


def held(view, seconds=5):
    """Whether the page is held by a dialog of its own: drawn in the view's
    window, not one of X11's, it blocks the page, so an evaluation does not
    answer. Two probes in a row, a second each, unanswered: a page that is only
    slow (a loaded machine) answers one of them."""
    blocked = lambda: ev(view, "1", 1) == ("timeout", "1")  # noqa: E731
    return wait(lambda: blocked() and blocked(), seconds)


# The engine as a program of its own (tools/zebkit-run, a test program), and a
# CDP client of it.

class Engine:
    """command, started with --remote-debugging-port=0 and page appended (cdp=False:
    nothing appended, and url stays None). Its standard error is drained by a
    thread of its own into a file, so that the engine never blocks on a full
    pipe and what it said is still there when a check fails; url is its
    DevTools WebSocket, waited for with a real timeout (None when none came, or
    when the program ended first).

        engine = Engine([run, *switches])
        ...
        engine.stop(failed)  # the log's end on standard error when failed

    Also a context manager: stopped on leaving, the log shown when an exception
    leaves it."""

    def __init__(self, command, seconds=30, page="about:blank", cdp=True, env=None, stdout=subprocess.DEVNULL):
        self.log = tempfile.NamedTemporaryFile(prefix="zebkit-engine-", suffix=".log", delete=False)
        self.lines = []
        self.process = subprocess.Popen([*command, *(["--remote-debugging-port=0"] + ([page] if page else [])
                                                     if cdp else [])],
                                        stdout=stdout, stderr=subprocess.PIPE, env=env)
        self.url = None
        found = threading.Event()

        def drain():
            for line in self.process.stderr:
                self.log.write(line)
                self.log.flush()
                self.lines.append(line.decode(errors="replace"))
                if not found.is_set():
                    match = re.search(rb"ws://\S+", line)
                    if match:
                        self.url = match.group(0).decode()
                        found.set()
            found.set()
        self.thread = threading.Thread(target=drain, daemon=True)
        self.thread.start()
        if cdp:
            found.wait(seconds)

    @property
    def port(self):
        """The CDP port, from url."""
        return int(re.match(r"ws://[^:/]+:(\d+)", self.url).group(1)) if self.url else None

    def poll(self):
        return self.process.poll()

    def said(self, pattern, seconds=10):
        """The first line of its standard error that pattern (a regular
        expression) is in, waited for; None when none came."""
        end = time.monotonic() + seconds
        while True:
            for line in list(self.lines):
                if re.search(pattern, line):
                    return line
            if time.monotonic() >= end or (self.process.poll() is not None and not self.thread.is_alive()):
                return None
            time.sleep(POLL * 4)

    def tail(self, lines=40):
        with open(self.log.name, "rb") as log:
            return b"".join(log.readlines()[-lines:]).decode(errors="replace")

    def stop(self, failed=False, seconds=10):
        """Ends it (terminate, then kill); the exit status."""
        if self.process.poll() is None:
            self.process.terminate()
        try:
            self.process.wait(seconds)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait()
        self.thread.join(5)
        if failed:
            sys.stderr.write("the engine's standard error, its end:\n" + self.tail())
        if not self.log.closed:
            self.log.close()
            os.unlink(self.log.name)
        return self.process.returncode

    def __enter__(self):
        return self

    def __exit__(self, kind, value, traceback):
        self.stop(kind is not None or bool(failures))
        return False


def launch_engine(command, seconds=30, **options):
    """Engine(command, seconds, ...): see there."""
    return Engine(command, seconds, **options)


class Cdp:
    """One WebSocket to a CDP target (the browser's, by default), with flat
    sessions; on the standard library alone."""

    def __init__(self, url, timeout=30):
        host, port, path = re.match(r"ws://([^:/]+):(\d+)(/.*)", url).groups()
        self.timeout = timeout
        self.sock = socket.create_connection((host, int(port)), timeout=timeout)
        key = base64.b64encode(os.urandom(16)).decode()
        self.sock.sendall((f"GET {path} HTTP/1.1\r\nHost: {host}:{port}\r\nUpgrade: websocket\r\n"
                           f"Connection: Upgrade\r\nSec-WebSocket-Key: {key}\r\n"
                           "Sec-WebSocket-Version: 13\r\n\r\n").encode())
        response = b""
        while b"\r\n\r\n" not in response:
            chunk = self.sock.recv(4096)
            if not chunk:
                raise EOFError("the WebSocket closed during its handshake")
            response += chunk
        if b" 101 " not in response.split(b"\r\n", 1)[0]:
            raise ConnectionError(response.split(b"\r\n", 1)[0].decode(errors="replace"))
        self.buffer = response.split(b"\r\n\r\n", 1)[1]
        self.next_id = 0
        self.events = []

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass

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
        """Sends a command; its id, for reply()."""
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
        """The command's result; RuntimeError with the message of its error,
        TimeoutError when no answer came."""
        wanted = self.send(method, params, session)
        end = time.time() + timeout
        while time.time() < end:
            for i, message in enumerate(self.events):
                if message.get("method") == "#reply" and message.get("id") == wanted:
                    del self.events[i]
                    return self._result(message)
            self.sock.settimeout(max(0.05, end - time.time()))
            try:
                message = self._read_message()
            except socket.timeout:
                continue
            finally:
                self.sock.settimeout(self.timeout)
            if message.get("id") == wanted and "method" not in message:
                return self._result(message)
            self.events.append(message if "method" in message else {"method": "#reply", **message})
        raise TimeoutError(method)

    @staticmethod
    def _result(message):
        if "error" in message:
            raise RuntimeError(message["error"].get("message"))
        return message.get("result", {})

    def event(self, method, timeout=10, session=None):
        """The next event of method (of session, when given), waited for; None
        when none came."""
        end = time.time() + timeout
        while time.time() < end:
            for i, message in enumerate(self.events):
                if message.get("method") == method and (session is None or message.get("sessionId") == session):
                    return self.events.pop(i)
            self.sock.settimeout(max(0.05, end - time.time()))
            try:
                message = self._read_message()
            except socket.timeout:
                continue
            finally:
                self.sock.settimeout(self.timeout)
            self.events.append(message if "method" in message else {"method": "#reply", **message})
        return None

    def reply(self, wanted, timeout=10):
        """The answer to the command send() numbered wanted, waited for; None when
        none came."""
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
                self.sock.settimeout(self.timeout)
            self.events.append(message if "method" in message else {"method": "#reply", **message})
        return None

    def page(self, url="about:blank"):
        """A new page target at url, attached (flat): (target id, session id),
        with Page and Runtime enabled."""
        target = self.call("Target.createTarget", {"url": url})["targetId"]
        session = self.call("Target.attachToTarget", {"targetId": target, "flatten": True})["sessionId"]
        self.call("Page.enable", session=session)
        self.call("Runtime.enable", session=session)
        return target, session

    def evaluate(self, expression, session, timeout=30):
        """The expression's value in session's page (promises awaited);
        RuntimeError with its exception's description when it threw."""
        result = self.call("Runtime.evaluate", {"expression": expression, "returnByValue": True,
                                                "awaitPromise": True}, session=session, timeout=timeout)
        if "exceptionDetails" in result:
            details = result["exceptionDetails"]
            raise RuntimeError(details.get("exception", {}).get("description") or details.get("text"))
        return result.get("result", {}).get("value")


# Programs run to their end: a test's half in another language, a helper.

def program(command, what, expect=None, seconds=60, env=None, quiet=False):
    """Runs command to its end, its output shown indented under a "  | " (so
    that its own ok, FAIL and summary lines are not this test's); its "skip "
    and "narrowed:" lines become this test's, of what. One check: it exited 0,
    printed no FAIL line, every check of its own "N of M checks passed" passed,
    and, when expect (a regular expression) is given, printed a line it is in.
    The completed process, or None when it did not end within seconds (killed)."""
    try:
        done = subprocess.run(command, capture_output=True, text=True, timeout=seconds, env=env,
                              errors="replace")
    except subprocess.TimeoutExpired as timed_out:
        out = timed_out.stdout or ""
        out = out.decode(errors="replace") if isinstance(out, bytes) else out
        err = timed_out.stderr or ""
        err = err.decode(errors="replace") if isinstance(err, bytes) else err
        _show(out, err, True)
        check(False, what, "it did not end within %d s" % seconds)
        return None
    lines = done.stdout.splitlines()
    summaries = [m for m in (re.match(r"(\d+) of (\d+) checks passed$", line) for line in lines) if m]
    problems = []
    if done.returncode != 0:
        problems.append("exit status %d" % done.returncode)
    if any(line.startswith("FAIL") for line in lines):
        problems.append("FAIL lines")
    if any(m.group(1) != m.group(2) or m.group(2) == "0" for m in summaries):
        problems.append("not every check passed")
    if expect and not any(re.search(expect, line) for line in lines):
        problems.append("no line matching %r" % expect)
    if not quiet or problems:
        _show(done.stdout, done.stderr, bool(problems))
    for line in lines:
        if line.startswith("skip "):
            skip("%s: %s" % (what, line[5:].split(": ", 1)[0]), line[5:].split(": ", 1)[-1])
        elif line.startswith("narrowed:"):
            print(line, flush=True)
            if STRICT and not expected_skip("%s: %s" % (TEST_NAME, line)):
                failures.append(line)
    check(not problems, what, ", ".join(problems) if problems else None)
    return done


def _show(out, err, failed):
    for line in out.splitlines():
        print("  | " + line, flush=True)
    if failed and err.strip():
        print("  | its standard error, the end:", flush=True)
        for line in err.splitlines()[-30:]:
            print("  | " + line, flush=True)
