"""zebkit and Electron, measured the same way (scripts/electron-compare.sh).

    compare.py run --work DIR --zebkit HOST --electron EXE --bench DIR [--pdf FILE]
                   [--series a,b] [--runs N] [--ua UA]
    compare.py report --work DIR --out DIR --meta FILE

What makes it equal is that nothing below knows which engine it runs, but for
the command of each:

- Both get the same switches, environment, X display (Xvfb, no GPU), window
  content size (checked by the probe series), pages over the same local HTTP
  server and, where a page's load has to be read over CDP, CDP.
- One clock: this program's. A run starts just before the engine's exec; a
  page's marks arrive here, or carry the page's wall clock.
- Every process counts: this program is a subreaper, so what an engine starts
  and disowns (Electron's crash handler) stays among its descendants. CPU is
  /proc's utime + stime of every one sampled each second, those that ended
  keeping their last count; memory is PSS plus swapped PSS over all of them.
- Engines take turns: zebkit first in odd pairs, Electron first in even ones.
- Each pair waits for a quiet machine: load about 4 (under 4.5), no build or suite running.
  The load is written with every run. A series in which a build appeared, or
  the load went over 8, is marked discarded and run again.

Raw results go to WORK/results.jsonl, one line per run.

On macOS (a window on the real display, no Xvfb) there is no /proc: CPU comes
from ps, memory is the sum of footprint's phys_footprint, which leaves shared
code out and so is not PSS and is not comparable with Linux's numbers, and the
caches cannot be dropped, so there is no cold start.
"""
import argparse
import collections
import ctypes
import http.server
import json
import os
import shutil
import signal
import statistics
import subprocess
import sys
import threading
import time
import urllib.parse
import urllib.request

MACOS = sys.platform == "darwin"
HZ = os.sysconf("SC_CLK_TCK")
PR_SET_CHILD_SUBREAPER = 36
QUIET_LOAD = 4.5
SPOILED_LOAD = 8.0
SCRIPTS = ("engine-test.sh", "playwright-suite.sh", "build.sh", "package.sh", "web-bench.sh", "layout-bench.sh",
           "automation-bench.sh", "release.sh", "dist.sh", "ui-bench.sh")
TOOLS = ("ninja", "siso", "autoninja", "rustc", "cargo", "ld.lld", "lld", "gn")


def load1():
    return round(os.getloadavg()[0], 2)


def other_work():
    """Builds and suites running on this machine, by name, not by a text search over command lines."""
    if MACOS:
        return []  # builds happen elsewhere
    found = set()
    for name in os.listdir("/proc"):
        if not name.isdigit() or int(name) == os.getpid():
            continue
        try:
            comm = open(f"/proc/{name}/comm").read().strip()
            args = open(f"/proc/{name}/cmdline").read().split("\0")
        except OSError:
            continue
        if comm in TOOLS or comm.startswith("clang"):
            found.add(comm)
        elif comm in ("bash", "sh", "python3") and len(args) > 1:
            base = os.path.basename(args[1])
            if base in SCRIPTS:
                found.add(base)
    return sorted(found)


def wait_quiet(limit):
    """Waits for a quiet machine, at most limit seconds; False if it was not."""
    deadline = time.time() + limit
    while True:
        work = other_work()
        load = load1()
        if load < QUIET_LOAD and not work:
            return True
        if time.time() > deadline:
            print(f"not quiet after {limit} s: load {load}, {work}", flush=True)
            return False
        time.sleep(10)


def macos_table():
    table = {}
    for line in subprocess.check_output(["ps", "-axo", "pid=,ppid=,stat=,time=,comm="], text=True).splitlines():
        fields = line.split(None, 4)
        if len(fields) < 5 or fields[4] == "ps" or fields[4].endswith("/ps"):
            continue
        minutes, seconds = fields[3].split(":")
        table[int(fields[0])] = (fields[2][0], int(minutes) * 60 + float(seconds), int(fields[1]))
    return table


def command_line(pid):
    if MACOS:
        return subprocess.run(["ps", "-o", "command=", "-p", str(pid)], capture_output=True, text=True).stdout.split()
    # Chromium rewrites its argv into one string with spaces.
    return open(f"/proc/{pid}/cmdline").read().replace("\0", " ").split()


def descendants(skip):
    """Every process below this one, except skip (the X server): pid -> (state, CPU seconds, parent)."""
    table = macos_table() if MACOS else {}
    for name in [] if MACOS else os.listdir("/proc"):
        if not name.isdigit():
            continue
        try:
            fields = open(f"/proc/{name}/stat").read().rsplit(")", 1)[1].split()
        except OSError:
            continue
        table[int(name)] = (fields[0], (int(fields[11]) + int(fields[12])) / HZ, int(fields[1]))
    children = {}
    for pid, row in table.items():
        children.setdefault(row[2], []).append(pid)
    found, todo = {}, list(children.get(os.getpid(), []))
    while todo:
        pid = todo.pop()
        if pid == skip:
            continue
        found[pid] = table[pid]
        todo += children.get(pid, [])
    return found


def kind(pid):
    """Chromium's process type; the crash handler by its name; browser otherwise."""
    try:
        args = command_line(pid)
    except OSError:
        return "gone"
    if args and "crashpad" in os.path.basename(args[0]):
        return "crashpad"
    for arg in args:
        if arg.startswith("--type="):
            if arg[7:] != "utility":
                return arg[7:]
            for sub in args:
                if sub.startswith("--utility-sub-type="):
                    return "utility:" + sub.split("=", 1)[1].split(".")[0]
            return "utility"
    return "browser"


def x_cpu(pid):
    if MACOS:
        return 0.0
    fields = open(f"/proc/{pid}/stat").read().rsplit(")", 1)[1].split()
    return (int(fields[11]) + int(fields[12])) / HZ


def main_thread_cpu():
    """Renderers' main threads: pid -> nanoseconds on a CPU, read to the nanosecond."""
    out = {}
    for pid in [] if MACOS else descendants(None):
        if kind(pid) == "renderer":
            try:
                out[pid] = int(open(f"/proc/{pid}/task/{pid}/schedstat").read().split()[0])
            except OSError:
                pass
    return out


class Marks:
    """What the page reported in this run."""

    def __init__(self):
        self.lock = threading.Lock()
        self.arrived = {}
        self.values = {}
        self.result = None
        self.done = threading.Event()
        self.changed = threading.Condition(self.lock)
        self.layout_begin = {}
        self.layout_end = {}

    def mark(self, name, at):
        with self.changed:
            self.arrived[name] = time.time()
            if at:
                self.values[name] = float(at) / 1000
            self.changed.notify_all()

    def wait(self, names, limit):
        deadline = time.time() + limit
        with self.changed:
            while not all(n in self.arrived for n in names):
                left = deadline - time.time()
                if left <= 0:
                    return False
                self.changed.wait(left)
        return True


class Server(http.server.ThreadingHTTPServer):
    daemon_threads = True
    marks = None
    pdf = None
    entry = None  # the path whose first GET marks the page's request


class Handler(http.server.SimpleHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *args):
        pass

    def reply(self):
        self.send_response(204)
        self.send_header("Content-Length", "0")
        self.end_headers()

    def do_POST(self):
        body = self.rfile.read(int(self.headers.get("Content-Length", 0)))
        self.server.marks.result = json.loads(body)
        self.server.marks.mark("result", None)
        self.reply()

    def do_GET(self):
        url = urllib.parse.urlsplit(self.path)
        query = dict(urllib.parse.parse_qsl(url.query))
        marks = self.server.marks
        if url.path == "/mark":
            name = query["name"]
            if name == "layout-begin":
                marks.layout_begin = main_thread_cpu()
            elif name == "layout-end":
                marks.layout_end = main_thread_cpu()
            marks.mark(name, query.get("at"))
            return self.reply()
        if url.path == "/result":
            query.pop("profile", None)
            marks.result = query
            marks.mark("result", None)
            return self.reply()
        if url.path == "/favicon.ico":
            return self.reply()
        if url.path == self.server.entry and "request" not in marks.arrived:
            marks.mark("request", None)
        if url.path == "/long.pdf":
            size = os.path.getsize(self.server.pdf)
            self.send_response(200)
            self.send_header("Content-Type", "application/pdf")
            self.send_header("Content-Length", str(size))
            self.end_headers()
            with open(self.server.pdf, "rb") as source:
                shutil.copyfileobj(source, self.wfile)
            return None
        return super().do_GET()

    def end_headers(self):
        self.send_header("Cache-Control", "no-cache")
        super().end_headers()


class Bench:
    def __init__(self, args):
        self.args = args
        self.work = args.work
        self.commands = {"zebkit": [args.zebkit], "electron": [args.electron]}
        self.extra = args.switch or []
        self.xvfb_pid = None
        self.display = ""
        if not MACOS:
            ctypes.CDLL(None).prctl(PR_SET_CHILD_SUBREAPER, 1, 0, 0, 0)
            read, write = os.pipe()
            xvfb = subprocess.Popen(["Xvfb", "-displayfd", str(write), "-screen", "0", "1920x1080x24", "-nolisten", "tcp"],
                                    pass_fds=(write,), stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            self.xvfb_pid = xvfb.pid
            os.close(write)
            self.display = ":" + os.read(read, 16).decode().strip()
            os.close(read)
        handler = lambda *a, **k: Handler(*a, directory=args.bench, **k)  # noqa: E731
        self.server = Server(("127.0.0.1", 0), handler)
        self.server.pdf = args.pdf
        self.port = self.server.server_address[1]
        threading.Thread(target=self.server.serve_forever, daemon=True).start()
        self.results = open(os.path.join(self.work, "results.jsonl"), "a")
        self.mapped = {"zebkit": set(), "electron": set()}

    def environment(self, url, bind):
        env = {k: v for k, v in os.environ.items() if k not in ("WAYLAND_DISPLAY", "ELECTRON_RUN_AS_NODE")}
        env.update(DISPLAY=self.display, BENCH_URL=url, BENCH_UA=self.args.ua, LANG="en_US.UTF-8", LANGUAGE="en")
        if bind:
            env["BENCH_BIND"] = "1"
        return env

    def start(self, engine, url, bind=False, cdp=0, keep_profile=False):
        profile = os.path.join(self.work, f"profile-{engine}")
        if not keep_profile:
            shutil.rmtree(profile, ignore_errors=True)
        command = self.commands[engine] + ([] if MACOS else ["--ozone-platform=x11"]) + [f"--user-data-dir={profile}"] + self.extra
        if cdp:
            command.append(f"--remote-debugging-port={cdp}")
        self.server.marks = Marks()
        self.server.entry = urllib.parse.urlsplit(url).path
        stderr = open(os.path.join(self.work, f"{engine}.log"), "w")
        started = time.time()
        process = subprocess.Popen(command, env=self.environment(url, bind), stdout=stderr, stderr=stderr, start_new_session=True)
        return process, started

    def reap(self, alive):
        for pid, row in alive.items():
            if row[0] == "Z":
                try:
                    os.waitpid(pid, os.WNOHANG)
                except OSError:
                    pass

    def sample(self, seen):
        """Adds the descendants' CPU to seen (pid -> [kind, seconds]); the total, and those alive."""
        alive = descendants(self.xvfb_pid)
        for pid, row in alive.items():
            entry = seen.get(pid)
            if entry is None:
                entry = seen[pid] = [kind(pid), 0.0]
            elif entry[0] == "gone" or row[0] != "Z" and entry[0] == "browser" and pid != self.root:
                # A process's type shows in its command line only after exec.
                entry[0] = kind(pid) if row[0] != "Z" else entry[0]
            entry[1] = max(entry[1], row[1])
        self.reap(alive)
        return sum(e[1] for e in seen.values()), {p: r for p, r in alive.items() if r[0] != "Z"}

    def pss(self, alive):
        total = 0
        for pid in alive:
            if MACOS:
                out = subprocess.run(["footprint", str(pid)], capture_output=True, text=True).stdout
                for line in out.splitlines():
                    if "phys_footprint:" in line:
                        value = line.split(":")[1].split()
                        total += float(value[0]) * {"KB": 1, "MB": 1024, "GB": 1048576}[value[1]]
                continue
            try:
                for line in open(f"/proc/{pid}/smaps_rollup"):
                    if line.startswith(("Pss:", "SwapPss:")):
                        total += int(line.split()[1])
            except OSError:
                pass
        return total / 1024

    def stop(self, process):
        for sig, patience in ((signal.SIGTERM, 40), (signal.SIGKILL, 40)):
            alive = descendants(self.xvfb_pid)
            for pid in alive:
                try:
                    os.kill(pid, sig)
                except OSError:
                    pass
            for _ in range(patience):
                time.sleep(0.1)
                process.poll()
                self.reap(descendants(self.xvfb_pid))
                if not {p for p, r in descendants(self.xvfb_pid).items() if r[0] != "Z"}:
                    return
        print("processes left after SIGKILL", flush=True)

    def costs(self, engine, url, cdp_port, load, rest, bind=False):
        """A page for load + rest seconds: CPU, memory and processes of everything the engine started."""
        process, _ = self.start(engine, url, bind=bind, cdp=cdp_port)
        self.root = process.pid
        seen = {}
        x_before = x_cpu(self.xvfb_pid)
        started = time.time()
        loaded_cpu = 0
        while time.time() - started < load:
            time.sleep(1)
            loaded_cpu, _ = self.sample(seen)
        x_loaded = x_cpu(self.xvfb_pid)
        rest_started = time.time()
        while time.time() - rest_started < rest:
            time.sleep(1)
            total, alive = self.sample(seen)
        elapsed_rest = time.time() - rest_started
        memory = self.pss(alive)
        kinds = {}
        pss_kinds = {}
        for pid in alive:
            name = kind(pid)
            kinds[name] = kinds.get(name, 0) + 1
            if not MACOS:
                pss_kinds[name] = pss_kinds.get(name, 0) + self.pss([pid])
        title, page_url = self.title(cdp_port)
        args = {}
        for pid in alive:
            if kind(pid) == "gpu-process":
                args["gpu"] = [a for a in command_line(pid)
                               if a.startswith(("--use-gl", "--use-angle", "--disable-gpu", "--enable-unsafe", "--ozone"))]
        x_end = x_cpu(self.xvfb_pid)
        self.stop(process)
        cpu_kinds = {}
        for name, cpu in seen.values():
            cpu_kinds[name] = round(cpu_kinds.get(name, 0) + cpu, 2)
        return dict(load_cpu_s=round(loaded_cpu, 2), rest_cpu_pct=round((total - loaded_cpu) / elapsed_rest * 100, 2),
                    pss_mb=round(memory, 1), processes=len(alive), renderers=kinds.get("renderer", 0), kinds=kinds,
                    pss_kinds={k: round(v, 1) for k, v in pss_kinds.items()}, cpu_kinds=cpu_kinds,
                    x_load_cpu_s=round(x_loaded - x_before, 2), x_rest_cpu_pct=round((x_end - x_loaded) / elapsed_rest * 100, 2),
                    title=title[:80], url=page_url[:200], **args)

    def title(self, port):
        try:
            with urllib.request.urlopen(f"http://127.0.0.1:{port}/json/list", timeout=3) as reply:
                for target in json.load(reply):
                    if target.get("type") == "page":
                        return target.get("title", ""), target.get("url", "")
        except Exception as error:  # noqa: BLE001 - written down, the run goes on
            return f"(no CDP: {error.__class__.__name__})", ""
        return "(no page)", ""

    def until_result(self, engine, url, limit=180, bind=False):
        process, started = self.start(engine, url, bind=bind)
        self.root = process.pid
        marks = self.server.marks
        seen = {}
        deadline = started + limit
        ok = False
        while time.time() < deadline:
            self.sample(seen)
            if marks.wait(["result"], 1):
                ok = True
                break
        cpu, alive = self.sample(seen)
        memory = self.pss(alive)
        self.stop(process)
        if not ok:
            raise RuntimeError(f"{engine}: no result from {url} in {limit} s")
        return marks, dict(result=marks.result, total_cpu_s=round(cpu, 2), pss_mb=round(memory, 1),
                           seconds=round(marks.arrived["result"] - started, 2), processes=len(alive))

    # ---- The series: each is a function (engine, run index) -> dict ----

    def local(self, path):
        return f"http://127.0.0.1:{self.port}/{path}"

    def probe(self, engine, i):
        _, out = self.until_result(engine, self.local("equal/pages/probe.html"), 60)
        out["result"] = json.loads(out["result"]["probe"])
        return out

    def evict(self, engine):
        """Takes the engine's files out of the page cache: those of its package, and what it maps from the system."""
        files = set(self.mapped[engine]) | {self.commands[engine][0]}
        for top in [self.args.package if engine == "zebkit" else os.path.dirname(self.commands[engine][0])]:
            for folder, _, names in os.walk(top):
                files.update(os.path.join(folder, n) for n in names)
        for path in files:
            try:
                fd = os.open(path, os.O_RDONLY)
                os.posix_fadvise(fd, 0, 0, os.POSIX_FADV_DONTNEED)
                os.close(fd)
            except OSError:
                pass

    @staticmethod
    def resident(path):
        """The share of a file in the page cache."""
        libc = ctypes.CDLL(None, use_errno=True)
        libc.mmap.restype = ctypes.c_void_p
        libc.mmap.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_long]
        libc.mincore.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_char_p]
        libc.munmap.argtypes = [ctypes.c_void_p, ctypes.c_size_t]
        size = os.path.getsize(path)
        pages = (size + 4095) // 4096
        vector = ctypes.create_string_buffer(pages)
        fd = os.open(path, os.O_RDONLY)
        address = libc.mmap(None, size, 1, 1, fd, 0)
        libc.mincore(address, size, vector)
        libc.munmap(address, size)
        os.close(fd)
        return sum(b & 1 for b in vector.raw) / pages

    def main_file(self, engine):
        return os.path.join(self.args.package, "libzebkit.so.1") if engine == "zebkit" else self.commands[engine][0]

    def start_up(self, engine, keep_profile):
        url = self.local("equal/pages/first.html")
        process, started = self.start(engine, url, keep_profile=keep_profile)
        self.root = process.pid
        marks = self.server.marks
        seen = {}
        ok = marks.wait(["request", "fcp", "load", "frame"], 120)
        cpu, alive = self.sample(seen)
        try:
            maps = open(f"/proc/{process.pid}/maps").read()
            self.mapped[engine].update(line.split()[-1] for line in maps.splitlines() if "/" in line.split()[-1])
        except OSError:
            pass
        memory = self.pss(alive)
        self.stop(process)
        if not ok:
            raise RuntimeError(f"{engine}: no first frame")
        return dict(request_ms=round((marks.arrived["request"] - started) * 1000, 1),
                    fcp_ms=round((marks.values["fcp"] - started) * 1000, 1),
                    load_ms=round((marks.values["load"] - started) * 1000, 1),
                    frame_ms=round((marks.arrived["frame"] - started) * 1000, 1),
                    cpu_s=round(cpu, 2), pss_mb=round(memory, 1), processes=len(alive))

    def warm(self, engine, i):
        return self.start_up(engine, keep_profile=True)

    def cold(self, engine, i):
        self.evict(engine)
        share = self.resident(self.main_file(engine))
        out = self.start_up(engine, keep_profile=False)
        out["resident_before"] = round(share, 3)
        return out

    def layout(self, engine, i):
        marks, out = self.until_result(engine, self.local("web/local/layout.html"), 120)
        grown = {p: marks.layout_end[p] - marks.layout_begin.get(p, 0) for p in marks.layout_end}
        row = dict(layout_ms=float(out["result"]["layout_ms"]))
        if grown:
            row["layout_cpu_ms"] = round(max(grown.values()) / 60 / 1e6, 3)
        return row

    def js(self, engine, i):
        _, out = self.until_result(engine, self.local(f"index.html?profile={engine}&run={i}"), 240)
        result = {k: float(v) for k, v in out["result"].items() if k != "run"}
        result["all"] = sum(result.values())
        return result

    def calls(self, engine, i):
        _, out = self.until_result(engine, self.local("equal/pages/calls.html"), 240, bind=True)
        if "error" in out["result"]:
            raise RuntimeError(f"{engine}: {out['result']['error']}")
        flat = {}
        for size, row in out["result"].items():
            flat[f"{size}_ms"] = row["median_ms"]
            flat[f"{size}_p95_ms"] = row["p95_ms"]
        return flat

    def document(self, engine, i):
        _, out = self.until_result(engine, self.local("equal/pages/document.html"), 180)
        return dict(out["result"], total_cpu_s=out["total_cpu_s"], pss_mb=out["pss_mb"], seconds=out["seconds"])

    def series(self, name, function, runs, retries=2, **extra):
        for attempt in range(retries + 1):
            rows, clean = [], True
            for i in range(runs):
                if not wait_quiet(self.args.quiet_limit):
                    clean = False
                    break
                order = ("zebkit", "electron") if i % 2 == 0 else ("electron", "zebkit")
                for engine in order:
                    before = load1()
                    try:
                        row = function(engine, i)
                    except RuntimeError as error:
                        row = dict(error=str(error))
                    after = load1()
                    work = other_work()
                    row.update(series=name, engine=engine, run=i, first=engine == order[0], attempt=attempt,
                               load_before=before, load_after=after, at=round(time.time()), **extra)
                    if work:
                        row["other_work"] = work
                    if work or after > SPOILED_LOAD:
                        clean = False
                    rows.append(row)
                    print(json.dumps({k: v for k, v in row.items() if k not in ("kinds", "pss_kinds", "cpu_kinds")})[:400], flush=True)
                    time.sleep(1)
                if not clean:
                    break
            for row in rows:
                row["discarded"] = not clean
                self.results.write(json.dumps(row) + "\n")
            self.results.flush()
            if clean:
                return True
            print(f"series {name}: discarded, attempt {attempt + 1}", flush=True)
        return False

    def run(self):
        wanted = self.args.series.split(",") if self.args.series else None
        runs = self.args.runs
        plan = [("probe", self.probe, 1), ("cold", self.cold, max(runs, 7)), ("warm", self.warm, max(runs, 7)),
                ("layout", self.layout, max(runs, 7)), ("js", self.js, runs), ("calls", self.calls, runs),
                ("document", self.document, runs)]
        for name, function, count in plan:
            if wanted and name not in wanted:
                continue
            if name in ("cold", "warm"):
                # A profile, the files in the page cache, and the list of what each maps from the system.
                for engine in self.commands:
                    self.start_up(engine, keep_profile=False)
                    time.sleep(1)
            self.series(name, function, count)
        pages = [("blank", "equal/pages/blank.html"), ("static", "web/local/static.html"),
                 ("animation", "web/local/animation.html"), ("heavy", "web/local/heavy.html")]
        if self.args.pdf:
            pages.append(("long-pdf", "long.pdf"))
        for page, path in pages:
            name = "page:" + page
            if wanted and name not in wanted and "pages" not in wanted:
                continue
            url = self.local(path)
            self.series(name, lambda engine, i, url=url: self.costs(engine, url, 9431, self.args.load, self.args.rest), runs)
        if not wanted or "sites" in wanted or any(w.startswith("site:") for w in wanted):
            for line in open(os.path.join(self.args.bench, "web", "sites.txt")):
                url = line.strip()
                if not url or url.startswith("#"):
                    continue
                name = "site:" + url.removeprefix("https://")
                if wanted and name not in wanted and "sites" not in wanted:
                    continue
                self.series(name, lambda engine, i, url=url: self.costs(engine, url, 9431, self.args.load, self.args.rest), runs)


# ---- The report ----

MEASURES = [
    ("Startup, files not in cache, new profile", "cold", [("to the page's request", "request_ms", "ms"), ("to first paint (FCP)", "fcp_ms", "ms"), ("to a frame after load", "frame_ms", "ms"), ("CPU until then, all processes", "cpu_s", "s")]),
    ("Startup, files in cache, profile kept", "warm", [("to the page's request", "request_ms", "ms"), ("to first paint (FCP)", "fcp_ms", "ms"), ("to a frame after load", "frame_ms", "ms"), ("CPU until then, all processes", "cpu_s", "s"), ("memory at that frame", "pss_mb", "MB")]),
    ("Text layout, a chapter", "layout", [("time per layout, the median of the last 50", "layout_ms", "ms"), ("renderer main thread's CPU, the mean of all 60 with the first 10", "layout_cpu_ms", "ms")]),
    ("JavaScript and DOM, tests/bench/index.html", "js", [(w, w, "ms") for w in ("all", "loop", "objects", "strings", "json", "regex", "sort", "parse", "gc", "jit", "dom")]),
    ("Calls from the page to the host and back", "calls", [("64 bytes", "64B_ms", "ms"), ("64 KiB", "64KiB_ms", "ms"), ("1 MiB", "1MiB_ms", "ms"), ("16 MiB", "16MiB_ms", "ms")]),
    ("Long document (200,000 words)", "document", [("to its first frame", "first_frame_ms", "ms"), ("longest task", "longest_task_ms", "ms"), ("scrolling frame, p50", "frame_p50_ms", "ms"), ("scrolling frame, p95", "frame_p95_ms", "ms"), ("scrolling frame, max", "frame_max_ms", "ms"), ("CPU, all processes", "total_cpu_s", "s"), ("memory", "pss_mb", "MB")]),
]
COST_MEASURES = [("CPU loading (first 10 s)", "load_cpu_s", "s"), ("CPU at rest (20 s after), % of a core", "rest_cpu_pct", "%"), ("X server's CPU at rest (Linux), % of a core", "x_rest_cpu_pct", "%"), ("memory of all processes", "pss_mb", "MB"), ("processes", "processes", ""), ("renderers", "renderers", "")]


def stat(values):
    return statistics.median(values), min(values), max(values)


def number(value):
    return f"{value:.0f}" if abs(value) >= 100 else (f"{value:.1f}" if abs(value) >= 10 else f"{value:.2f}".rstrip("0").rstrip(".") if value else "0")


def compare(zebkit, electron):
    """(zebkit cell, electron cell, verdict). Within noise: the ranges of the runs overlap."""
    z, e = stat(zebkit), stat(electron)
    cell = lambda s: f"{number(s[0])} ({number(s[1])}-{number(s[2])})"  # noqa: E731
    if z[0] == e[0]:
        verdict = "level"
    else:
        winner = "zebkit" if z[0] < e[0] else "Electron"
        low, high = sorted((z[0], e[0]))
        percent = (high - low) / high * 100
        overlap = z[1] <= e[2] and e[1] <= z[2]
        verdict = f"{winner}, {percent:.0f}% less" + (", within noise" if overlap else "")
    return cell(z), cell(e), verdict


def report(args):
    rows = [json.loads(line) for line in open(os.path.join(args.work, "results.jsonl"))]
    meta = json.load(open(args.meta))
    kept = [r for r in rows if not r.get("discarded") and "error" not in r]
    discarded = sum(1 for r in rows if r.get("discarded"))
    errors = [r for r in rows if "error" in r and not r.get("discarded")]
    # Only the last clean run of a series counts, should one have been repeated.
    last = {}
    for r in kept:
        last[r["series"]] = max(last.get(r["series"], 0), r["at"])
    groups = {}
    for r in kept:
        groups.setdefault(r["series"], {}).setdefault(r["engine"], []).append(r)
    latest = {}
    for series, engines in groups.items():
        latest[series] = {}
        for engine, runs in engines.items():
            top = max(x.get("attempt", 0) for x in runs)
            latest[series][engine] = runs
    lines = meta["intro"][:]
    wire = {}
    def table(title, series, measures):
        engines = latest.get(series)
        if not engines or len(engines) < 2:
            return
        lines.extend(["", f"### {title}", "", "| Measure | zebkit | Electron | Fewer or faster |", "|---|---:|---:|---|"])
        for label, key, unit in measures:
            z = [r[key] for r in engines["zebkit"] if key in r]
            e = [r[key] for r in engines["electron"] if key in r]
            if not z or not e:
                continue
            zc, ec, verdict = compare(z, e)
            lines.append(f"| {label}{', ' + unit if unit else ''} | {zc} | {ec} | {verdict} |")
            wire[f"{series}.{key}"] = dict(zebkit=stat(z), electron=stat(e), verdict=verdict)
        loads = [r["load_before"] for rs in engines.values() for r in rs]
        lines.append(f"\nLoad before a run: {min(loads):.1f} to {max(loads):.1f}. Runs: {len(engines['zebkit'])} of each.")
    for title, series, measures in MEASURES:
        table(title, series, measures)
    for section, prefix in (("Local pages", "page:"), ("Sites", "site:")):
        names = [s for s in latest if s.startswith(prefix)]
        if not names:
            continue
        lines.extend(["", f"## {section}"])
        for name in names:
            table(name[len(prefix):], name, COST_MEASURES)
            engines = latest[name]
            if len(engines) == 2:
                t = {e: sorted(f"{title} ({n})" for title, n in collections.Counter(r.get('title', '')[:50] for r in engines[e]).items()) for e in engines}
                k = {e: engines[e][len(engines[e]) // 2].get('kinds', {}) for e in engines}
                lines.append(f"\nTitles, zebkit: {t['zebkit']}; Electron: {t['electron']}. Processes in a middle run, zebkit: {k['zebkit']}; Electron: {k['electron']}.")
        # A page that was not the same page in both, in any run, is left out of the sums.
        if prefix == "site:":
            different = [n for n in names if len({r.get("title", "") for rs in latest[n].values() for r in rs}) > 1]
            names = [n for n in names if n not in different]
            if different:
                lines.extend(["", "Left out of the sums, for showing a different page in some run: " + ", ".join(n[len(prefix):] for n in different) + "."])
        totals = {}
        for key in ("load_cpu_s", "rest_cpu_pct", "pss_mb"):
            for engine in ("zebkit", "electron"):
                totals.setdefault(engine, {})[key] = sum(stat([r[key] for r in latest[n][engine]])[0] for n in names if len(latest[n]) == 2)
        wins = {key: sum(1 for n in names if len(latest[n]) == 2 and stat([r[key] for r in latest[n]["zebkit"]])[0] < stat([r[key] for r in latest[n]["electron"]])[0]) for key in ("load_cpu_s", "rest_cpu_pct", "pss_mb")}
        wire[prefix + "totals"] = dict(totals=totals, zebkit_wins=wins, pages=len(names))
        lines.extend(["", f"Sums of the medians over {len(names)} pages, zebkit / Electron: CPU loading {totals['zebkit']['load_cpu_s']:.1f} / {totals['electron']['load_cpu_s']:.1f} s (zebkit less on {wins['load_cpu_s']}), CPU at rest {totals['zebkit']['rest_cpu_pct']:.1f} / {totals['electron']['rest_cpu_pct']:.1f} % of a core (zebkit less on {wins['rest_cpu_pct']}), memory {totals['zebkit']['pss_mb']:.0f} / {totals['electron']['pss_mb']:.0f} MB (zebkit less on {wins['pss_mb']})."])
    lines.extend(["", f"Runs discarded for a machine that was not quiet: {discarded}. Runs that failed: {len(errors)}" + ("." if not errors else ": " + "; ".join(sorted({r['series'] + ' ' + r['engine'] for r in errors})) + ".")])
    open(os.path.join(args.out, "electron-compare.md"), "w").write("\n".join(lines) + "\n")
    json.dump(dict(meta={k: v for k, v in meta.items() if k != "intro"}, results=wire), open(os.path.join(args.out, "electron-compare.json"), "w"), indent=1)
    print("\n".join(lines))


def main():
    parser = argparse.ArgumentParser()
    sub = parser.add_subparsers(dest="command", required=True)
    run = sub.add_parser("run")
    run.add_argument("--work", required=True)
    run.add_argument("--zebkit", required=True)
    run.add_argument("--package", required=True)
    run.add_argument("--electron", required=True)
    run.add_argument("--bench", required=True)
    run.add_argument("--pdf")
    run.add_argument("--ua", required=True)
    run.add_argument("--series")
    run.add_argument("--runs", type=int, default=5)
    run.add_argument("--load", type=float, default=10)
    run.add_argument("--rest", type=float, default=20)
    run.add_argument("--quiet-limit", type=float, default=1200)
    run.add_argument("--switch", action="append")
    made = sub.add_parser("report")
    made.add_argument("--work", required=True)
    made.add_argument("--out", required=True)
    made.add_argument("--meta", required=True)
    args = parser.parse_args()
    if args.command == "run":
        bench = Bench(args)

        def stop(signum, frame):
            raise SystemExit(1)

        signal.signal(signal.SIGTERM, stop)
        signal.signal(signal.SIGPIPE, signal.SIG_IGN)
        try:
            bench.run()
        finally:
            for pid in descendants(None):
                try:
                    os.kill(pid, signal.SIGKILL)
                except OSError:
                    pass
    else:
        report(args)


if __name__ == "__main__":
    main()
