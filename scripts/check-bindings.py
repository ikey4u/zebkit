#!/usr/bin/env python3
"""Whether every binding has every function of the C API.

    scripts/check-bindings.py [--list]

For each header of src/engine/include/zebkit, the functions it exports
(ZEBKIT_API) and which bindings reach each one: Rust (bindings/rust/zebkit-rs,
which the safe crate is over), Python (bindings/python/zebkit), Go
(bindings/golang) and host JavaScript (zebkit-js: src/engine/script.cc, the
scripts it includes, the bridge its native.call() reaches, and
tools/zebkit-js). Every binding is to have all of them; they are added in the
order Rust, Python, Go, then the others. JAVASCRIPT_REACHED lists what
zebkit-js reaches otherwise than by a function's name, and how. --list names
what each lacks; each package's LICENSE is checked to be a copy of LICENSE.
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
    # zebkit-js: script.cc and its scripts, and the bridge its native.call()
    # reaches (zebkit.pdf, canvas, text and the services: bridge.cc, pdf_bridge.cc).
    "javascript": (text("src/engine/script*.cc", "src/engine/*_script.h", "src/engine/bridge.cc",
                        "src/engine/pdf_bridge.cc", "tools/zebkit-js/zebkit-js.c"), r"\b{}\b"),
}


# What zebkit-js reaches other than by calling the function by its name,
# each checked in the code: host JavaScript records, runs on its own thread,
# or is the host itself.
_CANVAS_OPS = ("save restore translate scale rotate transform set_matrix set_fill_color set_stroke_color set_alpha "
               "set_blend set_line_width set_line_cap set_line_join set_miter_limit set_dash set_linear_gradient "
               "set_radial_gradient begin_path move_to line_to quad_to curve_to arc rect close_path fill stroke clip "
               "clear draw_image font_new set_font fill_text text_path").split()
JAVASCRIPT_REACHED = {
    **{"zebkit_canvas_" + op: "zebkit.Canvas records it as an operation, which zebkit_canvas_run() runs (bridge.cc)"
       for op in _CANVAS_OPS},
    "zebkit_canvas_width": "a Canvas's width, which it is made with",
    "zebkit_canvas_height": "a Canvas's height, which it is made with",
    "zebkit_canvas_font_close": "fonts live for one run of a Canvas's operations",
    "zebkit_pdf_string": "a C hook's allocator for its answer: zebkit.pdf plugins answer with values",
    "zebkit_pdf_bytes": "a C hook's allocator for its answer: zebkit.pdf plugins answer with values",
    **{"zebkit_pdf_hook_" + hook: "zebkit.pdf plugins (pdf_script.h), on the script's own thread; "
       "tests/engine/script/pdf_plugin.mjs" for hook in ("before", "appearance", "ocr", "font", "trust", "action", "input")},
    "zebkit_icu_init": "ICU for the services before an engine runs; zebkit-js runs inside one",
    "zebkit_init": "the host's own loop: zebkit-js is the host, on zebkit_main()",
    "zebkit_pump": "the host's own loop: zebkit-js is the host, on zebkit_main()",
    "zebkit_shutdown": "the host's own loop: zebkit-js is the host, on zebkit_main()",
    "zebkit_set_wakeup": "the host's own loop: zebkit-js is the host, on zebkit_main()",
    "zebkit_init_with_settings": "its settings are the engine's switches (startup_settings.cc), on zebkit-js's command line",
    "zebkit_post_task": "setTimeout(fn, 0): the script runs on the engine's thread",
    "zebkit_post_delayed_task": "setTimeout(fn, ms) and setInterval",
    "zebkit_set_cookie_storage": "--zebkit-cookie-storage and --zebkit-cookie-key-directory (cookies.cc)",
    "zebkit_scheme_register": "--zebkit-scheme=NAME,standard,... before the engine starts; zebkit.scheme.handle()",
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
            absent = [n for n in names if not re.search(pattern.format(n), source)
                      and not (binding == "javascript" and n in JAVASCRIPT_REACHED)]
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
