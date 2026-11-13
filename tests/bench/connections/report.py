"""The connection benchmark's report, from client.py's results.

    python3 -I report.py <engine line> <rtt> <runs> <result.json> [<other result.json> ...]

The first result is the report's; each other one adds a column of its load
times, for comparing a change with what it changes (docs/CONNECTION_REUSE.md).
Markdown on stdout.
"""

import json
import sys

CASES = [
    ("A", "first load, new profile"),
    ("B", "a second view, same origin, after A"),
    ("C", "zebkit_fetch() to the origin after A"),
    ("C2", "zebkit_fetch() after A in a zebkit_context"),
    ("C3", "zebkit_fetch_in() the context, after A in it"),
    ("D", "restart, same profile, load again"),
    ("D-pre", "D with the startup setting preconnect"),
    ("E", "a new zebkit_context, same page"),
    ("F", "load again after 30 s idle"),
    ("G", "load again after 5 min idle"),
    ("H", "E with <link rel=preconnect> to b"),
    ("I", "back (back-forward cache)"),
    ("P1", "E after zebkit_preconnect(), 1 s before"),
    ("P90", "E after zebkit_preconnect(), 90 s before"),
    ("X", "a shared host's resources from another site"),
    ("S-off", "to another site, no spare renderer"),
    ("S-on", "to another site, spare renderer"),
]


def main():
    engine, rtt, runs = sys.argv[1], sys.argv[2], sys.argv[3]
    results = [json.load(open(path)) for path in sys.argv[4:]]
    main_result = results[0]
    summary = main_result["summary"]
    label = lambda r: r.get("label") or "this run"
    lines = ["# Connection reuse benchmark", "", f"{engine}.", "",
             f"`scripts/connection-bench.sh` (tests/bench/connections, docs/CONNECTION_REUSE.md): a local HTTPS",
             f"server behind a proxy adding {rtt} ms a round trip (one more for TCP's handshake), a page of 20",
             "images from its origin (half `max-age=3600`, half `no-cache` with an ETag) and a script that adds",
             "one from a second origin; HTTP/2 (ALPN h2) and HTTP/1.1 on ports of their own. Headless, offline.",
             f"Medians of {runs} runs (F and G: of the idle runs). TCP: connections the server accepted; full and",
             "resumed: TLS handshakes; requests and 304s: what reached it. Time: from zebkit_view_load_url() to",
             "the load (C: the fetch's answer; I: the back navigation's commit).", ""]
    if main_result.get("switches"):
        lines += ["Engine switches: `" + " ".join(main_result["switches"]) + "`.", ""]
    for protocol in ("h2", "h1"):
        rows = [(case, what, summary.get(f"{case}/{protocol}")) for case, what in CASES]
        rows = [row for row in rows if row[2]]
        if not rows:
            continue
        others = [r for r in results[1:]]
        lines += [f"## {'HTTP/2' if protocol == 'h2' else 'HTTP/1.1'}", "",
                  "| Case | What | Time ms | TCP | Full | Resumed | Requests | 304 |"
                  + "".join(f" {label(o)} ms |" for o in others),
                  "|---|---|---|---|---|---|---|---|" + "---|" * len(others)]
        for case, what, s in rows:
            extra = ""
            if s.get("restored") is not None:
                extra = f" (restored {s['restored']} of {s['n']})"
            cells = []
            for o in others:
                v = o["summary"].get(f"{case}/{protocol}")
                cells.append(f" {v['ms']} |" if v else " |")
            lines.append(f"| {case} | {what}{extra} | {s['ms']} | {s['tcp']:g} | {s['full']:g} | {s['resumed']:g} "
                         f"| {s['requests']:g} | {s['notModified']:g} |" + "".join(cells))
        memory = [(case, summary.get(f"S-memory-{case}/{protocol}")) for case in ("off", "on")]
        if all(m for _, m in memory):
            lines += ["", "Spare renderer, memory of the engine's processes after the first load: "
                      + ", ".join(f"{name} {m['pssMiB']} MiB PSS in {m['processes']:g} processes"
                                  for name, m in memory) + "."]
        lines.append("")
    print("\n".join(lines))


if __name__ == "__main__":
    main()
