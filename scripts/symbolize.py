#!/usr/bin/env python3
"""Turn a tools/crashsite log into function names.

    scripts/symbolize.py <profile> <crash.log>

crashsite writes "module+offset" for the crash location and for every stack
word that points into code. Offsets inside the engine are looked up in the
unstripped library under out/<profile>/lib (the shipped one has no symbols,
but the same layout). Stack entries are candidates for return addresses, so
read the output as "functions that were probably on the stack", innermost
first; repeated neighbours are collapsed.
"""

import bisect
import pathlib
import re
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent


def load_symbols(library):
    out = subprocess.run(["nm", "-C", "--defined-only", str(library)], check=True, capture_output=True, text=True).stdout
    table = []
    for line in out.splitlines():
        parts = line.split(" ", 2)
        if len(parts) == 3 and parts[1] in "tTwW":
            table.append((int(parts[0], 16), parts[2]))
    table.sort()
    return [a for a, _ in table], [n for _, n in table]


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__.strip().splitlines()[2].strip())
    profile, log = sys.argv[1], pathlib.Path(sys.argv[2])
    libraries = sorted((ROOT / "out" / profile / "lib").glob("libWPEWebKit-*.so.*.*.*"))
    if not libraries:
        sys.exit(f"error: profile '{profile}' is not built")
    addresses, names = load_symbols(libraries[0])
    if not addresses:
        sys.exit(f"error: {libraries[0]} has no symbol table")

    previous = None
    for line in log.read_text().splitlines():
        m = re.match(r"^(pc|stack) (\S+)\+0x([0-9a-f]+)$", line)
        if not m:
            if line.startswith("crash"):
                print(line)
                previous = None
            continue
        kind, module, offset = m.group(1), m.group(2), int(m.group(3), 16)
        if module.startswith("libWPEWebKit"):
            i = bisect.bisect_right(addresses, offset) - 1
            where = f"{names[i]} + {offset - addresses[i]:#x}" if i >= 0 else f"{module}+{offset:#x}"
        else:
            where = f"{module}+{offset:#x}"
        key = where.split(" + ")[0]
        if kind == "stack" and key == previous:
            continue
        previous = key
        print(f"  {kind:5} {where[:150]}")


if __name__ == "__main__":
    main()
