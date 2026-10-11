#!/usr/bin/env python3
"""Attribute the engine binary's bytes to the object files they came from.

Reads the linker map of a profile (out/<profile>/link.map, written when the
profile links with -Wl,-Map) and writes docs/reports/<profile>-map.md. Unlike
codesize.py, which guesses from symbol names, every input section is charged
to its object file, so the numbers add up to the real section sizes and cover
data as well as code.

Objects are grouped by component (CMake target or static library) and, inside
WebKit's own components, by source area. WebKit compiles "unified sources"
named after their directory, which is where the areas come from.

    scripts/mapsize.py <profile>
"""

import collections
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent

# VMA, LMA, size, alignment, then text whose indentation says what the row is:
# 0 = output section, 8 = input section, 16 = symbol.
ROW = re.compile(r"^\s*[0-9a-f]+\s+[0-9a-f]+\s+([0-9a-f]+)\s+\d+ ( *)(.*)$")

# Output sections that occupy space in the file. .bss and friends do not.
KINDS = {".text": "code", ".rodata": "rodata", ".data.rel.ro": "relro", ".data": "data"}
KIND_ORDER = ["code", "rodata", "relro", "data"]

TARGET = re.compile(r"/CMakeFiles/([^/]+)\.dir/(.*)$")
STATIC_LIB = re.compile(r"/(lib[^/()]+\.a)\(")
LTO_ARCHIVE = re.compile(r"\.lto\.(lib\w+)\.a\(([^)]+?)\.o at \d+\)")
UNIFIED = re.compile(r"UnifiedSource-(.+?)-\d+\.(?:cpp|mm|c)\.o$")

COMPONENTS = [
    ("WebCore", re.compile(r"^(WebCore\w*|PAL)$")),
    ("JavaScriptCore", re.compile(r"^(JavaScriptCore\w*|LowLevelInterpreterLib)$")),
    ("WebKit", re.compile(r"^(WebKit\w*|WPE\w*|xdgmime)$")),
    ("WTF + allocator", re.compile(r"^(WTF|bmalloc)$")),
    ("Skia", re.compile(r"^Skia$")),
]


def component_of(target):
    for name, pattern in COMPONENTS:
        if pattern.match(target):
            return name
    return target


def area_of_path(rest):
    """Area for a file compiled on its own: its directory, or what generated it."""
    name = re.sub(r"^.*\.lto\.", "", rest.rsplit("/", 1)[-1])
    name = re.sub(r"\.(cpp|cc|c|S|asm)\.o$", "", name)
    if "DerivedSources" in rest:
        return "generated: JS bindings" if name.startswith("JS") else "generated: " + name
    directory = re.sub(r"^(__/)+", "", rest.rsplit("/", 1)[0]) if "/" in rest else ""
    return directory or name


def classify(origin):
    """Return (component, area) for a map "In" entry such as file:(section)."""
    path = origin.rsplit(":(", 1)[0]

    m = LTO_ARCHIVE.search(path)
    if m:
        member = m.group(2)
        u = UNIFIED.search(member + ".o")
        area = u.group(1) if u else re.sub(r"\.(cpp|cc|c)$", "", member.rsplit("/", 1)[-1])
        return component_of(m.group(1)[3:]), area

    m = TARGET.search(path)
    if m:
        target, rest = m.groups()
        u = UNIFIED.search(rest)
        return component_of(target), (u.group(1) if u else area_of_path(rest))

    m = STATIC_LIB.search(path)
    if m:
        return "Third-party: " + m.group(1)[3:-2], ""
    if path.startswith("<internal>"):
        return "Linker-generated", ""
    return "C runtime and other", ""


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__.strip().splitlines()[-1].strip())
    profile = sys.argv[1]
    map_path = ROOT / "out" / profile / "link.map"
    if not map_path.exists():
        sys.exit(f"error: {map_path} not found; the profile must link with -Wl,-Map")

    by_component = collections.defaultdict(collections.Counter)
    by_area = collections.defaultdict(collections.Counter)
    kind = None
    with open(map_path, errors="replace") as f:
        next(f)
        for line in f:
            m = ROW.match(line)
            if not m:
                continue
            size, indent, text = int(m.group(1), 16), len(m.group(2)), m.group(3)
            if indent == 0:
                kind = KINDS.get(text.strip())
            elif indent == 8 and kind and size:
                component, area = classify(text)
                by_component[component][kind] += size
                if area:
                    by_area[component][area] += size

    mib = lambda n: f"{n / 1048576:.1f}"
    total = sum(sum(c.values()) for c in by_component.values())
    per_kind = ", ".join(f"{mib(sum(c[k] for c in by_component.values()))} {k}" for k in KIND_ORDER)

    lines = [
        f"# Engine size by origin: `{profile}`",
        "",
        "- Source: the linker map, so these are the bytes in the file's loaded sections.",
        f"- Total attributed: {mib(total)} MiB ({per_kind})",
        "- `relro` is data that needs load-time relocation, mostly vtables.",
        "",
        "## By component",
        "",
        "| Component | Code | Read-only data | Relro | Data | Total | Share |",
        "|---|---:|---:|---:|---:|---:|---:|",
    ]
    for component, c in sorted(by_component.items(), key=lambda kv: -sum(kv[1].values())):
        size = sum(c.values())
        if size >= 20 * 1024:
            cells = " | ".join(mib(c[k]) for k in KIND_ORDER)
            lines.append(f"| {component} | {cells} | {mib(size)} | {100 * size / total:.0f}% |")

    for component in ("WebCore", "JavaScriptCore", "WebKit", "Skia"):
        areas = by_area.get(component)
        if not areas:
            continue
        shown = [(a, s) for a, s in areas.most_common(30) if s >= 100 * 1024]
        rest = sum(areas.values()) - sum(s for _, s in shown)
        lines += ["", f"## Inside {component}", "", "| Area | MiB |", "|---|---:|"]
        lines += [f"| `{a}` | {mib(s)} |" for a, s in shown]
        lines.append(f"| everything smaller | {mib(rest)} |")

    report = ROOT / "docs" / "reports" / f"{profile}-map.md"
    report.write_text("\n".join(lines) + "\n")
    print(f"wrote {report.relative_to(ROOT)}")


if __name__ == "__main__":
    main()
