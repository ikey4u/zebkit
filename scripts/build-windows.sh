#!/usr/bin/env bash
# Build one profile of the Windows engine into out/windows/<profile>/. This
# is what scripts/build.sh runs when ZEBKIT_TARGET=windows.
#
#   ZEBKIT_TARGET=windows scripts/build.sh <profile> [--fresh] [--configure-only] [ninja args...]
#
# The engine is WebKit's Windows port, and it is built here, on Linux:
# WebKit's CMake build with clang-cl, against Microsoft's SDK and C++
# runtime, linking libraries vcpkg builds from WebKit's own manifest. All
# three come from scripts/fetch-toolchain-windows.sh. The libraries are
# built by the first configure, into out/windows/<profile>/vcpkg_installed.

source "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

profile="${1:-}"
require_profile "$profile"
shift

configure_only=0
fresh=()
while [[ "${1:-}" == --* ]]; do
    case "$1" in
        --configure-only) configure_only=1 ;;
        --fresh) fresh=(--fresh) ;;
        *) break ;;
    esac
    shift
done

[[ -d "$SRC_DIR/.git" ]] || die "no upstream tree; run ZEBKIT_TARGET=windows scripts/fetch-upstream.sh first"
llvm="$WINDOWS_TOOLCHAIN_DIR/llvm/bin"
sysroot="$WINDOWS_TOOLCHAIN_DIR/sysroot"
vcpkg="$WINDOWS_TOOLCHAIN_DIR/vcpkg"
[[ -x "$llvm/clang-cl" && -f "$sysroot/sdk/include/um/windows.h" && -x "$vcpkg/vcpkg" ]] \
    || die "no Windows toolchain; run scripts/fetch-toolchain-windows.sh first"

build_dir="$OUT_DIR/$profile"
triplet=x64-windows-zebkit
installed="$build_dir/vcpkg_installed"
mkdir -p "$build_dir"

# WebKit's build looks for the SDK inside its own tree.
ln -sfn "$sysroot" "$SRC_DIR/WebKitLibraries/windows"

# Nothing of this machine's own libraries may reach a build for Windows.
unset C_INCLUDE_PATH CPLUS_INCLUDE_PATH CPATH LIBRARY_PATH LD_LIBRARY_PATH \
      LDFLAGS CFLAGS CXXFLAGS CPPFLAGS CMAKE_PREFIX_PATH PKG_CONFIG_PATH
export PKG_CONFIG_LIBDIR="$installed/$triplet/lib/pkgconfig"
export VCPKG_ROOT="$vcpkg" VCPKG_DISABLE_METRICS=1
# What vcpkg downloads and the packages it has built are kept with the
# other downloads, so a new build tree does not start from nothing.
export VCPKG_DOWNLOADS="$CACHE_DIR/vcpkg/downloads"
export VCPKG_DEFAULT_BINARY_CACHE="$CACHE_DIR/vcpkg/archives"
export X_VCPKG_REGISTRIES_CACHE="$CACHE_DIR/vcpkg/registries"
mkdir -p "$VCPKG_DOWNLOADS" "$VCPKG_DEFAULT_BINARY_CACHE" "$X_VCPKG_REGISTRIES_CACHE"
# The tools, and the stand-in for PowerShell that vcpkg asks for when the
# target is Windows.
export PATH="$WINDOWS_TOOLCHAIN_DIR/bin:$llvm:$PATH"

# -fno-pch-timestamp: a precompiled header otherwise records the date of
# every file that went into it and is refused when one differs, and the
# compiler cache hands out precompiled headers made at another time.
include="-Xclang -fno-pch-timestamp -imsvc $sysroot/crt/include -imsvc $sysroot/sdk/include/ucrt -imsvc $sysroot/sdk/include/um -imsvc $sysroot/sdk/include/shared -imsvc $sysroot/sdk/include/winrt"
libpath="-libpath:$sysroot/crt/lib/x64 -libpath:$sysroot/sdk/lib/ucrt/x64 -libpath:$sysroot/sdk/lib/um/x64"

cmake_args=(-S "$SRC_DIR" -B "$build_dir" -G Ninja -C "$PROFILES_DIR/$profile.cmake"
            -DCMAKE_TOOLCHAIN_FILE="$vcpkg/scripts/buildsystems/vcpkg.cmake"
            -DVCPKG_OVERLAY_TRIPLETS="$ZEBKIT_ROOT/deps/windows"
            -DVCPKG_TARGET_TRIPLET="$triplet"
            -DVCPKG_INSTALLED_DIR="$installed"
            -DVCPKG_APPLOCAL_DEPS=OFF
            -DCMAKE_SYSTEM_NAME=Windows
            -DCMAKE_C_COMPILER="$llvm/clang-cl"
            -DCMAKE_CXX_COMPILER="$llvm/clang-cl"
            -DCMAKE_LINKER="$llvm/lld-link"
            -DCMAKE_AR="$llvm/llvm-lib"
            -DCMAKE_RANLIB="$llvm/llvm-ranlib"
            -DCMAKE_RC_COMPILER="$llvm/llvm-rc"
            -DCMAKE_MT="$llvm/llvm-mt"
            -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreadedDLL
            -DCMAKE_C_FLAGS_INIT="$include"
            -DCMAKE_CXX_FLAGS_INIT="$include"
            -DCMAKE_EXE_LINKER_FLAGS_INIT="$libpath"
            -DCMAKE_SHARED_LINKER_FLAGS_INIT="$libpath"
            -DCMAKE_MODULE_LINKER_FLAGS_INIT="$libpath"
            -DCMAKE_MAKE_PROGRAM="$(command -v ninja)"
            -DZEBKIT_ENGINE_VERSION="$UPSTREAM_VERSION")

if [[ ! -f "$build_dir/build.ninja" || ${#fresh[@]} -gt 0 || $configure_only -ne 0 \
    || -n "$(find "$PROFILES_DIR" -type f -newer "$build_dir/build.ninja" -print -quit)" ]]; then
    log "configuring profile '$profile' for Windows"
    cmake "${fresh[@]}" "${cmake_args[@]}"
fi

(( configure_only )) && exit 0

attempts="${BUILD_ATTEMPTS:-3}"
for (( attempt = 1; attempt <= attempts; attempt++ )); do
    log "building profile '$profile' for Windows (pass $attempt of $attempts)"
    status=0
    ninja -C "$build_dir" "$@" || status=$?
    (( status == 0 )) && exit 0
    (( status > 128 )) && die "build of profile '$profile' was interrupted"
done
die "build of profile '$profile' failed after $attempts passes"
