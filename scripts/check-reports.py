#!/usr/bin/env python3
"""Which measurement reports in docs/reports/ are stale against upstream.lock.

    check-reports.py <reports dir> <profiles dir> <CHROMIUM_VERSION> <ELECTRON_VERSION>

Run by scripts/check.sh (also --static). A report is stale when it names a
Chromium release of zebkit's, or an Electron release, other than the ones
upstream.lock pins now: it was measured before a bump and has to be measured
again (scripts/measure.sh, web-bench.sh, automation-bench.sh, ...), or kept as
history. The rules, on each docs/reports/*.md:

- A report with a line beginning "> History:" is history, kept on purpose, and
  is not checked (the line says what it records).
- "Engine: zebkit <version> (Chromium <version>), ..." is the line
  scripts/measure.sh, web-bench.sh, automation-bench.sh and library-bench.sh
  write into each of their reports: its Chromium is the engine's that was
  measured. The reports those scripts write (<profile>.md, web-bench.md,
  automation-bench.md, library-bench.md) must have it.
- Every other "Chromium <a.b.c.d>" must be the pinned one too, except
  Electron's own, written "Electron <version> (Chromium <a.b.c.d>" as
  scripts/electron-baseline.sh writes it, for the pinned Electron.
- Every "Electron <version>" must be the pinned Electron.

Prints each stale report with why; exits 1 when there is one.
"""

import pathlib
import re
import sys

CHROMIUM = re.compile(r"Chromium (\d+\.\d+\.\d+\.\d+)")
ELECTRON = re.compile(r"Electron (\d+\.\d+\.\d+(?:-[0-9A-Za-z.]+)?)")
ELECTRONS_CHROMIUM = re.compile(r"Electron (\S+) \(Chromium (\d+\.\d+\.\d+\.\d+)")
ENGINE = re.compile(r"^(?:- )?Engine: zebkit (\S+) \(Chromium (\d+\.\d+\.\d+\.\d+)\)", re.M)


def problems(path, text, chromium, electron, needs_engine_line):
    if re.search(r"^> History:", text, re.M):
        return []
    found = []
    engines = ENGINE.findall(text)
    if needs_engine_line and not engines:
        found.append("no \"Engine: zebkit ... (Chromium ...)\" line (written by the script since 2026-11-11)")
    exempt = set()
    for match in ELECTRONS_CHROMIUM.finditer(text):
        if match[1] == electron:
            exempt.add(match.start(2))
    for match in CHROMIUM.finditer(text):
        if match.start(1) not in exempt and match[1] != chromium:
            found.append(f"Chromium {match[1]}")
    for match in ELECTRON.finditer(text):
        if match[1] != electron:
            found.append(f"Electron {match[1]}")
    return sorted(set(found), key=found.index)


def main():
    reports, profiles = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])
    chromium, electron = sys.argv[3], sys.argv[4]
    # What scripts/measure.sh, web-bench.sh, automation-bench.sh and library-bench.sh write.
    written = {p.stem + ".md" for p in profiles.glob("*.gn") if not p.name.startswith("_")}
    written |= {"web-bench.md", "automation-bench.md", "library-bench.md"}
    stale = 0
    for path in sorted(reports.glob("*.md")):
        found = problems(path, path.read_text(encoding="utf-8"), chromium, electron, path.name in written)
        if found:
            stale += 1
            print(f"stale: docs/reports/{path.name}: {'; '.join(found)}"
                  f" (pinned: Chromium {chromium}, Electron {electron})")
    if stale:
        print(f"{stale} report(s) measured before the pins of upstream.lock: measure them again, "
              "or mark one kept on purpose with a line \"> History: <what it records>\"")
    return 1 if stale else 0


if __name__ == "__main__":
    sys.exit(main())
