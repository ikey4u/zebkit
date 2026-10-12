#!/usr/bin/env python3
"""Generate what exposes libraries inside the engine to applications.

    gen-vendor.py            write out/deps/vendor/ (only what changed)

The engine has whole libraries linked into it. An application that needs one
of them would otherwise ship a second copy. For each library in VENDORED
this makes it usable from the engine instead, under names of zebkit's own:

    C++   zebkit::vendor::<name>::<the library's own API>
    C     the library's own API, unchanged
    link  every function as zebkit_vendor_<name>_<function>, from libzebkit

The link names are what keeps this safe: the engine does not export
"avcodec_open2", so a process that also loads some other FFmpeg, through
another library or its own copy, has two that cannot be confused. The
header maps one set of names to the other; the application writes the
library's ordinary API.

Written to out/deps/vendor/:

    src/ZebkitVendor<Name>.cpp        one exported entry point per function,
                                      each a jump to the function inside
                                      the engine (built into libzebkit, or
                                      on macOS into WebKit.framework)
    include/zebkit/vendor/<name>.h    the header an application includes
    include/zebkit/vendor/<name>/     the library's own headers, as built

What is exposed is exactly what the build of the library in deps/ contains.
The functions are those the library's static archives define and its public
headers declare; nothing is listed by hand.
"""

import os
import pathlib
import platform
import re
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
PREFIX = ROOT / "out" / "deps" / "prefix"
OUTPUT = ROOT / "out" / "deps" / "vendor"

VENDORED = {
    "ffmpeg": {
        "title": "FFmpeg",
        "recipe": "ffmpeg",
        # Static archives whose functions are exposed, and the directory of
        # public headers that belongs to each.
        "libraries": ["avcodec", "avformat", "avutil", "swresample", "swscale"],
        # FFmpeg's shared libraries export by this pattern too.
        "symbols": r"^(av|swr|sws)[a-z0-9]*_",
        # What <zebkit/vendor/ffmpeg.h> includes. Headers for one platform's
        # hardware decoding API need that platform's own headers and are left
        # to the application to include by themselves.
        "headers": [
            "libavutil/avutil.h", "libavutil/avstring.h", "libavutil/audio_fifo.h", "libavutil/base64.h",
            "libavutil/buffer.h", "libavutil/channel_layout.h", "libavutil/cpu.h", "libavutil/dict.h",
            "libavutil/error.h", "libavutil/fifo.h", "libavutil/frame.h", "libavutil/imgutils.h",
            "libavutil/log.h", "libavutil/mathematics.h", "libavutil/mem.h", "libavutil/opt.h",
            "libavutil/parseutils.h", "libavutil/pixdesc.h", "libavutil/pixfmt.h", "libavutil/rational.h",
            "libavutil/samplefmt.h", "libavutil/time.h", "libavutil/timestamp.h",
            "libavcodec/avcodec.h", "libavcodec/bsf.h", "libavformat/avformat.h", "libavformat/avio.h",
            "libswresample/swresample.h", "libswscale/swscale.h",
        ],
    },
}


def defined_functions(archive):
    """Names of the global functions a static archive defines."""
    output = subprocess.run(["nm", "-g", "--defined-only", str(archive)], capture_output=True, text=True, check=True).stdout
    names = {fields[2] for fields in (line.split() for line in output.splitlines()) if len(fields) == 3 and fields[1] == "T"}
    # A C name has an underscore in front of it in a Mach-O file.
    if platform.system() == "Darwin":
        names = {name[1:] for name in names if name.startswith("_")}
    return names


def write_if_changed(path, text):
    """Leave an unchanged file alone, so that nothing is rebuilt for it."""
    path.parent.mkdir(parents=True, exist_ok=True)
    if path.exists() and path.read_text() == text:
        return False
    path.write_text(text)
    return True


def generate(name, spec):
    directories = [f"lib{library}" for library in spec["libraries"]]
    archives = [PREFIX / "lib" / f"lib{library}.a" for library in spec["libraries"]]
    if not all(archive.exists() for archive in archives):
        return None

    # The library's public headers, with includes of one another made
    # relative, so that they work from wherever the package is put without
    # an include path of their own.
    include_root = OUTPUT / "include" / "zebkit" / "vendor" / name
    headers = {}
    for directory in directories:
        for header in sorted((PREFIX / "include" / directory).glob("*.h")):
            text = header.read_text()
            text = re.sub(r'#(\s*)include "(lib[a-z]+)/', lambda m: f'#{m.group(1)}include "../{m.group(2)}/', text)
            headers[f"{directory}/{header.name}"] = text

    # Functions: defined in the archives, named like the library's API, and
    # declared in a public header.
    declared = "\n".join(headers.values())
    pattern = re.compile(spec["symbols"])
    functions = sorted(
        function
        for archive in archives
        for function in defined_functions(archive)
        if pattern.search(function) and re.search(rf"\b{re.escape(function)}\s*\(", declared)
    )
    functions = sorted(set(functions))
    prefix = f"zebkit_vendor_{name}_"

    # System headers the library's headers include. They must be included
    # before the namespace is opened, or their contents would land inside it.
    reached, pending = set(), list(spec["headers"])
    system = set()
    while pending:
        current = pending.pop()
        if current in reached or current not in headers:
            continue
        reached.add(current)
        directory = current.split("/")[0]
        for kind, target in re.findall(r'#\s*include\s+(["<])([^">]+)[">]', headers[current]):
            if kind == "<":
                system.add(target)
            else:
                pending.append(os.path.normpath(f"{directory}/{target}"))

    guard = f"ZEBKIT_VENDOR_{name.upper()}_H"
    lines = [
        f"/* {spec['title']} {spec['version']}, as built into zebkit. Generated by scripts/gen-vendor.py.",
        " *",
        f" * C++: the library's API is in the namespace zebkit::vendor::{name}.",
        " * C:   the library's API is as the library declares it.",
        " *",
        " * Link with libzebkit (on macOS, WebKit.framework) and nothing else. Each",
        f" * function is taken from the engine under the name {prefix}<function>,",
        " * which this header arranges; the engine does not export the library's own",
        " * names, so another copy of the library in the same process is a different",
        " * one and does no harm.",
        " * Preprocessor macros and inline functions are the library's own.",
        " *",
        " * Only what the engine was built with is there. See docs/VENDOR.md. */",
        "",
        f"#ifndef {guard}",
        f"#define {guard}",
        "",
    ]
    lines += [f"#include <{header}>" for header in sorted(system)]
    lines += [""]
    # The pragma takes the name as the object file has it, which for Mach-O
    # is the C name with an underscore in front.
    underscore = "_" if platform.system() == "Darwin" else ""
    lines += [f"#pragma redefine_extname {function} {underscore}{prefix}{function}" for function in functions]
    lines += [
        "",
        "#ifdef __cplusplus",
        f"namespace zebkit {{ namespace vendor {{ namespace {name} {{",
        'extern "C" {',
        "#endif",
        "",
    ]
    lines += [f'#include "{name}/{header}"' for header in spec["headers"]]
    lines += [
        "",
        "#ifdef __cplusplus",
        '} /* extern "C" */',
        f"}} }} }} /* zebkit::vendor::{name} */",
        "#endif",
        "",
        f"#endif /* {guard} */",
        "",
    ]
    changed = write_if_changed(include_root.parent / f"{name}.h", "\n".join(lines))
    for relative, text in headers.items():
        changed |= write_if_changed(include_root / relative, text)

    # The entry points. A jump, not a call: arguments, including variable
    # ones, pass through untouched, and the function returns to the caller.
    source = [
        f"// {spec['title']} {spec['version']} for applications. Generated by scripts/gen-vendor.py; do not edit.",
        f"// Each {prefix}<function> is an exported entry point that jumps to <function>,",
        "// which the engine links statically and does not export.",
        "",
        "#if defined(__APPLE__) && defined(__aarch64__)",
        '#define ZEBKIT_VENDOR_ENTRY(name) __asm__(".text\\n.p2align 2\\n" \\',
        f'    ".globl _{prefix}" #name "\\n" \\',
        f'    "_{prefix}" #name ":\\n\\tb _" #name "\\n");',
        "#elif defined(__APPLE__) && defined(__x86_64__)",
        '#define ZEBKIT_VENDOR_ENTRY(name) __asm__(".text\\n" \\',
        f'    ".globl _{prefix}" #name "\\n" \\',
        f'    "_{prefix}" #name ":\\n\\tjmp _" #name "\\n");',
        "#elif defined(__x86_64__)",
        '#define ZEBKIT_VENDOR_ENTRY(name) __asm__(".pushsection .text.zebkit_vendor,\\"ax\\",@progbits\\n" \\',
        f'    ".globl {prefix}" #name "\\n.type {prefix}" #name ",@function\\n" \\',
        f'    "{prefix}" #name ":\\n\\tjmp " #name "@PLT\\n" \\',
        f'    ".size {prefix}" #name ", .-{prefix}" #name "\\n.popsection\\n");',
        "#elif defined(__aarch64__)",
        '#define ZEBKIT_VENDOR_ENTRY(name) __asm__(".pushsection .text.zebkit_vendor,\\"ax\\",@progbits\\n" \\',
        f'    ".globl {prefix}" #name "\\n.type {prefix}" #name ",%function\\n" \\',
        f'    "{prefix}" #name ":\\n\\tb " #name "\\n" \\',
        f'    ".size {prefix}" #name ", .-{prefix}" #name "\\n.popsection\\n");',
        "#else",
        "#error \"zebkit's vendored libraries are not ported to this processor\"",
        "#endif",
        "",
    ]
    source += [f"ZEBKIT_VENDOR_ENTRY({function})" for function in functions]
    source += [""]
    title = "".join(part.capitalize() for part in re.split(r"[^a-z0-9]+", name))
    changed |= write_if_changed(OUTPUT / "src" / f"ZebkitVendor{title}.cpp", "\n".join(source))
    write_if_changed(OUTPUT / f"{name}.functions", "\n".join(functions) + "\n")
    return len(functions), changed


def main():
    for name, spec in VENDORED.items():
        recipe = (ROOT / "deps" / spec["recipe"] / "recipe.sh").read_text()
        spec["version"] = re.search(r"^DEP_VERSION=(\S+)", recipe, re.M).group(1)
        result = generate(name, spec)
        if result is None:
            print(f"gen-vendor: {name} is not built; run scripts/build-deps.sh", file=sys.stderr)
            return 1
        count, changed = result
        print(f"gen-vendor: {name} {spec['version']}: {count} functions{'' if changed else ' (unchanged)'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
