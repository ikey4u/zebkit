"""Measure one engine on one page: what it costs to load, and to keep showing.

    python3 measure.py --label NAME --port PORT [--load 10] [--rest 20] -- COMMAND...

COMMAND starts the engine with the page and with CDP on 127.0.0.1:PORT. The
whole process tree is counted, from /proc: CPU seconds in the first --load
seconds (loading), the share of a core in the --rest seconds after (at
rest), proportional memory (PSS, so that shared pages count once) and the
processes by Chromium's process type. The page's URL and title come from
CDP, which shows that it loaded. One JSON object is printed.

Linux only; scripts/web-bench.sh runs it for zebkit and for Electron.
"""
import argparse
import json
import os
import subprocess
import time
import urllib.request

HZ = os.sysconf("SC_CLK_TCK")


def processes():
    """pid -> (parent pid, CPU seconds so far)."""
    table = {}
    for name in os.listdir("/proc"):
        if not name.isdigit():
            continue
        try:
            fields = open(f"/proc/{name}/stat").read().rsplit(")", 1)[1].split()
        except OSError:
            continue
        table[int(name)] = (int(fields[1]), (int(fields[11]) + int(fields[12])) / HZ)
    return table


def tree(root, table):
    children = {}
    for pid, (parent, _) in table.items():
        children.setdefault(parent, []).append(pid)
    found, todo = [], [root]
    while todo:
        pid = todo.pop()
        found.append(pid)
        todo += children.get(pid, [])
    return found


def kind(pid):
    """Chromium's process type: browser, renderer, gpu-process, utility:<service>, zygote."""
    try:
        # Chromium rewrites its argv into one string with spaces.
        args = open(f"/proc/{pid}/cmdline").read().replace("\0", " ").split()
    except OSError:
        return "gone"
    for arg in args:
        if arg.startswith("--type="):
            if arg[7:] != "utility":
                return arg[7:]
            for sub in args:
                if sub.startswith("--utility-sub-type="):
                    return "utility:" + sub.split("=", 1)[1].split(".")[-2]
            return "utility"
    return "browser"


class Tree:
    def __init__(self, root):
        self.root = root
        self.seen = {}  # pid -> [kind, CPU seconds]; processes that ended keep their last count

    def sample(self):
        table = processes()
        alive = tree(self.root, table)
        for pid in alive:
            if pid in table:
                self.seen.setdefault(pid, [kind(pid), 0.0])[1] = table[pid][1]
        return sum(cpu for _, cpu in self.seen.values()), alive

    def pss_mb(self, alive):
        total = 0
        for pid in alive:
            try:
                for line in open(f"/proc/{pid}/smaps_rollup"):
                    if line.startswith("Pss:"):
                        total += int(line.split()[1])
            except OSError:
                pass
        return total / 1024


def page(port):
    try:
        with urllib.request.urlopen(f"http://127.0.0.1:{port}/json/list", timeout=3) as reply:
            for target in json.load(reply):
                if target.get("type") == "page":
                    return target.get("url", ""), target.get("title", "")
    except Exception as error:  # noqa: BLE001 - reported, the run goes on
        return "", f"(no CDP: {error.__class__.__name__})"
    return "", "(no page)"


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--label", required=True)
    parser.add_argument("--port", required=True)
    parser.add_argument("--load", type=float, default=10)
    parser.add_argument("--rest", type=float, default=20)
    parser.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    command = args.command[1:] if args.command[:1] == ["--"] else args.command

    engine = subprocess.Popen(command, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                              start_new_session=True)
    counted = Tree(engine.pid)
    time.sleep(args.load)
    loaded, _ = counted.sample()
    time.sleep(args.rest)
    total, alive = counted.sample()
    kinds = {}
    for pid in alive:
        if pid in counted.seen:
            name = counted.seen[pid][0]
            kinds[name] = kinds.get(name, 0) + 1
    memory = counted.pss_mb(alive)
    url, title = page(args.port)
    os.killpg(engine.pid, 15)
    time.sleep(1.5)
    try:
        os.killpg(engine.pid, 9)
    except OSError:
        pass

    cpu_by_kind = {}
    for name, cpu in counted.seen.values():
        cpu_by_kind[name] = round(cpu_by_kind.get(name, 0) + cpu, 2)
    print(json.dumps({
        "label": args.label,
        "load_cpu_s": round(loaded, 2),
        "rest_cpu_pct": round((total - loaded) / args.rest * 100, 1),
        "pss_mb": round(memory),
        "processes": len(alive),
        "process_kinds": kinds,
        "cpu_by_kind": dict(sorted(cpu_by_kind.items(), key=lambda item: -item[1])),
        "url": url,
        "title": title[:80],
    }), flush=True)


if __name__ == "__main__":
    main()
