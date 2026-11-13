"""The connection benchmark's driver (scripts/connection-bench.sh,
docs/CONNECTION_REUSE.md, step 0), through the Python binding.

    python3 -I client.py run --control PORT --h2 PORT --h1 PORT --spki HASH --work DIR
                             [--runs N] [--idle-runs N] [--protocols h2,h1] [--idle]
                             [--long] [--only REGEX] [--switch SWITCH ...] [--label TEXT]

runs the cases, each in engine processes of its own (a session: this script
with --session), and prints one JSON object: the medians of each case.

A session starts the engine with its profile and switches, runs its cases and
prints one JSON line per measurement: the case, the time from
zebkit_view_load_url() to the load (or the fetch's answer), the page's
Navigation Timing, and what reached the server meanwhile (its control port:
TCP connections, full and resumed handshakes, requests, 304s).
"""

import argparse
import json
import os
import re
import shutil
import statistics
import subprocess
import sys
import threading
import time
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
# ZEBKIT_BENCH_BINDING: another copy of the binding, for a package older
# than the checkout (a baseline measured again).
sys.path.insert(0, os.environ.get("ZEBKIT_BENCH_BINDING") or os.path.join(HERE, "..", "..", "..", "bindings", "python"))

# How long after a load to wait for what the page or the stack still sends.
SETTLE = 0.25

# ---- the session: one engine process ----


class Loop:
    """The engine pumped when it asks (zebkit_set_wakeup), not on a timer."""

    def __init__(self, zebkit):
        self.zebkit = zebkit
        self.cond = threading.Condition()
        self.deadline = 0.0
        zebkit.set_wakeup(self.wake)

    def wake(self, delay_ms):
        with self.cond:
            at = time.monotonic() + max(delay_ms, 0) / 1000
            if not self.deadline or at < self.deadline:
                self.deadline = at
            self.cond.notify()

    def until(self, condition, seconds=60):
        end = time.monotonic() + seconds
        while True:
            with self.cond:
                self.deadline = 0.0
            self.zebkit.pump()
            if condition():
                return True
            now = time.monotonic()
            if now >= end:
                return False
            with self.cond:
                wait = end - now
                if self.deadline:
                    wait = min(wait, max(self.deadline - now, 0))
                if wait > 0:
                    self.cond.wait(min(wait, 0.05))

    def sleep(self, seconds):
        self.until(lambda: False, seconds)


class Session:
    def __init__(self, args):
        import zebkit
        self.zebkit = zebkit
        self.args = args
        self.control = f"http://127.0.0.1:{args.control}"
        self.ports = {"h2": args.h2, "h1": args.h1}
        argv = ["connection-bench", "--no-sandbox", "--headless", "--no-proxy-server",
                "--host-resolver-rules=MAP *.test 127.0.0.1",
                f"--ignore-certificate-errors-spki-list={args.spki}",
                f"--user-data-dir={args.profile}", *args.switch]
        settings = json.loads(args.settings) if args.settings else {}
        settings.setdefault("logging", {"destination": "none"})
        self.loop = Loop(zebkit)
        if zebkit.init(argv, settings=settings) != 0:
            raise SystemExit("zebkit.init() failed")

    def origin(self, protocol, host="a.bench.test"):
        return f"https://{host}:{self.ports[protocol]}"

    def server(self, path):
        with urllib.request.urlopen(self.control + path, timeout=10) as response:
            return json.load(response)

    def emit(self, case, protocol, ms, stats, **more):
        print(json.dumps({"case": case, "protocol": protocol, "ms": round(ms, 2), "stats": stats, **more}),
              flush=True)

    def view(self, context=None):
        view = self.zebkit.View("about:blank", 800, 600, context=context)
        view.events = []
        view.on_load(lambda event, url: view.events.append((event, url, time.monotonic())))
        self.loop.until(lambda: any(e[0] == self.zebkit.LOAD_FINISHED for e in view.events), 30)
        return view

    def evaluate(self, view, script):
        box = []
        view.evaluate(script, lambda value, error=None: box.append((value, error)))
        self.loop.until(lambda: box, 15)
        return box[0][0] if box and box[0][1] is None else None

    def timing(self, view):
        return self.evaluate(view, """(() => {
          const n = performance.getEntriesByType('navigation')[0];
          const b = performance.getEntriesByType('resource').find((r) => r.name.includes('b.bench.test'));
          const at = (e) => e && Object.fromEntries(['startTime', 'connectStart', 'secureConnectionStart',
              'connectEnd', 'requestStart', 'responseStart', 'responseEnd'].map((k) => [k, Math.round(e[k] * 10) / 10]));
          return {navigation: at(n), b: at(b), restored: window.restored || 0};
        })()""")

    def load(self, case, protocol, view, url, measure_timing=True):
        """zebkit_view_load_url() to the load of url; what reached the server."""
        self.server("/reset")
        view.events.clear()
        start = time.monotonic()
        view.load(url)
        done = lambda: [e for e in view.events if e[1] == url and e[0] in
                        (self.zebkit.LOAD_FINISHED, self.zebkit.LOAD_FAILED)]
        if not self.loop.until(done, 60):
            raise SystemExit(f"{case}: no load of {url}")
        event = done()[0]
        if event[0] == self.zebkit.LOAD_FAILED:
            raise SystemExit(f"{case}: the load of {url} failed")
        ms = (event[2] - start) * 1000
        self.loop.sleep(SETTLE)
        stats = self.server("/stats")
        self.emit(case, protocol, ms, stats, timing=self.timing(view) if measure_timing else None)
        return ms

    def fetch(self, case, protocol, url, context=None):
        self.server("/reset")
        box = []
        start = time.monotonic()
        if context is None:
            self.zebkit.fetch(url, lambda response: box.append((response, time.monotonic())))
        else:
            self.zebkit.fetch_in(context, url, lambda response: box.append((response, time.monotonic())))
        if not self.loop.until(lambda: box, 60):
            raise SystemExit(f"{case}: no answer from {url}")
        response, at = box[0]
        if response["status"] != 200:
            raise SystemExit(f"{case}: {url}: {response['status']} {response['error']}")
        self.loop.sleep(SETTLE)
        self.emit(case, protocol, (at - start) * 1000, self.server("/stats"))

    def back(self, case, protocol, view, url):
        """Back to url: the time to its pageshow, from the back-forward cache or not."""
        self.server("/reset")
        before = self.evaluate(view, "1")
        view.events.clear()
        start = time.monotonic()
        view.back()
        committed = lambda: [e for e in view.events if e[1] == url and e[0] in
                             (self.zebkit.LOAD_COMMITTED, self.zebkit.LOAD_FINISHED)]
        self.loop.until(committed, 30)
        # Restored: no load of its own; the page is shown once committed.
        shown = None
        end = time.monotonic() + 10
        while time.monotonic() < end:
            state = self.evaluate(view, "[document.readyState, window.restored || 0, location.pathname]")
            if state and state[2] == "/page.html" and state[0] == "complete":
                shown = state
                break
        ms = ((committed()[0][2] if committed() else time.monotonic()) - start) * 1000
        self.loop.sleep(SETTLE)
        self.emit(case, protocol, ms, self.server("/stats"), restored=bool(shown and shown[1]))
        del before

    def has(self, name):
        return hasattr(self.zebkit, name)


def session_main(args):
    s = Session(args)
    z = s.zebkit
    page = lambda protocol, host="a.bench.test", extra="": f"{s.origin(protocol, host)}/page.html{extra}"
    cases = re.compile(args.only) if args.only else None
    want = lambda case: not cases or cases.search(case)
    for protocol in args.protocols.split(","):
        if args.session == "main":
            first = s.view()
            if want("A"):
                s.load("A", protocol, first, page(protocol))
            else:
                s.load("warm", protocol, first, page(protocol))
            if want("B"):
                s.load("B", protocol, s.view(), page(protocol))
            if want("C"):
                s.fetch("C", protocol, s.origin(protocol) + "/data")
            if want("I"):
                s.load("I-forward", protocol, first, s.origin(protocol) + "/page2.html", measure_timing=False)
                s.back("I", protocol, first, page(protocol))
            if want("E"):
                context = z.Context()
                s.load("E", protocol, s.view(context), page(protocol))
            if want("H"):
                context = z.Context()
                s.load("H", protocol, s.view(context), page(protocol, extra="?preconnect=1"))
            if want("X"):
                # A host two sites share: its connection and cache entries,
                # partitioned by top-level site or not.
                context = z.Context()
                s.load("X-first", protocol, s.view(context), s.origin(protocol) + "/cross-site.html",
                       measure_timing=False)
                s.load("X", protocol, s.view(context), s.origin(protocol, "c.other.test") + "/cross-site.html",
                       measure_timing=False)
            if s.has("preconnect") and want("P1"):
                context = z.Context()
                view = s.view(context)
                s.server("/reset")
                z.preconnect(s.origin(protocol), 1, context=context)
                s.loop.sleep(1)
                s.load("P1", protocol, view, page(protocol))
            if s.has("preconnect") and args.long and want("P90"):
                context = z.Context()
                view = s.view(context)
                z.preconnect(s.origin(protocol), 1, context=context)
                s.loop.sleep(90)
                s.load("P90", protocol, view, page(protocol))
        elif args.session == "restart":
            if want("D"):
                s.load("D", protocol, s.view(), page(protocol))
        elif args.session == "restart-preconnect":
            # D with the startup setting preconnect naming the origin.
            if want("D-pre"):
                s.load("D-pre", protocol, s.view(), page(protocol))
        elif args.session == "context":
            context = z.Context()
            s.load("A-context", protocol, s.view(context), page(protocol))
            if want("C2"):
                # The engine's profile's network context made first, by a
                # request to another host: C2 is the connection, not that.
                s.fetch("C2-warm", protocol, s.origin(protocol, "c.other.test") + "/data")
                s.fetch("C2", protocol, s.origin(protocol) + "/data")
            if s.has("fetch_in") and want("C3"):
                s.fetch("C3", protocol, s.origin(protocol) + "/data", context=context)
        elif args.session == "idle":
            view = s.view()
            s.load("idle-first", protocol, view, page(protocol), measure_timing=False)
            for _ in range(args.idle_runs):
                s.loop.sleep(30)
                s.load("F", protocol, view, page(protocol))
                s.loop.sleep(300)
                s.load("G", protocol, view, page(protocol))
        elif args.session == "spare":
            view = s.view()
            s.load("spare-first", protocol, view, page(protocol), measure_timing=False)
            s.loop.sleep(2)
            memory = descendants_pss_kib()
            s.load("S", protocol, view, s.origin(protocol, "c.other.test") + "/page2.html", measure_timing=False)
            print(json.dumps({"case": "S-memory", "protocol": protocol, "pss_kib": memory[0],
                              "processes": memory[1]}), flush=True)
    z.shutdown()


def descendants_pss_kib():
    """Proportional set size of this process and its descendants, in KiB."""
    children = {}
    for entry in os.listdir("/proc"):
        if entry.isdigit():
            try:
                with open(f"/proc/{entry}/stat") as f:
                    parent = int(f.read().rsplit(")", 1)[1].split()[1])
                children.setdefault(parent, []).append(int(entry))
            except (OSError, IndexError, ValueError):
                pass
    todo, total, count = [os.getpid()], 0, 0
    while todo:
        pid = todo.pop()
        todo.extend(children.get(pid, []))
        try:
            with open(f"/proc/{pid}/smaps_rollup") as f:
                for line in f:
                    if line.startswith("Pss:"):
                        total += int(line.split()[1])
                        count += 1
        except OSError:
            pass
    return total, count


# ---- the run: the sessions, and the medians ----

def session_command(args, session, profile, extra=(), settings=None, protocols=None):
    command = [sys.executable, "-I", os.path.abspath(__file__), "session", "--session", session,
               "--profile", profile, "--control", str(args.control), "--h2", str(args.h2), "--h1", str(args.h1),
               "--spki", args.spki, "--protocols", protocols or args.protocols, "--idle-runs", str(args.idle_runs)]
    if args.only:
        command += ["--only", args.only]
    if args.long:
        command += ["--long"]
    if settings:
        command += ["--settings", json.dumps(settings)]
    for switch in [*args.switch, *extra]:
        command.append(f"--switch={switch}")
    return command


def results_of(session, code, out, err):
    if code:
        sys.stderr.write(out + err)
        raise SystemExit(f"session {session} failed")
    return [json.loads(line) for line in out.splitlines() if line.startswith("{")]


def run_session(args, session, profile, runs_dir, extra=(), settings=None):
    out = subprocess.run(session_command(args, session, profile, extra, settings), capture_output=True, text=True,
                         timeout=7200)
    return results_of(session, out.returncode, out.stdout, out.stderr)


def summarize(results):
    by = {}
    for r in results:
        by.setdefault((r["case"], r["protocol"]), []).append(r)
    summary = {}
    for (case, protocol), items in sorted(by.items()):
        entry = {"n": len(items)}
        if "ms" in items[0]:
            entry["ms"] = round(statistics.median(i["ms"] for i in items), 1)
            s = [i["stats"] for i in items]
            med = lambda f: statistics.median(f(x) for x in s)
            entry.update({
                "tcp": med(lambda x: x["tcp"]["h2"] + x["tcp"]["h1"]),
                "full": med(lambda x: x["fullHandshakes"]),
                "resumed": med(lambda x: x["resumedHandshakes"]),
                "requests": med(lambda x: x["requests"]),
                "notModified": med(lambda x: x["notModified"]),
            })
            timings = [i.get("timing") for i in items if i.get("timing")]
            navigation = [t["navigation"] for t in timings if t and t.get("navigation")]
            if navigation:
                entry["connectMs"] = statistics.median(n["connectEnd"] - n["connectStart"] for n in navigation)
                entry["responseStart"] = statistics.median(n["responseStart"] for n in navigation)
            b = [t["b"] for t in timings if t and t.get("b")]
            if b:
                entry["bConnectMs"] = statistics.median(x["connectEnd"] - x["connectStart"] for x in b)
            if "restored" in items[0]:
                entry["restored"] = sum(1 for i in items if i["restored"])
        if "pss_kib" in items[0]:
            entry["pssMiB"] = round(statistics.median(i["pss_kib"] for i in items) / 1024, 1)
            entry["processes"] = statistics.median(i["processes"] for i in items)
        summary[f"{case}/{protocol}"] = entry
    return summary


def run_main(args):
    os.makedirs(args.work, exist_ok=True)
    results = []
    for run in range(args.runs):
        profile = os.path.join(args.work, f"profile-{run}")
        shutil.rmtree(profile, ignore_errors=True)
        results += run_session(args, "main", profile, args.work)
        results += run_session(args, "restart", profile, args.work)
        if args.preconnect_setting:
            origins = [f"https://a.bench.test:{getattr(args, p)}/" for p in args.protocols.split(",")]
            results += run_session(args, "restart-preconnect", profile, args.work, settings={"preconnect": origins})
        other = os.path.join(args.work, f"profile-context-{run}")
        shutil.rmtree(other, ignore_errors=True)
        results += run_session(args, "context", other, args.work)
        shutil.rmtree(other, ignore_errors=True)
        if args.spare:
            for switch, name in (((), "off"), (("--zebkit-spare-renderer",), "on")):
                profile_s = os.path.join(args.work, f"profile-spare-{run}")
                shutil.rmtree(profile_s, ignore_errors=True)
                for r in run_session(args, "spare", profile_s, args.work, extra=switch):
                    r["case"] = f"{r['case']}-{name}"
                    results.append(r)
                shutil.rmtree(profile_s, ignore_errors=True)
        sys.stderr.write(f"run {run + 1} of {args.runs}\n")
    if args.idle:
        # Each protocol in an engine of its own, at once, 15 s apart: they
        # mostly wait.
        running = []
        for i, protocol in enumerate(args.protocols.split(",")):
            profile = os.path.join(args.work, f"profile-idle-{protocol}")
            shutil.rmtree(profile, ignore_errors=True)
            if i:
                time.sleep(15)
            running.append(subprocess.Popen(session_command(args, "idle", profile, protocols=protocol),
                                            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True))
        for process in running:
            out, err = process.communicate()
            results += results_of("idle", process.returncode, out, err)
    print(json.dumps({"label": args.label, "switches": args.switch, "summary": summarize(results),
                      "results": results}))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("mode", choices=("run", "session"))
    parser.add_argument("--session")
    parser.add_argument("--profile")
    parser.add_argument("--control", type=int, required=True)
    parser.add_argument("--h2", type=int, required=True)
    parser.add_argument("--h1", type=int, required=True)
    parser.add_argument("--spki", required=True)
    parser.add_argument("--work")
    parser.add_argument("--runs", type=int, default=10)
    parser.add_argument("--idle-runs", type=int, default=3)
    parser.add_argument("--protocols", default="h2,h1")
    parser.add_argument("--idle", action="store_true")
    parser.add_argument("--spare", action="store_true")
    parser.add_argument("--long", action="store_true")
    parser.add_argument("--preconnect-setting", action="store_true")
    parser.add_argument("--only")
    parser.add_argument("--settings")
    parser.add_argument("--label", default="")
    parser.add_argument("--switch", action="append", default=[])
    args = parser.parse_args()
    if args.mode == "session":
        session_main(args)
    else:
        run_main(args)


if __name__ == "__main__":
    main()
