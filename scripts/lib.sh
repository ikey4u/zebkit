# Shared paths and helpers. Source this; do not execute it.

set -euo pipefail
# macOS's bash (3.2) takes an empty array for an unset variable under -u,
# which bash 4.4 stopped doing; the scripts expand arrays that may be empty.
if (( BASH_VERSINFO[0] < 4 || (BASH_VERSINFO[0] == 4 && BASH_VERSINFO[1] < 4) )); then
    set +u
fi

# Work from the physical path, so that the same build tree is not reached
# under two spellings.
ZEBKIT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
cd -P "$ZEBKIT_ROOT"

# Keep host-injected libraries out of the compiler, the tools and the
# browser under test.
unset LD_PRELOAD

# shellcheck source=../upstream.lock
source "$ZEBKIT_ROOT/upstream.lock"

# The system this runs on: linux or macos.
case "$(uname -s)" in
    Darwin) ZEBKIT_HOST=macos ;;
    MINGW*|MSYS*) ZEBKIT_HOST=windows ;;
    *) ZEBKIT_HOST=linux ;;
esac
export ZEBKIT_HOST

log() { printf '\033[1m==> %s\033[0m\n' "$*" >&2; }
die() { printf 'error: %s\n' "$*" >&2; exit 1; }

CACHE_DIR="$ZEBKIT_ROOT/.cache"
# Apple's and Microsoft's SDKs for building on Linux (scripts/zsdk-create.sh):
# never deleted by a clean. ZEBKIT_SDK_DIR, or a link here, shares them between
# checkouts.
SDK_DIR="${ZEBKIT_SDK_DIR:-$CACHE_DIR/sdk}"
UPSTREAM_DIR="$ZEBKIT_ROOT/upstream"
# Packages and reports per profile. The builds themselves are in
# $SRC_DIR/out/<profile>, where Chromium's tools expect them.
OUT_DIR="$ZEBKIT_ROOT/out/blink"
PATCHES_DIR="$ZEBKIT_ROOT/patches"
PROFILES_DIR="$ZEBKIT_ROOT/profiles"
REPORTS_DIR="$ZEBKIT_ROOT/docs/reports"

# gclient's root: .gclient, and Chromium's tree under src/. One directory
# for every version, so that a bump fetches what changed and not 30 GB.
GCLIENT_DIR="$UPSTREAM_DIR/chromium"
SRC_DIR="$GCLIENT_DIR/src"
DEPOT_TOOLS_DIR="$CACHE_DIR/depot_tools"

# depot_tools and the tools it downloads cache in the home directory and
# in /tmp unless told otherwise. Everything goes under .cache instead, on
# the disk the checkout is on.
export PATH="$DEPOT_TOOLS_DIR:$PATH"
export DEPOT_TOOLS_UPDATE=0
export DEPOT_TOOLS_METRICS=0
export GCLIENT_SUPPRESS_GIT_VERSION_WARNING=1
export TMPDIR="$CACHE_DIR/tmp"
export XDG_CACHE_HOME="$CACHE_DIR/xdg"
export VPYTHON_VIRTUALENV_ROOT="$CACHE_DIR/vpython"
export CIPD_CACHE_DIR="$CACHE_DIR/cipd"
mkdir -p "$TMPDIR" "$XDG_CACHE_HOME" "$VPYTHON_VIRTUALENV_ROOT" "$CIPD_CACHE_DIR"

# sha256 [file...]: like sha256sum, which macOS does not have.
sha256() {
    if command -v sha256sum >/dev/null; then sha256sum "$@"; else shasum -a 256 "$@"; fi
}

# profile_args <name>: a profile's GN arguments, on top of _common.gn and of
# the profile it is based on ("# zebkit-base: <name>").
profile_args() {
    local base
    base="$(sed -n 's/^# zebkit-base: *//p' "$PROFILES_DIR/$1.gn")"
    if [[ -n "$base" ]]; then profile_args "$base"; else cat "$PROFILES_DIR/_common.gn"; fi
    cat "$PROFILES_DIR/$1.gn"
}

# require_profile <name>: fail unless there is a profile of that name.
require_profile() {
    [[ -n "${1:-}" ]] || die "usage: $(basename "$0") <profile>   (one of: $(list_profiles))"
    [[ -f "$PROFILES_DIR/$1.gn" ]] || die "unknown profile '$1' (have: $(list_profiles))"
}

# compressed_size <xz|zip> <dir>: the bytes a download of the directory
# takes. xz is a tar compressed with xz -6, as scripts/release.sh makes
# zebkit's archives; zip is the highest deflate level, the form Electron is
# distributed in. Symbolic links, which a macOS framework has, are kept as
# links in both.
compressed_size() {
    case "$1" in
        xz) tar -C "$2" --sort=name --owner=0 --group=0 --numeric-owner -cf - . | xz -6 -T0 | wc -c ;;
        zip) (cd "$2" && zip -q -9 -r -X -y - .) | wc -c ;;
        *) die "compressed_size: unknown method $1" ;;
    esac
}

list_profiles() {
    find "$PROFILES_DIR" -maxdepth 1 -name '*.gn' ! -name '_*' -exec basename {} .gn \; | sort | paste -sd' ' -
}

# limited <seconds> <command...>: run a command, and end it if it is still
# running after that long. macOS has no timeout(1); an alarm that survives
# exec does the same.
limited() {
    if command -v timeout >/dev/null; then
        timeout "$@"
    else
        perl -e 'alarm shift; exec @ARGV or die "$ARGV[0]: $!\n"' "$@"
    fi
}

# What a program is built for: the system this runs on, or macOS for a profile
# that builds it, cross-compiled here on Linux as the engine is (docs/MACOS.md).
ZEBKIT_TARGET="$ZEBKIT_HOST"
ZEBKIT_TARGET_RPATHS=""
# set_target_for_profile <name>: the programs are for that profile's system.
set_target_for_profile() {
    case "$1" in
        *macos*) ZEBKIT_TARGET=macos ;;
        *windows*) ZEBKIT_TARGET=windows ;;
        *) ZEBKIT_TARGET=linux ;;
    esac
    if [[ "$ZEBKIT_TARGET" == macos && "$ZEBKIT_HOST" != macos ]]; then
        macos_sdk="$SDK_DIR/macos/xcode/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk"
        llvm_bin="$SRC_DIR/third_party/llvm-build/Release+Asserts/bin"
        [[ -d "$macos_sdk" && -x "$llvm_bin/clang" ]] || die "no macOS SDK or clang; see scripts/macos-toolchain.sh"
    fi
    if [[ "$ZEBKIT_TARGET" == windows && "$ZEBKIT_HOST" != windows ]]; then
        llvm_bin="$SRC_DIR/third_party/llvm-build/Release+Asserts/bin"
        [[ -x "$llvm_bin/clang" ]] || die "no clang; run scripts/fetch-upstream.sh first"
        windows_sysroot="$(python3 -I -c 'import json, sys; print(json.load(open(sys.argv[1]))["path"])' "$SRC_DIR/build/win_toolchain.json")" \
            || die "no Windows toolchain; see scripts/windows-toolchain.sh"
    fi
}

# target_cc, target_cxx <options...>: the C and C++ compilers for the target. Across
# systems, Chromium's clang, Apple's SDK and ld64.lld, which signs ad hoc for arm64.
# That clang warns of what Xcode's and Linux's do not; -Werror is for what they share.
# For Windows, MSVC's libraries and the static C runtime, as the sandbox needs,
# and tests/engine/win_compat for the POSIX the tests use.
windows_compiler() {
    local tool="$1" args=() arg i
    shift
    # GCC's and macOS's spellings the tests' builds use, and -Werror: clang warns of
    # what the POSIX headers do not.
    for arg in "$@"; do
        case "$arg" in -pthread|-ldl|-Werror) ;; *) args+=("$arg") ;; esac
    done
    # Programs have their extension, which the shell finds them without.
    for (( i = 0; i + 1 < ${#args[@]}; i++ )); do
        if [[ "${args[i]}" == -o && "${args[i+1]}" != *.exe ]]; then args[i+1]+=.exe; fi
    done
    "$llvm_bin/$tool" --target=x86_64-pc-windows-msvc -Xmicrosoft-windows-sys-root "$windows_sysroot" \
        -fms-runtime-lib=static -fuse-ld=lld -I"$ZEBKIT_ROOT/tests/engine/win_compat" -include win_compat.h \
        -D_CRT_SECURE_NO_WARNINGS -D_CRT_NONSTDC_NO_WARNINGS -Wno-unused-command-line-argument "${args[@]}"
}
target_cc() {
    if [[ "$ZEBKIT_TARGET" == windows ]]; then
        windows_compiler clang "$@"
    elif [[ "$ZEBKIT_TARGET" == macos && "$ZEBKIT_HOST" != macos ]]; then
        "$llvm_bin/clang" --target=arm64-apple-macos13 -isysroot "$macos_sdk" -fuse-ld=lld "$@" -Wno-unused-but-set-global
    else
        cc "$@"
    fi
}
target_cxx() {
    if [[ "$ZEBKIT_TARGET" == windows ]]; then
        windows_compiler clang++ "$@"
    elif [[ "$ZEBKIT_TARGET" == macos && "$ZEBKIT_HOST" != macos ]]; then
        "$llvm_bin/clang++" --target=arm64-apple-macos13 -isysroot "$macos_sdk" -fuse-ld=lld "$@" -Wno-unused-but-set-global
    else
        c++ "$@"
    fi
}

# engine_library <package dir>: the file an application links, on the target:
# libzebkit.so.1, or the framework's library on macOS.
engine_library() {
    case "$ZEBKIT_TARGET" in
        macos) echo "$1/Zebkit.framework/Zebkit" ;;
        windows) echo "$1/zebkit.dll" ;;
        *) echo "$1/libzebkit.so.1" ;;
    esac
}

# engine_link_options <package dir>: how a program links the engine. Its rpaths are
# the package unless ZEBKIT_TARGET_RPATHS, for programs that move with it, says.
engine_link_options() {
    local rpath rpaths="${ZEBKIT_TARGET_RPATHS:-$1}" options
    if [[ "$ZEBKIT_TARGET" == windows ]]; then
        # A DLL is found beside the program; no run paths.
        echo "$1/zebkit.dll.lib"
        return
    elif [[ "$ZEBKIT_TARGET" == macos ]]; then
        options="-F$1 -framework Zebkit"
    else
        options="-L$1 -l:libzebkit.so.1"
    fi
    for rpath in $rpaths; do options+=" -Wl,-rpath,$rpath"; done
    echo "$options"
}

# build_against_package <package dir> <output> <source...> [-- cc options]:
# compile a C program that uses the engine, the way an application does:
# the package's headers, its library, and an rpath to it. The system's C
# compiler, as an application's would be, or the target's across systems.
build_against_package() {
    local package="$1" output="$2"
    shift 2
    local sources=() options=()
    while (( $# )) && [[ "$1" != -- ]]; do sources+=("$1"); shift; done
    (( $# )) && shift
    options=("$@")
    mkdir -p "$(dirname "$output")"
    # shellcheck disable=SC2046
    target_cc -std=c11 -O1 -Wall -Werror -I"$package/include" "${sources[@]}" "${options[@]}" \
        $(engine_link_options "$package") -o "$output"
}
