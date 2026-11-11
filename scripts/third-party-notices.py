#!/usr/bin/env python3
"""The third-party notices of what zebkit ships, as plain text.

    scripts/third-party-notices.py engine <gn-out-dir> <target-os> <output>
    scripts/third-party-notices.py crates <Cargo.toml> <rust-target> <program> <output>

engine: Chromium's own tool (tools/licenses/licenses.py, what Chrome's
about:credits comes from) over the engine's GN dependencies, so that only what
is linked is listed, each with its license text. Run from Chromium's src.

crates: the Rust crates compiled into a program (zebkit-mcp), from cargo
metadata for the target: normal dependencies only, as build scripts and
tests are not shipped. Crates of identical license text are listed together
with that text once. zebkit's own crates are under LICENSE.
"""

import json
import os
import pathlib
import subprocess
import sys

RULE = "-" * 20
OURS = {"zebkit", "zebkit-rs", "zebkit-build", "zebkit-mcp"}
NAMES = ("license", "licence", "copying", "notice", "unlicense")


def engine(out_dir, target_os, output):
    sys.path.insert(0, os.path.abspath("tools/licenses"))
    import licenses
    # The macOS build is generated from //:zebkit alone (scripts/build.sh): the
    # tool's own "gn desc" of the profile's arguments has to be as well.
    if target_os == "mac":
        gn = licenses._GnBinary
        licenses._GnBinary = lambda: gn() + ["--root-pattern=//:zebkit"]
    sys.argv = ["licenses.py", "license_file", "--gn-out-dir", out_dir, "--gn-target", "//zebkit/engine:engine",
                "--target-os", target_os, "--format", "txt", output]
    licenses.main()


def crates(manifest, target, program, output):
    meta = json.loads(subprocess.run(
        ["cargo", "metadata", "--format-version", "1", "--locked", "--filter-platform", target,
         "--manifest-path", manifest], check=True, capture_output=True, text=True).stdout)
    packages = {p["id"]: p for p in meta["packages"]}
    nodes = {n["id"]: n for n in meta["resolve"]["nodes"]}
    root = meta["resolve"]["root"]
    seen, stack = set(), [root]
    while stack:
        node = nodes[stack.pop()]
        if node["id"] in seen:
            continue
        seen.add(node["id"])
        for dep in node["deps"]:
            if any(kind["kind"] is None for kind in dep["dep_kinds"]):
                stack.append(dep["pkg"])
    groups = {}
    for pid in sorted(seen, key=lambda i: (packages[i]["name"], packages[i]["version"])):
        package = packages[pid]
        if package["name"] in OURS:
            continue
        directory = pathlib.Path(package["manifest_path"]).parent
        texts = []
        if package.get("license_file"):
            texts.append((directory / package["license_file"]).read_text(errors="replace"))
        for file in sorted(directory.iterdir()):
            if file.is_file() and file.name.lower().startswith(NAMES) and not file.suffix.lower() in (".rs", ".toml"):
                text = file.read_text(errors="replace")
                if text not in texts:
                    texts.append(text)
        expression = package.get("license") or "see the text below"
        body = "\n\n".join(t.strip() for t in texts) or "(The crate ships no license file; its license is " + expression + ".)"
        groups.setdefault(body, []).append("%s %s (%s)" % (package["name"], package["version"], expression))
    with open(output, "w") as out:
        out.write("The Rust crates compiled into %s.\n\n" % program)
        for body, names in groups.items():
            out.write("%s\n%s\n%s\n%s\n\n" % (RULE, "\n".join(names), RULE, body))
    print("%d crates in %d license texts" % (sum(len(n) for n in groups.values()), len(groups)))


if __name__ == "__main__":
    if len(sys.argv) == 5 and sys.argv[1] == "engine":
        engine(*sys.argv[2:])
    elif len(sys.argv) == 6 and sys.argv[1] == "crates":
        crates(*sys.argv[2:])
    else:
        sys.exit(__doc__)
