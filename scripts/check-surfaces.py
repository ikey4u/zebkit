#!/usr/bin/env python3
"""Checks src/engine/surfaces.json, the registry of every place where the
engine meets the system or the person (docs/SYSTEM_INTEGRATION.md, 6.6),
against the code:

- every virtual method of the content layer's delegate classes it lists is
  classified, as reaching the system or the person (with its surface) or as
  plumbing: a Chromium update that adds one fails here until it is;
- a method the registry says zebkit implements is overridden in src/engine,
  and one it says zebkit leaves to Chromium is not;
- every surface a method names exists, its tests exist, and it is in the
  design document.

    scripts/check-surfaces.py [--chromium SRC] [--list]

SRC is Chromium's src (default: upstream/chromium/src). --list prints the
surfaces by status. Exit 0 when the registry and the code agree.
"""

import argparse
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
STATUSES = ("done", "partial", "default only", "gap")


def class_body(source, name):
    match = re.search(r"\nclass\s+(?:CONTENT_EXPORT\s+)?%s\b[^;{]*\{" % re.escape(name), source)
    if not match:
        return None
    start = index = match.end()
    depth = 1
    while depth:
        depth += {"{": 1, "}": -1}.get(source[index], 0)
        index += 1
    return source[start:index]


def virtual_methods(path, name):
    with open(path, encoding="utf-8") as handle:
        body = class_body(handle.read(), name)
    if body is None:
        return None
    body = re.sub(r"//[^\n]*", "", body)
    return {m.group(1) for m in re.finditer(r"virtual\s+[^;{]*?\b(\w+)\s*\(", body)
            if m.group(1) != name}


def engine_sources():
    text = {}
    engine = os.path.join(ROOT, "src", "engine")
    for directory, _, files in os.walk(engine):
        if "vendor" in os.path.relpath(directory, engine).split(os.sep):
            continue
        for file in files:
            if file.endswith((".h", ".cc", ".mm")):
                path = os.path.join(directory, file)
                with open(path, encoding="utf-8", errors="replace") as handle:
                    text[os.path.relpath(path, ROOT)] = handle.read()
    return text


def overridden(sources, method):
    pattern = re.compile(r"\b%s\s*\([^;{}]*?\)\s*(?:const\s*)?(?:noexcept\s*)?(?:override|final)\b" % method,
                         re.S)
    return sorted(path for path, text in sources.items() if pattern.search(text))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--chromium", default=os.path.join(ROOT, "upstream", "chromium", "src"))
    parser.add_argument("--list", action="store_true")
    args = parser.parse_args()

    with open(os.path.join(ROOT, "src", "engine", "surfaces.json"), encoding="utf-8") as handle:
        registry = json.load(handle)
    with open(os.path.join(ROOT, "docs", "SYSTEM_INTEGRATION.md"), encoding="utf-8") as handle:
        design = handle.read()

    problems = []
    surfaces = {}
    for surface in registry["surfaces"]:
        if surface["id"] in surfaces:
            problems.append("surface %s listed twice" % surface["id"])
        surfaces[surface["id"]] = surface
        if surface["status"] not in STATUSES:
            problems.append("surface %s: status %r, not one of %s" % (surface["id"], surface["status"], STATUSES))
        if "`%s`" % surface["id"] not in design:
            problems.append("surface %s is not in docs/SYSTEM_INTEGRATION.md" % surface["id"])
        for test in surface.get("tests", []):
            if not os.path.exists(os.path.join(ROOT, test)):
                problems.append("surface %s: its test %s is not there" % (surface["id"], test))

    sources = engine_sources()
    used = set()
    for name, entry in registry["classes"].items():
        header = os.path.join(args.chromium, entry["header"])
        if not os.path.exists(header):
            problems.append("%s: %s is not in %s" % (name, entry["header"], args.chromium))
            continue
        methods = virtual_methods(header, name)
        if methods is None:
            problems.append("%s: no class %s in %s" % (name, name, entry["header"]))
            continue
        system = entry.get("system", {})
        plumbing = set(entry.get("plumbing", []))
        for method in sorted(methods):
            if method not in system and method not in plumbing:
                problems.append("%s::%s is new: classify it in surfaces.json (system, with its surface, or plumbing)"
                                % (name, method))
        for method in sorted((set(system) | plumbing) - methods):
            problems.append("%s::%s is listed but no longer in %s" % (name, method, entry["header"]))
        for method in sorted(set(system) & plumbing):
            problems.append("%s::%s is both system and plumbing" % (name, method))
        for method, use in sorted(system.items()):
            if use["surface"] not in surfaces:
                problems.append("%s::%s: no surface %s" % (name, method, use["surface"]))
            used.add(use["surface"])
            where = overridden(sources, method)
            if use["zebkit"] and not where:
                problems.append("%s::%s: the registry says zebkit implements it, and src/engine does not"
                                % (name, method))
            if not use["zebkit"] and where:
                problems.append("%s::%s: the registry says Chromium's default holds, and %s overrides it"
                                % (name, method, ", ".join(where)))

    for surface in surfaces.values():
        if surface["id"] not in used and not surface.get("zebkit_only"):
            problems.append("surface %s has no content method and is not marked zebkit_only" % surface["id"])

    if args.list:
        for status in STATUSES:
            ids = [s["id"] for s in registry["surfaces"] if s["status"] == status]
            print("%s (%d): %s" % (status, len(ids), " ".join(ids)))
    for problem in problems:
        print("surfaces: " + problem, file=sys.stderr)
    if not problems:
        count = sum(len(e.get("system", {})) + len(e.get("plumbing", [])) for e in registry["classes"].values())
        print("surfaces: %d surfaces; %d content methods classified, %d reaching the system or the person"
              % (len(surfaces), count, sum(len(e.get("system", {})) for e in registry["classes"].values())))
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
