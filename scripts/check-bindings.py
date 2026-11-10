#!/usr/bin/env python3
"""Whether every binding has every function of the C API.

    scripts/check-bindings.py [--list]

For each header of src/engine/include/zebkit, the functions it exports
(ZEBKIT_API) and which bindings reach each one: Rust (bindings/rust/zebkit-rs,
which the safe crate is over), Python (bindings/python/zebkit), Go
(bindings/golang) and host JavaScript (zebkit-js: src/engine/script.cc and the
scripts it includes). Every binding is to have all of them; they are added in
the order Rust, Python, Go, then the others. --list names what each lacks.
Exit 0 when nothing is missing.
"""

import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
HEADERS = ROOT / "src/engine/include/zebkit"
# Declared for one system's program and not a binding's to call.
EXEMPT = {"zebkit_win_set_sandbox", "zebkit_helper_main"}


def text(*patterns):
    return "\n".join(p.read_text(errors="replace") for pattern in patterns for p in sorted(ROOT.glob(pattern)))


BINDINGS = {
    "rust": (text("bindings/rust/zebkit-rs/src/*.rs"), r"fn {}\b"),
    "python": (text("bindings/python/zebkit/*.py"), r"\b{}\b"),
    "go": (text("bindings/golang/*.go", "bindings/golang/*.c"), r"\b{}\b"),
    "javascript": (text("src/engine/script*.cc", "src/engine/*_script.h", "src/engine/script/*.cc"), r"\b{}\b"),
}


def exported(header):
    names = []
    for match in re.finditer(r"ZEBKIT_API\s+[^;(]*?\b(zebkit_\w+)\s*\(", header.read_text()):
        if match.group(1) not in EXEMPT:
            names.append(match.group(1))
    return names


def main():
    listing = "--list" in sys.argv
    missing_total = 0
    print("%-16s %5s %s" % ("header", "C", " ".join("%10s" % b for b in BINDINGS)))
    gaps = {b: [] for b in BINDINGS}
    for header in sorted(HEADERS.glob("*.h")):
        names = exported(header)
        if not names:
            continue
        row = []
        for binding, (source, pattern) in BINDINGS.items():
            absent = [n for n in names if not re.search(pattern.format(n), source)]
            gaps[binding] += [(header.name, n) for n in absent]
            row.append("%10s" % ("%d/%d" % (len(names) - len(absent), len(names))))
        print("%-16s %5d %s" % (header.name, len(names), " ".join(row)))
    for binding, absent in gaps.items():
        missing_total += len(absent)
        print("%s: %d missing" % (binding, len(absent)))
        if listing:
            for header, name in absent:
                print("    %s %s" % (header, name))
    # Each package carries the license as a file of its own, the same as LICENSE
    # (a symbolic link is a text file in a Windows checkout).
    license = (ROOT / "LICENSE").read_bytes()
    copies = ["bindings/python", "bindings/golang", "bindings/rust/zebkit", "bindings/rust/zebkit-rs",
              "bindings/rust/zebkit-build", "tools/zebkit-mcp"]
    stale = [c for c in copies if (ROOT / c / "LICENSE").is_symlink() or not (ROOT / c / "LICENSE").is_file()
             or (ROOT / c / "LICENSE").read_bytes() != license]
    for copy in stale:
        print("%s/LICENSE is not a copy of LICENSE" % copy)
    return 1 if missing_total or stale else 0


if __name__ == "__main__":
    sys.exit(main())
