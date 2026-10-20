# Shared paths and helpers. Source this; do not execute it.

set -euo pipefail

# Work from the physical path, so that the same build tree is not reached
# under two spellings.
ZEBKIT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
cd -P "$ZEBKIT_ROOT"

# Keep host-injected libraries out of the compiler, the tools and the
# browser under test.
unset LD_PRELOAD

# shellcheck source=../upstream.lock
source "$ZEBKIT_ROOT/upstream.lock"

log() { printf '\033[1m==> %s\033[0m\n' "$*" >&2; }
die() { printf 'error: %s\n' "$*" >&2; exit 1; }

CACHE_DIR="$ZEBKIT_ROOT/.cache"
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

# engine_library <package dir>: the file an application links, on this
# system: libzebkit.so.1, or the framework's library on macOS.
engine_library() {
    if [[ "$(uname)" == Darwin ]]; then echo "$1/Zebkit.framework/Zebkit"; else echo "$1/libzebkit.so.1"; fi
}

# engine_link_options <package dir>: how a program links the engine.
engine_link_options() {
    if [[ "$(uname)" == Darwin ]]; then
        echo "-F$1 -framework Zebkit -Wl,-rpath,$1"
    else
        echo "-L$1 -l:libzebkit.so.1 -Wl,-rpath,$1"
    fi
}

# build_against_package <package dir> <output> <source...> [-- cc options]:
# compile a C program that uses the engine, the way an application does:
# the package's headers, its library, and an rpath to it. The system's C
# compiler, as an application's would be.
build_against_package() {
    local package="$1" output="$2"
    shift 2
    local sources=() options=()
    while (( $# )) && [[ "$1" != -- ]]; do sources+=("$1"); shift; done
    (( $# )) && shift
    options=("$@")
    mkdir -p "$(dirname "$output")"
    # shellcheck disable=SC2046
    cc -std=c11 -O1 -Wall -Werror -I"$package/include" "${sources[@]}" "${options[@]}" \
        $(engine_link_options "$package") -o "$output"
}
