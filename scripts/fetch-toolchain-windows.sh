#!/usr/bin/env bash
# Download what building for Windows on Linux needs (toolchain.lock) into
# out/toolchain/windows/:
#
#   llvm/      clang-cl, lld-link and the LLVM tools that stand in for
#              Microsoft's (lib, rc, mt), with the compiler's own runtime
#              library for Windows
#   sysroot/   Microsoft's Windows SDK and C++ runtime: headers and import
#              libraries, fetched from Microsoft by xwin
#   vcpkg/     the package manager that builds the libraries WebKit links
#   bin/pwsh   a stand-in for PowerShell, which vcpkg asks for when the
#              target is Windows and has no use for here
#   wine/      Wine, which runs what was built on this machine
#              (scripts/wine.sh). It says whether the engine starts and
#              loads a page; it is not Windows
#
# The SDK and runtime are Microsoft's and under its license for the Visual
# Studio Build Tools (https://go.microsoft.com/fwlink/?LinkId=2086102).
# Running this script accepts it. Nothing of them is in a zebkit package
# except the runtime DLLs Microsoft allows to be redistributed.

ZEBKIT_TARGET=windows
source "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

dir="$WINDOWS_TOOLCHAIN_DIR"
cache="$CACHE_DIR/toolchain-windows"
stamp="$dir/.zebkit-stamp"
want="$WINDOWS_LLVM_SHA256 $WINDOWS_CLANG_RT_SHA256 $WINDOWS_XWIN_SHA256 $WINDOWS_SDK_VERSION $WINDOWS_CRT_VERSION $WINDOWS_VCPKG_COMMIT $WINDOWS_WINE_SHA256 $(sha256sum "${BASH_SOURCE[0]}" | cut -d' ' -f1)"

if [[ -f "$stamp" && "$(cat "$stamp")" == "$want" ]]; then
    log "Windows toolchain is up to date: $dir"
    exit 0
fi

mkdir -p "$cache" "$dir/bin"

# fetch <url> <sha256>: download into the cache unless it is there, and
# print the file's path.
fetch() {
    local file="$cache/$(basename "$1")"
    if [[ ! -f "$file" ]] || ! echo "$2  $file" | sha256sum -c --status; then
        log "downloading $1"
        curl -fL --retry 3 -o "$file.part" "$1"
        mv "$file.part" "$file"
    fi
    echo "$2  $file" | sha256sum -c --status || die "checksum mismatch for $file"
    echo "$file"
}

# LLVM. Its release is 2 GB and has everything LLVM builds; what is used is
# the compiler, the linker, a handful of tools and the compiler's headers.
llvm_tarball="$(fetch "$WINDOWS_LLVM_URL" "$WINDOWS_LLVM_SHA256")"
log "extracting LLVM $WINDOWS_LLVM_VERSION"
rm -rf "$dir/llvm"
mkdir -p "$dir/llvm"
tools='clang clang-[0-9]* clang-cl lld lld-link llvm-ar llvm-lib llvm-ranlib llvm-rc llvm-mt llvm-objdump llvm-readobj llvm-objcopy llvm-strip llvm-pdbutil'
members=('*/lib/clang/*')
for tool in $tools; do members+=("*/bin/$tool"); done
tar -xf "$llvm_tarball" -C "$dir/llvm" --strip-components=1 --wildcards "${members[@]}"

# The compiler's runtime library for Windows (128-bit arithmetic and the
# like) comes with LLVM's release for Windows only.
major="${WINDOWS_LLVM_VERSION%%.*}"
runtime="lib/clang/$major/lib/windows/clang_rt.builtins-x86_64.lib"
clang_rt_tarball="$(fetch "$WINDOWS_CLANG_RT_URL" "$WINDOWS_CLANG_RT_SHA256")"
tar -xf "$clang_rt_tarball" -C "$dir/llvm" --strip-components=1 --wildcards "*/$runtime"
[[ -f "$dir/llvm/$runtime" ]] || die "no $runtime in $clang_rt_tarball"

# Microsoft's SDK and runtime. The debug libraries are wanted too: CMake
# tries each compiler out with a debug build.
xwin_tarball="$(fetch "$WINDOWS_XWIN_URL" "$WINDOWS_XWIN_SHA256")"
tar -xzf "$xwin_tarball" -C "$cache" --strip-components=1 --wildcards '*/xwin'
log "fetching the Windows SDK $WINDOWS_SDK_VERSION and the C++ runtime $WINDOWS_CRT_VERSION from Microsoft"
rm -rf "$dir/sysroot"
"$cache/xwin" --accept-license --cache-dir "$cache/xwin-cache" -L warn \
    --sdk-version "$WINDOWS_SDK_VERSION" --crt-version "$WINDOWS_CRT_VERSION" \
    splat --preserve-ms-arch-notation --include-debug-libs --output "$dir/sysroot"
[[ -f "$dir/sysroot/sdk/include/um/windows.h" ]] || die "xwin left no SDK in $dir/sysroot"

# Windows does not tell file names apart by case and Linux does. xwin adds
# the spellings the SDK's own headers use; these are the ones the libraries
# and the engine ask for besides.
link_case() {
    local directory="$1" file base name
    shift
    for file in "$directory"/*.lib "$directory"/*.Lib; do
        [[ -f "$file" ]] || continue
        base="$(basename "${file%.*}")"
        for name in "${base,,}" "$base" "${base^^}" "${base^}" "$@"; do
            [[ "${name,,}" == "${base,,}" && ! -e "$directory/$name.lib" ]] && ln -s "$(basename "$file")" "$directory/$name.lib"
        done
    done
    return 0
}
mixed=(WindowsCodecs D2d1 Dwrite DWrite Iphlpapi Shlwapi DbgHelp)
link_case "$dir/sysroot/sdk/lib/um/x64" "${mixed[@]}"
link_case "$dir/sysroot/sdk/lib/ucrt/x64"
link_case "$dir/sysroot/crt/lib/x64"
[[ -e "$dir/sysroot/sdk/include/um/Winver.h" ]] || ln -s winver.h "$dir/sysroot/sdk/include/um/Winver.h"

# Every file of the SDK gets one date, the same on every fetch. A build
# remembers the dates of the headers it compiled against (precompiled
# headers, and the compiler cache through them), and with the dates of the
# download a toolchain fetched again would make every build tree stale
# without a byte having changed.
find "$dir/sysroot" -exec touch -h -d '2000-01-01 00:00:00 UTC' {} +

# vcpkg, at the commit WebKit's vcpkg-configuration.json names as the
# baseline of its package versions.
if [[ "$(git -C "$dir/vcpkg" rev-parse HEAD 2>/dev/null)" != "$WINDOWS_VCPKG_COMMIT" ]]; then
    log "fetching vcpkg $WINDOWS_VCPKG_COMMIT"
    rm -rf "$dir/vcpkg"
    git init -q "$dir/vcpkg"
    git -C "$dir/vcpkg" remote add origin "$WINDOWS_VCPKG_URL"
    git -C "$dir/vcpkg" fetch -q --depth 1 origin "$WINDOWS_VCPKG_COMMIT"
    git -C "$dir/vcpkg" checkout -q FETCH_HEAD
fi
[[ -x "$dir/vcpkg/vcpkg" ]] || "$dir/vcpkg/bootstrap-vcpkg.sh" -disableMetrics > /dev/null

wine_tarball="$(fetch "$WINDOWS_WINE_URL" "$WINDOWS_WINE_SHA256")"
log "extracting Wine"
rm -rf "$dir/wine"
mkdir -p "$dir/wine"
tar -xf "$wine_tarball" -C "$dir/wine" --strip-components=1

printf '#!/bin/sh\n# Generated by scripts/fetch-toolchain-windows.sh.\ncase "$1" in --version) echo "PowerShell 7.5.4";; esac\nexit 0\n' > "$dir/bin/pwsh"
chmod +x "$dir/bin/pwsh"

# The compiler and the linker with the SDK in their search paths, for the
# libraries zebkit builds itself (scripts/build-deps.sh): a build system
# wants a compiler to be one program.
sdk="$dir/sysroot"
wrapper() {
    printf '#!/bin/sh\n# Generated by scripts/fetch-toolchain-windows.sh.\n%s\n' "$2" > "$dir/bin/$1"
    chmod +x "$dir/bin/$1"
}
wrapper zebkit-cl "exec \"$dir/llvm/bin/clang-cl\" -imsvc \"$sdk/crt/include\" -imsvc \"$sdk/sdk/include/ucrt\" -imsvc \"$sdk/sdk/include/um\" -imsvc \"$sdk/sdk/include/shared\" \"\$@\""
wrapper zebkit-link "exec \"$dir/llvm/bin/lld-link\" -libpath:\"$sdk/crt/lib/x64\" -libpath:\"$sdk/sdk/lib/ucrt/x64\" -libpath:\"$sdk/sdk/lib/um/x64\" \"\$@\""

echo "$want" > "$stamp"
log "Windows toolchain ready: $("$dir/llvm/bin/clang-cl" --version | head -1)"
