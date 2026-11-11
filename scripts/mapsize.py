#!/usr/bin/env python3
"""Attribute the engine binary's bytes to the source directories they came from.

Reads the linker map of a profile (libzebkit.so.1.map.gz in its build
directory, written because profiles/_common.gn sets generate_linker_map) and
writes docs/reports/<profile>-map.md. Every input section is charged to its
object file, so the numbers add up to the real section sizes and cover data
as well as code.

Chromium's objects are named obj/<source directory>/<target>/<file>.o, also
under ThinLTO, so the source directory is the component. Blink, third_party
and components are split a level or two deeper, where the large pieces are.

    scripts/mapsize.py <profile>
"""

import collections
import gzip
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
SRC = ROOT / "upstream" / "chromium" / "src"

# VMA, LMA, size, alignment, then text whose indentation says what the row is:
# 0 = output section, 8 = input section, 16 = symbol.
ROW = re.compile(r"^\s*[0-9a-f]+\s+[0-9a-f]+\s+([0-9a-f]+)\s+\d+ ( *)(.*)$")

# Output sections that are in the stripped file, by what they hold. The
# symbol tables and .comment are in the map and not in what ships; .bss
# takes no space in the file.
KINDS = {
    ".text": "code", ".plt": "code", ".init": "code", ".fini": "code",
    ".rodata": "rodata", ".eh_frame": "rodata", ".eh_frame_hdr": "rodata", ".gcc_except_table": "rodata",
    ".data.rel.ro": "relro", ".init_array": "relro", ".fini_array": "relro",
    ".data": "data", ".got": "data", ".got.plt": "data",
    ".rela.dyn": "relocs", ".rela.plt": "relocs",
}
KIND_ORDER = ["code", "rodata", "relro", "data", "relocs"]

# obj/<directory>/<target>/<file>.o; with ThinLTO <file> is
# <binary>.lto.<source>. The whole path but the file names the component.
OBJECT = re.compile(r"^obj/(.+)/[^/]+\.o:\(")

# Directory depth that names a component, by prefix; the longest match wins.
DEPTH = {
    "": 1,
    "third_party": 2,
    "third_party/blink/renderer": 4,
    "third_party/blink/renderer/modules": 5,
    "third_party/blink": 3,
    "components": 2,
    "services": 2,
    "content": 2,
    "v8/src": 3,
    "v8": 2,
    "ui": 2,
    "gpu": 2,
    "media": 2,
    "net": 2,
}


def component_of(directory):
    parts = directory.split("/")
    best = ""
    for prefix in DEPTH:
        if prefix and (directory == prefix or directory.startswith(prefix + "/")) and len(prefix) > len(best):
            best = prefix
    return "/".join(parts[: DEPTH[best]])


def classify(origin):
    m = OBJECT.match(origin)
    if m:
        return component_of(m.group(1))
    if origin.startswith("<internal>"):
        return "linker-generated"
    return "C runtime and other"


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__.strip().splitlines()[-1].strip())
    profile = sys.argv[1]
    # The engine library's map.
    map_path = SRC / "out" / profile / "libzebkit.so.1.map.gz"
    if not map_path.exists():
        sys.exit(f"error: {map_path} not found; build the profile with scripts/build.sh {profile}")

    by_component = collections.defaultdict(collections.Counter)
    kind = None
    with gzip.open(map_path, "rt", errors="replace") as f:
        next(f)
        for line in f:
            m = ROW.match(line)
            if not m:
                continue
            size, indent, text = int(m.group(1), 16), len(m.group(2)), m.group(3)
            if indent == 0:
                kind = KINDS.get(text.strip())
            elif indent == 8 and kind and size:
                by_component[classify(text)][kind] += size

    mib = lambda n: f"{n / 1048576:.1f}"
    total = sum(sum(c.values()) for c in by_component.values())
    per_kind = ", ".join(f"{mib(sum(c[k] for c in by_component.values()))} {k}" for k in KIND_ORDER)

    lines = [
        f"# Engine size by origin: `{profile}`",
        "",
        f"- Source: the linker map of `{map_path.name[:-len('.map.gz')]}`, so these are the bytes of its loaded sections.",
        f"- Total attributed: {mib(total)} MiB ({per_kind})",
        "- `relro` is data that needs load-time relocation, mostly vtables; `relocs` is",
        "  the relocations themselves, which the linker makes and charges to no source.",
        "",
        "| Component | Code | Read-only data | Relro | Data | Relocs | Total | Share |",
        "|---|---:|---:|---:|---:|---:|---:|---:|",
    ]
    rest = collections.Counter()
    for component, c in sorted(by_component.items(), key=lambda kv: -sum(kv[1].values())):
        size = sum(c.values())
        if size >= 512 * 1024:
            cells = " | ".join(mib(c[k]) for k in KIND_ORDER)
            lines.append(f"| `{component}` | {cells} | {mib(size)} | {100 * size / total:.1f}% |")
        else:
            rest.update(c)
    if rest:
        cells = " | ".join(mib(rest[k]) for k in KIND_ORDER)
        size = sum(rest.values())
        lines.append(f"| everything under 0.5 MiB | {cells} | {mib(size)} | {100 * size / total:.1f}% |")

    report = ROOT / "docs" / "reports" / f"{profile}-map.md"
    report.write_text("\n".join(lines) + "\n")
    print(f"wrote {report.relative_to(ROOT)}")


if __name__ == "__main__":
    main()
