#!/usr/bin/env python3
"""Lay out MSVC and the Windows SDK as Chromium's Windows toolchain, on Linux.

    windows-toolchain.py <.cache/windows> <chromium src>

Run by scripts/windows-toolchain.sh, which says what comes from where. Lays
the tree out as build/vs_toolchain.py's packaged toolchain has it
(depot_tools/win_toolchain/package_from_installed.py makes that on a Windows
machine):

    vs_files/VC/Tools/MSVC/<version>/    the C++ library, ATL, the x64 tools
    vs_files/VC/Redist/MSVC/<version>/   the C++ runtime's DLLs
    vs_files/DIA SDK/
    vs_files/Windows Kits/10/            Include, Lib, bin, Debuggers, Redist
    vs_files/Windows Kits/10/bin/SetEnv.{x86,x64}.json
    vs_files/sys64                       the runtime's DLLs, for vs_toolchain.py
    win_toolchain.json                   what build/win_toolchain.json points to

A second run with everything in place only refreshes the links for names in
another case (see add_case_links).
"""

import hashlib
import json
import os
import pathlib
import re
import shutil
import sys
import urllib.request
import zipfile

# What build/vs_toolchain.py of this Chromium asks for: Visual Studio 2026
# (its toolset, VC145) and SDK_VERSION, with the servicing release its comment
# names, from NuGet, the one place Microsoft publishes that exact release.
SDK_VERSION = "10.0.28000.0"
VS_YEAR = "2026"
VS_CHANNEL = "https://aka.ms/vs/18/stable/channel"
MSVC_PREFIX = "Microsoft.VC.14.50.18.0."
MSVC_PACKAGES = [
    "CRT.Headers.base",
    "CRT.x64.Desktop.base",
    "CRT.x64.Store.base",
    "ATL.Headers.base",
    "ATL.X64.base",
    "CRT.Redist.X64.base",
    "Tools.HostX64.TargetX64.base",
]
OTHER_VS_PACKAGES = ["Microsoft.VisualCpp.DIA.SDK"]

NUGET = "https://api.nuget.org/v3-flatcontainer/{0}/{1}/{0}.{1}.nupkg"
SDK_PACKAGES = {
    ("microsoft.windows.sdk.cpp", "10.0.28000.2270"):
        "0a4887a64d1b17128f9ffd80032f20debabf943f34eea927aac00cc46cb32879",
    ("microsoft.windows.sdk.cpp.x64", "10.0.28000.2270"):
        "72076d00b16c3882bf9eab0c80e350f33d8bc65e5f29e2f0e9b66a3f2569ccb1",
}
# vs_toolchain.py copies dbghelp.dll from the SDK's debuggers at gn gen; the
# SDK's NuGet packages leave them out, and the debugging tools have their own.
DEBUGGERS_PACKAGE = ("microsoft.debugging.platform.dbgeng", "20260319.1511.0")
DEBUGGERS_SHA256 = "875678516f9ceed4a1c8b9b106d165106fcaa38723ad7c584c47b95b231600de"


def log(message):
    print(f"==> {message}", file=sys.stderr, flush=True)


def sha256_of(path):
    digest = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def download(url, path, sha256):
    if path.exists() and sha256_of(path) == sha256:
        return path
    log(f"fetching {path.name}")
    partial = path.with_name(path.name + ".partial")
    with urllib.request.urlopen(url, timeout=120) as response, open(partial, "wb") as out:
        shutil.copyfileobj(response, out, 1 << 20)
    got = sha256_of(partial)
    if got != sha256:
        partial.unlink()
        raise SystemExit(f"{path.name}: SHA-256 {got}, expected {sha256}")
    partial.rename(path)
    return path


def vs_packages(downloads):
    """The packages wanted from the current Visual Studio manifest, each with
    its SHA-256 from that manifest."""
    with urllib.request.urlopen(VS_CHANNEL, timeout=120) as response:
        channel = json.load(response)
    item = next(i for i in channel["channelItems"] if i["type"] == "Manifest")
    with urllib.request.urlopen(item["payloads"][0]["url"], timeout=300) as response:
        manifest = json.load(response)
    wanted = {MSVC_PREFIX + name for name in MSVC_PACKAGES} | set(OTHER_VS_PACKAGES)
    found = {}
    for package in manifest["packages"]:
        if package["id"] in wanted:
            found[package["id"]] = package
    missing = wanted - set(found)
    if missing:
        raise SystemExit(f"not in the Visual Studio manifest: {sorted(missing)}")
    paths = {}
    for name, package in sorted(found.items()):
        payload = package["payloads"][0]
        paths[name] = download(
            payload["url"], downloads / f"{name}-{package['version']}.vsix", payload["sha256"].lower()
        )
    return paths


def unzip(archive, root, rename):
    """Extract what rename() gives a path under root for, and refuse any path
    that would leave root."""
    with zipfile.ZipFile(archive) as z:
        for info in z.infolist():
            if info.is_dir():
                continue
            name = urllib.request.unquote(info.filename)
            relative = rename(name)
            if relative is None:
                continue
            target = (root / relative).resolve()
            if root.resolve() not in target.parents:
                raise SystemExit(f"{archive.name}: {name} leaves the tree")
            target.parent.mkdir(parents=True, exist_ok=True)
            with z.open(info) as source, open(target, "wb") as out:
                shutil.copyfileobj(source, out, 1 << 20)


def set_env(root, msvc):
    """The Windows Kits/10/bin/SetEnv.<cpu>.json of package_from_installed.py,
    which setup_toolchain.py reads."""
    kit = ["Windows Kits", "10"]
    vc = ["VC", "Tools", "MSVC", msvc]
    common = {
        "VSINSTALLDIR": [[".\\"]],
        "VCINSTALLDIR": [["VC\\"]],
        "INCLUDE": [
            kit + ["Include", SDK_VERSION, "um"],
            kit + ["Include", SDK_VERSION, "shared"],
            kit + ["Include", SDK_VERSION, "winrt"],
            kit + ["Include", SDK_VERSION, "ucrt"],
            vc + ["include"],
            vc + ["atlmfc", "include"],
        ],
        "LIBPATH": [vc + ["lib", "x86", "store", "references"], kit + ["UnionMetadata", SDK_VERSION]],
        "VCToolsInstallDir": [vc[:-1] + [msvc + os.path.sep]],
    }
    for cpu in ("x86", "x64"):
        env = dict(common)
        env["PATH"] = [kit + ["bin", SDK_VERSION, "x64"], vc + ["bin", "HostX64", "x64"]]
        env["LIB"] = [
            vc + ["lib", cpu],
            kit + ["Lib", SDK_VERSION, "um", cpu],
            kit + ["Lib", SDK_VERSION, "ucrt", cpu],
            vc + ["atlmfc", "lib", cpu],
        ]
        # GN declares the x86 toolchain beside x64 and setup_toolchain.py
        # wants its directories to exist. Nothing in the engine builds for it,
        # so its libraries are not fetched.
        for parts in env["LIB"] + common["LIBPATH"]:
            (root.joinpath(*parts)).mkdir(parents=True, exist_ok=True)
        (root.joinpath(*kit, "bin")).mkdir(parents=True, exist_ok=True)
        with open(root.joinpath(*kit, "bin", f"SetEnv.{cpu}.json"), "w") as f:
            json.dump({"env": env}, f, indent=2)


def assemble(cache, downloads, tree):
    sdk, sdk_x64 = (download(NUGET.format(*key), downloads / f"{key[0]}.{key[1]}.nupkg", digest)
                    for key, digest in SDK_PACKAGES.items())
    debuggers = download(
        NUGET.format(*DEBUGGERS_PACKAGE),
        downloads / f"{DEBUGGERS_PACKAGE[0]}.{DEBUGGERS_PACKAGE[1]}.nupkg",
        DEBUGGERS_SHA256,
    )
    vs = vs_packages(downloads)

    log("unpacking")
    if tree.exists():
        shutil.rmtree(tree)

    # Each package of a toolset names its own patch version; clang's
    # /winsysroot looks in one directory, the highest, so they share the CRT's.
    with zipfile.ZipFile(vs[MSVC_PREFIX + "CRT.Headers.base"]) as z:
        msvc = next(
            m.group(1) for m in map(re.compile(r"Contents/VC/Tools/MSVC/([\d.]+)/").match, z.namelist()) if m
        )

    def vs_path(name):
        if not name.startswith("Contents/"):
            return None
        name = name[len("Contents/"):]
        name = re.sub(r"^VC/Tools/MSVC/[\d.]+/", f"VC/Tools/MSVC/{msvc}/", name)
        name = re.sub(r"^VC/Redist/MSVC/[\d.]+/", f"VC/Redist/MSVC/{msvc}/", name)
        # The packages say Hostx64, and SetEnv and Chromium's tools HostX64.
        name = name.replace("/bin/Hostx64/", "/bin/HostX64/")
        return name

    for archive in vs.values():
        unzip(archive, tree, vs_path)

    def sdk_path(name):
        if not name.startswith("c/"):
            return None
        tail = name[2:]
        # What the engine has no use for: 100 MiB of documentation and sources,
        # and tools for other machines.
        if re.match(r"(Source|References|DesignTime|Extension SDKs|Platforms)/", tail):
            return None
        if re.match(rf"bin/{re.escape(SDK_VERSION)}/(x86|arm64)/", tail):
            return None
        return f"Windows Kits/10/{tail}"

    unzip(sdk, tree, sdk_path)

    def sdk_lib_path(name):
        m = re.match(r"c/(um|ucrt)/x64/(.+)", name)
        return f"Windows Kits/10/Lib/{SDK_VERSION}/{m[1]}/x64/{m[2]}" if m else None

    unzip(sdk_x64, tree, sdk_lib_path)

    def debuggers_path(name):
        m = re.match(r"content/amd64/(dbghelp|dbgcore|symsrv)\.dll$", name, re.I)
        return f"Windows Kits/10/Debuggers/x64/{m[1]}.dll" if m else None

    unzip(debuggers, tree, debuggers_path)

    for needed in (
        f"VC/Tools/MSVC/{msvc}/include/vector",
        f"VC/Tools/MSVC/{msvc}/atlmfc/include/atlbase.h",
        f"VC/Tools/MSVC/{msvc}/lib/x64/libcmt.lib",
        f"VC/Tools/MSVC/{msvc}/bin/HostX64/x64/cl.exe",
        f"Windows Kits/10/Include/{SDK_VERSION}/um/Windows.h",
        f"Windows Kits/10/Lib/{SDK_VERSION}/ucrt/x64/ucrt.lib",
        "Windows Kits/10/Debuggers/x64/dbghelp.dll",
    ):
        if not (tree / needed).exists():
            raise SystemExit(f"not in the packages: {needed}")

    set_env(tree, msvc)

    # The runtime's DLLs, where vs_toolchain.py is told to look. Only x64 has any.
    redist = next((tree / "VC/Redist/MSVC" / msvc / "x64").glob("Microsoft.VC*.CRT"))
    (tree / "sys64").symlink_to(redist.relative_to(tree))
    (tree / "sys32").mkdir()
    (tree / "sysarm64").mkdir()
    (tree / "wdk").mkdir()

    (cache / "win_toolchain.json").write_text(json.dumps({
        "path": str(tree),
        "version": VS_YEAR,
        "win_sdk": str(tree / "Windows Kits/10"),
        "wdk": str(tree / "wdk"),
        "runtime_dirs": [str(tree / "sys64"), str(tree / "sys32"), str(tree / "sysarm64")],
    }, indent=2) + "\n")
    (cache / "VERSIONS").write_text(
        f"MSVC {msvc} ({MSVC_PREFIX[:-1]})\n"
        f"Windows SDK {SDK_VERSION} ({' '.join(k[1] for k in SDK_PACKAGES)})\n"
        f"Debuggers {DEBUGGERS_PACKAGE[1]}\n"
    )


INCLUDE = re.compile(rb'^[ \t]*#[ \t]*(?:include|import)[ \t]*[<"]([^">\r\n]+)[">]', re.M)
PRAGMA_LIB = re.compile(rb'#[ \t]*pragma[ \t]+comment[ \t]*\([ \t]*lib[ \t]*,[ \t]*"([^"]+)"', re.I)
GN_LIB = re.compile(rb'"([A-Za-z0-9_.+-]+\.[Ll][Ii][Bb])"')
SOURCE_SUFFIXES = {".h", ".hh", ".hpp", ".hxx", ".inc", ".inl", ".c", ".cc", ".cpp", ".cxx", ".S", ".asm",
                   ".idl", ".rc", ".mm", ".ipp", ".tcc", ".def"}
SKIP_DIRS = {".git", "out", "llvm-build", "rust-toolchain", "node_modules", "test_data", "web_tests", "wpt"}


def wanted_names(src, tree):
    """Every name an #include, a #pragma comment(lib) or a BUILD.gn asks for."""
    includes, libs = set(), set()
    roots = [src, tree]
    for root in roots:
        for directory, subdirs, files in os.walk(root):
            subdirs[:] = [d for d in subdirs if d not in SKIP_DIRS]
            for name in files:
                path = os.path.join(directory, name)
                suffix = os.path.splitext(name)[1]
                is_gn = suffix in (".gn", ".gni")
                # The C++ library's headers have no suffix.
                if not (is_gn or suffix in SOURCE_SUFFIXES or root is tree):
                    continue
                if os.path.islink(path):
                    continue
                try:
                    with open(path, "rb") as f:
                        data = f.read()
                except OSError:
                    continue
                if is_gn:
                    libs.update(GN_LIB.findall(data))
                    continue
                if b"#" not in data:
                    continue
                includes.update(INCLUDE.findall(data))
                libs.update(PRAGMA_LIB.findall(data))
    decode = lambda names: {n.decode("latin-1").replace("\\", "/") for n in names}
    return decode(includes), decode(libs)


def names_in(directory, cache={}):
    """A directory's entries by their lower-case name. Nearly every name asked
    for is Chromium's own and in no directory here, so this is asked millions
    of times."""
    key = str(directory)
    if key not in cache:
        try:
            cache[key] = {}
            for entry in sorted(os.listdir(directory)):
                cache[key].setdefault(entry.lower(), set()).add(entry)
        except (NotADirectoryError, FileNotFoundError):
            pass
    return cache[key]


def resolve_case(directory, parts):
    """Walk parts down from directory, adding a link wherever a name exists
    only in another case. Returns whether the path now exists."""
    current = directory
    planned = []
    for part in parts:
        if part in ("", "."):
            continue
        spellings = names_in(current).get(part.lower())
        if not spellings:
            return False
        if part in spellings:
            current = current / part
            continue
        real = min(spellings)
        planned.append((current, part, real))
        current = current / real
    for where, link, real in planned:
        (where / link).symlink_to(real)
        names_in(where)[link.lower()].add(link)
    return True


def add_case_links(src, tree):
    """clang and lld look for a header or library in the case it is named in,
    which works on Windows' file system and not on Linux's. Chromium's answer
    is ciopfs, a case-insensitive FUSE mount, which needs /dev/fuse and a mount
    after every boot. Here each name asked for, by Chromium's sources or the
    SDK's own headers, gets a link in that case beside the file. Run again
    after a Chromium bump."""
    msvc = next((tree / "VC/Tools/MSVC").iterdir())
    kit = tree / "Windows Kits/10"
    include_roots = [kit / "Include" / SDK_VERSION / n for n in ("um", "shared", "winrt", "ucrt", "cppwinrt")]
    include_roots += [msvc / "include", msvc / "atlmfc/include"]
    lib_roots = [kit / "Lib" / SDK_VERSION / "um/x64", kit / "Lib" / SDK_VERSION / "ucrt/x64",
                 msvc / "lib/x64", msvc / "atlmfc/lib/x64"]

    log("reading every source for the names of headers and libraries")
    includes, libs = wanted_names(src, tree)
    added_before = sum(1 for root in include_roots + lib_roots for _ in walk_links(root))
    for name in sorted(includes):
        parts = name.split("/")
        if ".." in parts or name.startswith("/"):
            continue
        for root in include_roots:
            if resolve_case(root, parts):
                break
    # lld-link adds .lib to a name without a suffix, and every library is
    # also reachable in lower case with a lower-case suffix.
    for root in lib_roots:
        for entry in sorted(os.listdir(root)):
            if entry.lower().endswith(".lib"):
                resolve_case(root, [entry.lower()])
    for name in sorted(libs):
        if "/" in name:
            continue
        for root in lib_roots:
            if resolve_case(root, [name]):
                break
    added = sum(1 for root in include_roots + lib_roots for _ in walk_links(root))
    log(f"links for names in another case: {added} ({added - added_before} new)")


def walk_links(root):
    for directory, subdirs, files in os.walk(root):
        for name in subdirs + files:
            if os.path.islink(os.path.join(directory, name)):
                yield name


def main():
    cache = pathlib.Path(sys.argv[1]).resolve()
    src = pathlib.Path(sys.argv[2]).resolve()
    downloads = cache / "downloads"
    downloads.mkdir(parents=True, exist_ok=True)
    tree = cache / "vs_files"
    if not (cache / "win_toolchain.json").exists():
        assemble(cache, downloads, tree)
    add_case_links(src, tree)


if __name__ == "__main__":
    main()
