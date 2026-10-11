# Shared paths and helpers. Source this; do not execute it.

set -euo pipefail

# Work from the physical path. The checkout may be reached through a symlink,
# and CMake rewrites paths to match $PWD, so without this the same build tree
# gets configured under two spellings depending on where it was invoked from.
ZEBKIT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
cd -P "$ZEBKIT_ROOT"

# Keep host-injected libraries out of the compiler, the tools and the browser
# under test; builds and measurements should not depend on what the desktop
# session preloads.
unset LD_PRELOAD

# shellcheck source=../upstream.lock
source "$ZEBKIT_ROOT/upstream.lock"
# shellcheck source=../toolchain.lock
source "$ZEBKIT_ROOT/toolchain.lock"

CACHE_DIR="$ZEBKIT_ROOT/.cache"
UPSTREAM_DIR="$ZEBKIT_ROOT/upstream"
SRC_DIR="$UPSTREAM_DIR/$UPSTREAM_NAME-$UPSTREAM_VERSION"
OUT_DIR="$ZEBKIT_ROOT/out"
TOOLCHAIN_DIR="$OUT_DIR/toolchain"
PROFILES_DIR="$ZEBKIT_ROOT/profiles"
PATCHES_DIR="$ZEBKIT_ROOT/patches"
REPORTS_DIR="$ZEBKIT_ROOT/docs/reports"

log() { printf '\033[1m==> %s\033[0m\n' "$*" >&2; }
die() { printf 'error: %s\n' "$*" >&2; exit 1; }

# require_profile <name>: fail unless profiles/<name>.cmake exists.
require_profile() {
    [[ -n "${1:-}" ]] || die "usage: $(basename "$0") <profile>   (one of: $(list_profiles))"
    [[ -f "$PROFILES_DIR/$1.cmake" ]] || die "unknown profile '$1' (have: $(list_profiles))"
}

list_profiles() {
    find "$PROFILES_DIR" -maxdepth 1 -name '*.cmake' ! -name '_*' -printf '%f\n' | sed 's/\.cmake$//' | sort | paste -sd' '
}

# stage_profile <profile>: install a built profile into out/<profile>/stage
# and set build_dir, stage_dir and run_env (the command prefix that lets its
# MiniBrowser find the engine and, where used, the libraries from deps/).
stage_profile() {
    build_dir="$OUT_DIR/$1"
    stage_dir="$build_dir/stage"
    [[ -x "$build_dir/bin/MiniBrowser" ]] || die "profile '$1' is not built; run scripts/build.sh $1"

    # Refuse to install anywhere but the stage: a build tree configured
    # without scripts/build.sh would otherwise write into the system prefix.
    local dir
    while IFS='=' read -r _ dir; do
        [[ "$(realpath -m "$dir")/" == "$stage_dir/"* ]] \
            || die "$build_dir would install into $dir; re-run scripts/build.sh $1"
    done < <(grep -E '^(CMAKE_INSTALL_PREFIX|LIB_INSTALL_DIR|EXEC_INSTALL_DIR|LIBEXEC_INSTALL_DIR):' "$build_dir/CMakeCache.txt")

    log "installing into $stage_dir"
    cmake --install "$build_dir" > "$build_dir/install.log"

    # Profiles that link scripts/build-deps.sh libraries need them at run
    # time, including the TLS plugin, which replaces the host's GIO modules.
    local deps_prefix="$OUT_DIR/deps/prefix"
    run_env=(env "LD_LIBRARY_PATH=$stage_dir/lib")
    if grep -q "^CMAKE_PREFIX_PATH:.*=$deps_prefix\$" "$build_dir/CMakeCache.txt"; then
        run_env=(env "LD_LIBRARY_PATH=$stage_dir/lib:$deps_prefix/lib" "GIO_MODULE_DIR=$deps_prefix/lib/gio/modules")
    fi
}
