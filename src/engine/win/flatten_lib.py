#!/usr/bin/env python3
"""zebkit_sandbox.lib for an application: the build's archive refers to its
objects by path, and this one holds those a link of zebkit_win_startup() loads:
the rest of //base and what it brings, 120 MiB, would only be dropped again by
the application's linker. Not the C++ side of Rust's cxx, whose Rust side is not
linked here (win/sandbox_startup.cc).

    flatten_lib.py --ar <llvm-ar> --lld-link <lld-link> --in <thin archive> --out <lib>
"""

import argparse
import os
import pathlib
import re
import subprocess
import tempfile

LEFT_OUT = ("cxx_cppdeps/cxx.obj",)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--ar", required=True)
    parser.add_argument("--lld-link", required=True)
    parser.add_argument("--in", dest="input", required=True)
    parser.add_argument("--out", required=True)
    args = parser.parse_args()
    listing = subprocess.run([args.ar, "t", args.input], check=True, capture_output=True, text=True).stdout
    # llvm-ar names a thin archive's members relative to where it runs.
    members = [os.path.normpath(m) for m in listing.split("\n") if m]
    members = [m for m in members if not m.replace(os.sep, "/").endswith(LEFT_OUT)]
    out = pathlib.Path(args.out)
    # What a link takes, with the full archive less what is left out.
    with tempfile.TemporaryDirectory(dir=str(out.parent)) as work:
        whole = pathlib.Path(work, "whole.lib")
        rsp = pathlib.Path(work, "members.rsp")
        rsp.write_text("\n".join(members))
        subprocess.run([args.ar, "rcs", "--format=coff", str(whole), f"@{rsp}"], check=True)
        link = subprocess.run([args.lld_link, "/verbose", "/include:zebkit_win_startup", "/dll", "/noentry",
                               "/force:unresolved", "/nodefaultlib", "/machine:x64",
                               f"/out:{work}/probe.dll", str(whole)], capture_output=True, text=True)
    loaded = set(re.findall(r"Loaded [^\n]*?\(([^()\n]+\.(?:obj|o))\) for", link.stderr + link.stdout))
    if not loaded:
        raise SystemExit(f"error: no member loaded:\n{link.stderr[-2000:]}")
    members = [m for m in members if os.path.basename(m) in loaded]
    out.unlink(missing_ok=True)
    rsp = out.with_suffix(".rsp")
    rsp.write_text("\n".join(members))
    subprocess.run([args.ar, "rcs", "--format=coff", str(out), f"@{rsp}"], check=True)
    rsp.unlink()


if __name__ == "__main__":
    main()
