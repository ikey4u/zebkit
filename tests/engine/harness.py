"""What the Python engine tests share: their checks, waiting with the engine
pumped, evaluating in a view, the activation a page's gated call needs, a local
page server on a free port, and the summary line with the exit code.

    HERE = os.path.dirname(os.path.abspath(__file__))
    sys.path.insert(0, HERE)
    from harness import zebkit, check, wait, ...  # noqa: E402

Importing it puts the Python binding on sys.path (tests run as `python3 -I`).

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

import http.server
import os
import re
import subprocess
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
_binding = os.path.join(HERE, "..", "..", "bindings", "python")
if _binding not in sys.path:
    sys.path.insert(0, _binding)

import zebkit  # noqa: E402

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
    while True:
        zebkit.pump()
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
    except zebkit.Error as error:
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
