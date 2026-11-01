#!/usr/bin/env python3
"""Expose the C libraries inside the engine to applications.

    gen_vendor.py [--target linux|mac|win] --src <chromium src> --clang <clang> --sysroot <dir>
                  --nm <llvm-nm> --out <gen dir> --stamp <file> <library>...

Run by //zebkit/engine:vendor from the build directory, once the libraries
are compiled. For each library named, it writes into <gen dir>:

    vendor/<library>.S          one exported entry point per function,
                                zebkit_vendor_<library>_<function>, each a
                                jump to the function inside the engine
    vendor/<library>.c          on Windows in place of the .S: each entry point
                                is the linker's export of the function under
                                that name, so no code
    vendor/<library>.txt        the functions, one per line
    include/zebkit/vendor/<library>.h    the header an application includes
    include/zebkit/vendor/<library>/     the library's public headers

The application writes the library's own API. The header maps each
function's name to zebkit's link name, so the engine does not export the
library's names, and another copy of the library in the same process
cannot be confused with it (docs/API.md). In C++ the API is also in the
namespace zebkit::vendor::<library>.

Nothing is listed by hand: a function is exposed when the library's public
headers declare it and the library's compiled objects define it, which is
exactly what the engine was built with. Chromium renames some libraries'
functions (zlib's Cr_z_, SQLite's chrome_sqlite3_, ICU's version suffix,
libjpeg-turbo's chromium_), by macros in their headers; those macros are
followed, and the entry point is named after the library's own name.
"""

import argparse
import json
import os
import pathlib
import re
import subprocess
import sys

# Each library: where its public headers are in Chromium's tree, which of
# them are shipped and which the one header includes, the macros the
# engine compiles its users with, and where its objects are in the build.
LIBRARIES = {
    "zlib": {
        "title": "zlib",
        "version": ("zlib.h", r'#define ZLIB_VERSION "([^"]+)"'),
        "roots": {"": "third_party/zlib"},
        "ship": ["zlib.h", "zconf.h", "chromeconf.h"],
        "include": ["zlib.h"],
        "objects": ["third_party/zlib"],
    },
    "zstd": {
        "title": "Zstandard",
        "version": ("zstd.h", r"#define ZSTD_VERSION_MAJOR\s+(\d+)\s*\n#define ZSTD_VERSION_MINOR\s+(\d+)\s*\n#define ZSTD_VERSION_RELEASE\s+(\d+)"),
        "roots": {"": "third_party/zstd/src/lib"},
        "ship": ["zstd.h", "zstd_errors.h"],
        "include": ["zstd.h", "zstd_errors.h"],
        "objects": ["third_party/zstd"],
    },
    "brotli": {
        "title": "Brotli",
        "version": ("brotli/decode.h", None),
        "roots": {"": "third_party/brotli/include"},
        "ship": ["brotli/*.h"],
        "include": ["brotli/decode.h", "brotli/encode.h"],
        "objects": ["third_party/brotli"],
    },
    "boringssl": {
        "title": "BoringSSL",
        "version": None,
        "roots": {"": "third_party/boringssl/src/include"},
        "ship": ["openssl/*.h"],
        # Its C++ helpers (bssl::UniquePtr) are left out: they are templates
        # over Chromium's own C++ library, not the application's.
        "defines": {"BORINGSSL_NO_CXX": "1"},
        "include": [
            "openssl/aead.h", "openssl/aes.h", "openssl/base64.h", "openssl/bio.h", "openssl/bn.h",
            "openssl/bytestring.h", "openssl/chacha.h", "openssl/cipher.h", "openssl/cmac.h",
            "openssl/crypto.h", "openssl/curve25519.h", "openssl/digest.h", "openssl/ec.h",
            "openssl/ec_key.h", "openssl/ecdh.h", "openssl/ecdsa.h", "openssl/err.h", "openssl/evp.h",
            "openssl/hkdf.h", "openssl/hmac.h", "openssl/hpke.h", "openssl/md5.h", "openssl/mem.h",
            "openssl/mldsa.h", "openssl/mlkem.h", "openssl/nid.h", "openssl/obj.h", "openssl/pem.h",
            "openssl/pkcs8.h", "openssl/poly1305.h", "openssl/pool.h", "openssl/rand.h", "openssl/rsa.h",
            "openssl/sha.h", "openssl/ssl.h", "openssl/x509.h", "openssl/x509v3.h",
        ],
        "objects": ["third_party/boringssl"],
    },
    "sqlite": {
        "title": "SQLite",
        "version": ("src/amalgamation/sqlite3.h", r'#define SQLITE_VERSION\s+"([^"]+)"'),
        "roots": {"": "third_party/sqlite"},
        "ship": ["sqlite3.h", "src/amalgamation/sqlite3.h", "src/amalgamation/rename_exports.h"],
        # As Chromium builds it (//third_party/sqlite:chromium_sqlite3_compile_options).
        # Among them: exclusive locking by default, no JSON functions, and no
        # automatic initialization: sqlite3_initialize() comes first.
        "defines": {
            "SQLITE_DQS": "0",
            "SQLITE_OMIT_ANALYZE": "1",
            "SQLITE_OMIT_AUTOINIT": "1",
            "SQLITE_OMIT_AUTOMATIC_INDEX": "1",
            "SQLITE_OMIT_COMPILEOPTION_DIAGS": "1",
            "SQLITE_OMIT_EXPLAIN": "1",
            "SQLITE_OMIT_GET_TABLE": "1",
            "SQLITE_OMIT_INTROSPECTION_PRAGMAS": "1",
            "SQLITE_DEFAULT_LOOKASIDE": "0,0",
            "SQLITE_OMIT_LOOKASIDE": "1",
            "SQLITE_OMIT_TCL_VARIABLE": "1",
            "SQLITE_OMIT_REINDEX": "1",
            "SQLITE_OMIT_UPSERT": "1",
            "SQLITE_OMIT_WINDOWFUNC": "1",
            "SQLITE_OMIT_TRACE": "1",
            "SQLITE_DISABLE_FTS4_DEFERRED": "1",
            "SQLITE_ENABLE_ICU": "1",
            "SQLITE_SECURE_DELETE": "1",
            "SQLITE_THREADSAFE": "1",
            "SQLITE_MAX_WORKER_THREADS": "0",
            "SQLITE_MAX_MMAP_SIZE": "268435456",
            "SQLITE_DEFAULT_FILE_PERMISSIONS": "0600",
            "SQLITE_DEFAULT_LOCKING_MODE": "1",
            "SQLITE_DEFAULT_MEMSTATUS": "1",
            "SQLITE_DEFAULT_PAGE_SIZE": "4096",
            "SQLITE_DEFAULT_PCACHE_INITSZ": "0",
            "SQLITE_LIKE_DOESNT_MATCH_BLOBS": "1",
            "SQLITE_OMIT_DEPRECATED": "1",
            "SQLITE_OMIT_PROGRESS_CALLBACK": "1",
            "SQLITE_OMIT_SHARED_CACHE": "1",
            "SQLITE_USE_ALLOCA": "1",
            "SQLITE_OMIT_DECLTYPE": "1",
            "SQLITE_OMIT_JSON": "1",
            "SQLITE_OMIT_LOAD_EXTENSION": "1",
            "SQLITE_HAVE_ISNAN": "1",
            "SQLITE_HAVE_SQLITE3R": "1",
            "SQLITE_ENABLE_DBPAGE_VTAB": "1",
            "SQLITE_ENABLE_BATCH_ATOMIC_WRITE": "1",
            "SQLITE_TEMP_STORE": "3",
            "SQLITE_ENABLE_LOCKING_STYLE": "0",
        },
        "include": ["sqlite3.h"],
        # Not dev_sqlite3, Chromium's build for its developers, which has
        # what the engine's leaves out.
        "objects": ["third_party/sqlite/chromium_sqlite3"],
    },
    "icu": {
        "title": "ICU",
        "version": ("unicode/uvernum.h", r'#define U_ICU_VERSION "([^"]+)"'),
        "roots": {"": "third_party/icu/source/common", "i18n": "third_party/icu/source/i18n"},
        "ship": ["unicode/*.h", "i18n:unicode/*.h"],
        # As Chromium builds ICU (//third_party/icu:icu_config). The C++
        # API is left out: it is classes over Chromium's own C++ library.
        "defines": {
            "U_USING_ICU_NAMESPACE": "0",
            "U_ENABLE_DYLOAD": "0",
            "USE_CHROMIUM_ICU": "1",
            "U_ENABLE_TRACING": "1",
            "U_ENABLE_RESOURCE_TRACING": "0",
            "U_STATIC_IMPLEMENTATION": "1",
            "UCONFIG_ONLY_HTML_CONVERSION": "1",
            "U_CHARSET_IS_UTF8": "1",
            "U_SHOW_CPLUSPLUS_API": "0",
            "U_SHOW_CPLUSPLUS_HEADER_API": "0",
        },
        "include": [
            "unicode/ubrk.h", "unicode/ucal.h", "unicode/ucasemap.h", "unicode/uchar.h",
            "unicode/ucnv.h", "unicode/ucol.h", "unicode/ucsdet.h", "unicode/udat.h",
            "unicode/udatpg.h", "unicode/uidna.h", "unicode/uldnames.h", "unicode/uloc.h",
            "unicode/ulocdata.h", "unicode/unorm2.h", "unicode/unum.h", "unicode/uregex.h",
            "unicode/uscript.h", "unicode/usearch.h", "unicode/uset.h", "unicode/ushape.h",
            "unicode/uspoof.h", "unicode/ustring.h", "unicode/utext.h", "unicode/utrans.h",
            "unicode/udata.h", "unicode/uclean.h", "unicode/utypes.h",
        ],
        "objects": ["third_party/icu"],
    },
    "leveldb": {
        "title": "LevelDB",
        "version": None,
        "roots": {"": "third_party/leveldatabase/src/include"},
        "ship": ["leveldb/c.h", "leveldb/export.h"],
        "include": ["leveldb/c.h"],
        "objects": ["third_party/leveldatabase"],
    },
    "freetype": {
        "title": "FreeType",
        "version": ("freetype/freetype.h", r"#define\s+FREETYPE_MAJOR\s+(\d+)[\s\S]*?#define\s+FREETYPE_MINOR\s+(\d+)[\s\S]*?#define\s+FREETYPE_PATCH\s+(\d+)"),
        "roots": {"": "third_party/freetype/src/include"},
        "ship": ["ft2build.h", "freetype/*.h", "freetype/config/*.h"],
        "override_headers": {
            "freetype/config/ftoption.h": "third_party/freetype/include/freetype-custom/freetype/config/ftoption.h",
            "freetype/config/ftmodule.h": "third_party/freetype/include/freetype-custom/freetype/config/ftmodule.h",
        },
        "macro_include_prefix": "zebkit/vendor/freetype/",
        # Legacy Carbon/FSSpec APIs are not part of the engine's FT build.
        "skip_ship": ["freetype/ftmac.h"],
        "include": [
            "freetype/freetype.h", "freetype/ftadvanc.h", "freetype/ftbbox.h", "freetype/ftbdf.h",
            "freetype/ftbitmap.h", "freetype/ftbzip2.h", "freetype/ftcache.h", "freetype/ftcid.h",
            "freetype/ftcolor.h", "freetype/ftdriver.h", "freetype/ftfntfmt.h", "freetype/ftgasp.h",
            "freetype/ftglyph.h", "freetype/ftgxval.h", "freetype/ftgzip.h", "freetype/ftincrem.h",
            "freetype/ftlcdfil.h", "freetype/ftlist.h", "freetype/ftlogging.h", "freetype/ftlzw.h",
            "freetype/ftmm.h", "freetype/ftmodapi.h", "freetype/ftotval.h",
            "freetype/ftoutln.h", "freetype/ftparams.h", "freetype/ftpfr.h", "freetype/ftrender.h",
            "freetype/ftsizes.h", "freetype/ftsnames.h", "freetype/ftstroke.h", "freetype/ftsynth.h",
            "freetype/ftsystem.h", "freetype/fttrigon.h", "freetype/ftwinfnt.h", "freetype/otsvg.h",
            "freetype/t1tables.h", "freetype/ttnameid.h", "freetype/tttables.h", "freetype/tttags.h",
        ],
        "objects": ["third_party/freetype"],
    },
    "harfbuzz": {
        "title": "HarfBuzz",
        "version": ("hb-version.h", r'#define HB_VERSION_STRING "([^"]+)"'),
        "roots": {"": "third_party/harfbuzz/src/src"},
        "ship": ["hb.h", "hb-*.h"],
        "skip_ship": ["hb-ft.h", "hb-glib.h", "hb-gobject*.h", "hb-graphite2.h", "hb-icu.h",
                      "hb-coretext.h", "hb-directwrite.h", "hb-gdi.h", "hb-uniscribe.h",
                      "hb-cairo.h", "hb-wasm-api.h", "hb-cplusplus.hh", "hb-features.h"],
        "include": ["hb.h", "hb-ot.h", "hb-subset.h"],
        "objects": ["third_party/harfbuzz"],
    },
    "libwebp": {
        "title": "libwebp",
        "version": None,
        "roots": {"": "third_party/libwebp/src/src"},
        "ship": ["webp/*.h"],
        "include": ["webp/decode.h", "webp/encode.h", "webp/demux.h", "webp/mux.h"],
        "objects": ["third_party/libwebp"],
    },
    "opus": {
        "title": "Opus",
        "version": None,
        "roots": {"": "third_party/opus/src/include"},
        "ship": ["*.h"],
        "include": ["opus.h", "opus_multistream.h"],
        "objects": ["third_party/opus"],
    },
    "dav1d": {
        "title": "dav1d",
        "version": None,
        "roots": {"": "third_party/dav1d/libdav1d/include"},
        "ship": ["dav1d/*.h"],
        "include": ["dav1d/dav1d.h"],
        "objects": ["third_party/dav1d"],
    },
    "libvpx": {
        "title": "libvpx",
        "version": None,
        "roots": {"": "third_party/libvpx/source/libvpx"},
        "ship": ["vpx/vpx_codec.h", "vpx/vpx_decoder.h", "vpx/vpx_encoder.h", "vpx/vp8.h",
                 "vpx/vp8cx.h", "vpx/vp8dx.h", "vpx/vpx_image.h", "vpx/vpx_integer.h",
                 "vpx/vpx_ext_ratectrl.h", "vpx/vpx_tpl.h", "vpx/vpx_frame_buffer.h"],
        "include": ["vpx/vpx_decoder.h", "vpx/vpx_encoder.h", "vpx/vp8cx.h", "vpx/vp8dx.h"],
        "objects": ["third_party/libvpx"],
    },
    "libjpeg": {
        "title": "libjpeg-turbo",
        "version": None,
        "roots": {"": "third_party/libjpeg_turbo/src"},
        "ship": ["jpeglib.h", "jconfig.h", "jmorecfg.h", "jerror.h", "jpeglibmangler.h"],
        "defines": {"MANGLE_JPEG_NAMES": "1"},
        # jpeglib.h expects FILE and size_t to be declared before it.
        "prelude": ["stddef.h", "stdio.h"],
        "include": ["jpeglib.h", "jerror.h"],
        "objects": ["third_party/libjpeg_turbo"],
    },
    "libxml": {
        "title": "libxml2",
        "version": ("libxml/xmlversion.h", r'#define LIBXML_DOTTED_VERSION "([^"]+)"'),
        "roots": {"": "third_party/libxml/src/include", "linux": "third_party/libxml/linux/include"},
        "ship": ["libxml/*.h", "linux:libxml/xmlversion.h"],
        "defines": {"LIBXML_STATIC": "1"},
        "include": [
            "libxml/parser.h", "libxml/HTMLparser.h", "libxml/tree.h", "libxml/xpath.h",
            "libxml/xpathInternals.h", "libxml/xmlreader.h", "libxml/xmlwriter.h",
            "libxml/xmlsave.h", "libxml/uri.h", "libxml/xmlerror.h", "libxml/entities.h",
            "libxml/xinclude.h",
        ],
        "objects": ["third_party/libxml"],
    },
    "pdfium": {
        "title": "PDFium",
        "version": None,
        "roots": {"": "third_party/pdfium/public"},
        "ship": ["*.h"],
        # Built without XFA forms and without PDFium's own JavaScript
        # (profiles/_common.gn): the headers' XFA parts stay out.
        "include": [
            "fpdfview.h", "fpdf_annot.h", "fpdf_attachment.h", "fpdf_catalog.h", "fpdf_dataavail.h",
            "fpdf_doc.h", "fpdf_edit.h", "fpdf_ext.h", "fpdf_flatten.h", "fpdf_formfill.h",
            "fpdf_javascript.h", "fpdf_ppo.h", "fpdf_progressive.h", "fpdf_save.h", "fpdf_searchex.h",
            "fpdf_signature.h", "fpdf_structtree.h", "fpdf_sysfontinfo.h", "fpdf_text.h",
            "fpdf_thumbnail.h", "fpdf_transformpage.h",
        ],
        "objects": ["third_party/pdfium"],
    },
    "ffmpeg": {
        "title": "FFmpeg",
        # libavutil/common.h: #ifndef av_log2.
        "declare_if_macro": ["int av_log2(unsigned v);"],
        "version": ("libavutil/ffversion.h", r'#define FFMPEG_VERSION "([^"]+)"'),
        # zebkit's configuration of FFmpeg (src/ffmpeg, scripts/ffmpeg-config.sh),
        # for the system the engine is built for: "{config}" is filled in.
        "roots": {"": "third_party/ffmpeg", "config": "zebkit/ffmpeg/config/{config}"},
        "ship": ["libavutil/*.h", "libavcodec/*.h", "libavformat/*.h", "libswscale/*.h",
                 "libswresample/*.h", "config:libavutil/avconfig.h", "config:libavutil/ffversion.h"],
        "skip_ship": ["libavutil/hwcontext_*.h", "libavcodec/d3d*.h", "libavcodec/dxva2.h",
                      "libavcodec/qsv.h", "libavcodec/vdpau.h", "libavcodec/videotoolbox.h",
                      "libavcodec/mediacodec.h", "libavcodec/jni.h", "libavcodec/vaapi.h"],
        "include": [
            "libavutil/avutil.h", "libavutil/avstring.h", "libavutil/channel_layout.h",
            "libavutil/dict.h", "libavutil/error.h", "libavutil/frame.h", "libavutil/imgutils.h",
            "libavutil/log.h", "libavutil/mathematics.h", "libavutil/mem.h", "libavutil/opt.h",
            "libavutil/pixdesc.h", "libavutil/samplefmt.h", "libavutil/display.h",
            "libavutil/bprint.h", "libavcodec/avcodec.h", "libavformat/avformat.h",
            "libavformat/avio.h", "libswscale/swscale.h", "libswresample/swresample.h",
        ],
        "objects": ["third_party/ffmpeg"],
    },
}

INCLUDE = re.compile(r'^(\s*#\s*include\s*)([<"])([^">]+)([">])', re.M)


def version_of(spec, files):
    if not spec["version"]:
        return ""
    name, pattern = spec["version"]
    if pattern is None or name not in files:
        return ""
    m = re.search(pattern, files[name])
    return ".".join(m.groups()) if m else ""


def gather(spec, src):
    """The shipped headers: {path in the package: text}."""
    files = {}
    for pattern in spec["ship"]:
        root_key, _, glob = pattern.rpartition(":")
        root = src / spec["roots"][root_key].format(config=FFMPEG_CONFIGS[TARGET])
        matches = sorted(root.glob(glob))
        if not matches:
            raise SystemExit(f"error: no {glob} under {root}")
        for path in matches:
            relative = path.relative_to(root).as_posix()
            if any(pathlib.PurePosixPath(relative).match(skip) for skip in spec.get("skip_ship", [])):
                continue
            files[relative] = path.read_text(errors="replace")
    for relative, source in spec.get("override_headers", {}).items():
        files[relative] = (src / source).read_text()
    return files


def relink(spec, files):
    """Make includes between shipped headers relative, so that they work
    with include/ as the only include path. Returns the system headers
    they include."""
    system = set()
    for name, text in list(files.items()):
        here = posix_dir(name)

        def replace(m):
            target = m.group(3)
            # <...> is not looked for beside the including file.
            candidates = [target]
            if m.group(2) == '"':
                candidates.insert(0, os.path.normpath(os.path.join(here, target)))
            # Chromium's own headers include by path from the source root.
            for root in spec["roots"].values():
                if target.startswith(root + "/"):
                    inside = target[len(root) + 1:]
                    candidates.append(inside)
            for candidate in candidates:
                if candidate in files:
                    relative = os.path.relpath(candidate, here or ".")
                    return f'{m.group(1)}"{relative}"'
            if m.group(2) == "<":
                system.add(target)
            return m.group(0)

        prefix = spec.get("macro_include_prefix")
        if prefix:
            # FreeType's FT_*_H header names are expanded at the caller's
            # include site, so relative paths would depend on that caller.
            text = re.sub(r'(^\s*#\s*define\s+\w+\s+)<([^>]+)>',
                          lambda m: m.group(1) + "<" + prefix + m.group(2) + ">"
                          if m.group(2) in files else m.group(0), text, flags=re.M)
        files[name] = INCLUDE.sub(replace, text)
    return system


def posix_dir(name):
    return name.rpartition("/")[0]


def defined_functions(nm, objects_dirs, underscore):
    names = set()
    pattern = "*.obj" if TARGET == "win" else "*.o"
    objects = [str(p) for d in objects_dirs for p in pathlib.Path("obj", d).rglob(pattern)]
    for start in range(0, len(objects), 500):
        out = subprocess.run([nm, "--defined-only", "-g"] + objects[start:start + 500],
                             capture_output=True, text=True).stdout
        for line in out.splitlines():
            fields = line.split()
            if len(fields) == 3 and fields[1] == "T":
                # Mach-O puts an underscore before every C name.
                names.add(fields[2][1:] if underscore else fields[2])
    return names


TRIPLES = {"linux": "x86_64-unknown-linux-gnu", "mac": "arm64-apple-macos13",
           "win": "x86_64-pc-windows-msvc"}
# The configuration directories of zebkit's FFmpeg, by system.
FFMPEG_CONFIGS = {"linux": "linux/x64", "mac": "mac/arm64", "win": "win/x64"}
TARGET = "linux"


def sysroot_args(sysroot):
    """How the compiler is told where the system's headers are."""
    if TARGET == "win":
        return ["-Xmicrosoft-windows-sys-root", sysroot]
    return ["-isysroot" if TARGET == "mac" else "--sysroot", sysroot]


def preprocess(clang, sysroot, include_dir, defines, text, extra=()):
    args = [clang, "-x", "c", "-std=c11", "-E", "--target=" + TRIPLES[TARGET],
            *sysroot_args(sysroot), "-I", str(include_dir),
            "-I", str(include_dir.parents[2])]
    args += [f"-D{k}={v}" for k, v in defines.items()]
    args += list(extra) + ["-"]
    result = subprocess.run(args, input=text, capture_output=True, text=True)
    if result.returncode:
        raise SystemExit(f"error: preprocessing failed:\n{result.stderr[-3000:]}")
    return result.stdout


def external_declarations(clang, sysroot, text):
    """C functions with external linkage, after preprocessing the headers.

    Mach-O may emit C++ inline helpers as global text symbols even when
    the C header declares them static inline. They belong to the caller;
    redefine_extname cannot be applied to their internal C declarations.
    """
    args = [clang, "-x", "c", "-std=c11", "--target=" + TRIPLES[TARGET],
            *sysroot_args(sysroot),
            "-fsyntax-only", "-Xclang", "-ast-dump=json", "-"]
    result = subprocess.run(args, input=text, capture_output=True, text=True)
    if result.returncode:
        raise SystemExit(f"error: parsing public headers failed:\n{result.stderr[-3000:]}")
    pending = [json.loads(result.stdout)]
    names = set()
    while pending:
        node = pending.pop()
        if node.get("kind") == "FunctionDecl" and node.get("storageClass") != "static":
            names.add(node["name"])
        pending.extend(node.get("inner", []))
    return names


def generate(name, spec, args):
    src = pathlib.Path(args.src)
    out = pathlib.Path(args.out)
    files = gather(spec, src)
    version = version_of(spec, files)
    system = relink(spec, files)
    include_root = out / "include" / "zebkit" / "vendor" / name
    for relative, text in files.items():
        write_if_changed(include_root / relative, text)

    defines = spec.get("defines", {})
    prelude = spec.get("prelude", [])
    umbrella_includes = "".join(f"#include <{h}>\n" for h in prelude)
    umbrella_includes += "".join(f'#include "{h}"\n' for h in spec["include"])

    # What the headers declare, after the library's own renaming macros.
    declared = external_declarations(
        args.clang, args.sysroot,
        preprocess(args.clang, args.sysroot, include_root, defines, umbrella_includes))
    macros = preprocess(args.clang, args.sysroot, include_root, defines, umbrella_includes, ["-dM"])
    names = sorted({m.group(1) for m in re.finditer(r"^#define (\w+) ", macros, re.M)})
    # Each object-like macro's full expansion, to map a renamed function
    # back to the name the library documents.
    probe = umbrella_includes + "".join(f"zebkit_probe_{i} {n} zebkit_end\n" for i, n in enumerate(names))
    expanded = preprocess(args.clang, args.sysroot, include_root, defines, probe)
    renamed = {}
    for m in re.finditer(r"^zebkit_probe_(\d+) (\w+) zebkit_end$", expanded, re.M):
        original, expansion = names[int(m.group(1))], m.group(2)
        if expansion != original:
            renamed.setdefault(expansion, original)

    # The system headers the library's headers include, of those its
    # #if branches name, are the ones the preprocessor really opens here.
    opened = preprocess(args.clang, args.sysroot, include_root, defines, umbrella_includes, ["-M", "-MT", "x"])
    opened = {p for p in re.split(r"[\s\\]+", opened) if p and p != "x:"}
    # Not the library's own headers: ICU's unicode/uchar.h is not the C
    # library's <uchar.h>, which macOS does not have.
    root = str(include_root.resolve())
    outside = {p for p in opened if not os.path.realpath(p).startswith(root + os.sep)}
    system = {h for h in system if any(p.endswith("/" + h) for p in outside)} | set(prelude)

    # Only the library's headers that the one header reaches are shipped:
    # the rest are its internals (FFmpeg has hundreds), and the package's
    # size counts.
    reached = set()
    for path in opened:
        try:
            reached.add(pathlib.Path(path).resolve().relative_to(include_root.resolve()).as_posix())
        except ValueError:
            pass
    for relative in list(files):
        if relative not in reached:
            (include_root / relative).unlink(missing_ok=True)
            del files[relative]

    defined = defined_functions(args.nm, spec["objects"], TARGET == "mac")
    functions = []
    for symbol in sorted(defined):
        if symbol in declared:
            functions.append((renamed.get(symbol, symbol), symbol))
    if not functions:
        raise SystemExit(f"error: {name}: no function is both declared and defined")
    prefix = f"zebkit_vendor_{name}_"

    guard = f"ZEBKIT_VENDOR_{name.upper()}_H"
    title = f"{spec['title']} {version}".strip()
    lines = [
        f"/* {title}, as built into zebkit. Generated by gen_vendor.py.",
        " *",
        f" * C:   the library's API, as the library declares it.",
        f" * C++: the same, in the namespace zebkit::vendor::{name}.",
        " *",
        " * Link with libzebkit and nothing else. Each function is taken from the",
        f" * engine under the name {prefix}<function>, which this header arranges;",
        " * the engine does not export the library's own names, so another copy of",
        " * the library in the same process is a different one and does no harm.",
        " * Macros and inline functions are the library's own.",
        " *",
        " * What is there is what the engine was built with. See docs/API.md. */",
        "",
        f"#ifndef {guard}",
        f"#define {guard}",
        "",
    ]
    for key, value in defines.items():
        lines += [f"#ifndef {key}", f"#define {key} {value}", "#endif"]
    if defines:
        lines += [""]
    lines += [f"#include <{h}>" for h in sorted(system)]
    lines += [""]
    # The pragma's name is the symbol as it is written, which on macOS has
    # the underscore Mach-O puts before every C name.
    mangle = "_" if TARGET == "mac" else ""
    if TARGET == "win":
        # MSVC's cl has no such pragma; a macro does for any compiler there.
        lines += [f"#define {symbol} {prefix}{function}" for function, symbol in functions]
        # A declaration the library's header leaves out when the name is a macro.
        lines += spec.get("declare_if_macro", [])
    else:
        lines += [f"#pragma redefine_extname {symbol} {mangle}{prefix}{function}" for function, symbol in functions]
    lines += [
        "",
        "#ifdef __cplusplus",
        f"namespace zebkit {{ namespace vendor {{ namespace {name} {{",
        'extern "C" {',
        "#endif",
        "",
    ]
    lines += [f'#include "{name}/{h}"' for h in spec["include"]]
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
    write_if_changed(include_root.parent / f"{name}.h", "\n".join(lines))

    write_if_changed(out / "vendor" / f"{name}.txt", "".join(f"{f}\n" for f, _ in functions))
    if TARGET == "win":
        # lld-link's export of a function under another name, in the object's
        # directives. Windows' entry point is the function's address itself.
        c = [f"// {title} for applications. Generated by gen_vendor.py; do not edit.", ""]
        c += [f'#pragma comment(linker, "/export:{prefix}{function}={symbol}")' for function, symbol in functions]
        c += [""]
        write_if_changed(out / "vendor" / f"{name}.c", "\n".join(c))
        return len(functions)

    # A jump, not a call: every argument, variable ones too, passes
    # through untouched, and the function returns to the caller.
    asm = [
        f"// {title} for applications. Generated by gen_vendor.py; do not edit.",
        "",
        ".text",
    ]
    for function, symbol in functions:
        entry = prefix + function
        if TARGET == "mac":
            asm += [
                f".globl _{entry}",
                ".p2align 2",
                f"_{entry}:",
                f"\tb _{symbol}",
            ]
            continue
        asm += [
            f".globl {entry}",
            f".type {entry},@function",
            ".p2align 4",
            f"{entry}:",
            "\tendbr64",
            f"\tjmp {symbol}",
            f".size {entry},.-{entry}",
        ]
    if TARGET != "mac":
        asm += ['.section .note.GNU-stack,"",@progbits']
    asm += [""]
    write_if_changed(out / "vendor" / f"{name}.S", "\n".join(asm))
    return len(functions)


def write_if_changed(path, text):
    path.parent.mkdir(parents=True, exist_ok=True)
    if path.exists() and path.read_text() == text:
        return
    path.write_text(text)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--target", choices=sorted(TRIPLES), default="linux")
    parser.add_argument("--src", required=True)
    parser.add_argument("--clang", required=True)
    parser.add_argument("--sysroot", required=True)
    parser.add_argument("--nm", required=True)
    parser.add_argument("--out", required=True)
    parser.add_argument("--stamp", required=True)
    parser.add_argument("libraries", nargs="+")
    args = parser.parse_args()
    global TARGET
    TARGET = args.target
    if TARGET == "win":
        # Chromium's configuration of libxml2 for Windows.
        LIBRARIES["libxml"]["roots"]["linux"] = "third_party/libxml/win32/include"
    report = []
    for name in args.libraries:
        report.append(f"{name} {generate(name, LIBRARIES[name], args)}")
    pathlib.Path(args.stamp).write_text("\n".join(report) + "\n")


if __name__ == "__main__":
    sys.exit(main())
