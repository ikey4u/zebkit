#!/usr/bin/env bash
# Configure and build one profile into out/<profile>/.
#
#   scripts/build.sh <profile> [--fresh] [--configure-only] [ninja args...]

source "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

profile="${1:-}"
require_profile "$profile"
shift

configure_only=0
fresh=()
while [[ "${1:-}" == --* ]]; do
    case "$1" in
        --configure-only) configure_only=1 ;;
        # Discard the CMake cache. Needed after changing which libraries a
        # profile links, because found library paths are cached.
        --fresh) fresh=(--fresh) ;;
        *) break ;;
    esac
    shift
done

[[ -d "$SRC_DIR" ]] || die "no upstream tree; run scripts/fetch-upstream.sh first"

build_dir="$OUT_DIR/$profile"
stage_dir="$build_dir/stage"

# Helper process paths are compiled in from the install directories, so each
# profile installs into its own stage directory (see scripts/smoke.sh). The
# three *_INSTALL_DIR variables are cached as absolute paths on first
# configure and do not follow CMAKE_INSTALL_PREFIX, so set them explicitly.
cmake_args=(-S "$SRC_DIR" -B "$build_dir" -G Ninja -C "$PROFILES_DIR/$profile.cmake"
            -DCMAKE_INSTALL_PREFIX="$stage_dir"
            -DLIB_INSTALL_DIR="$stage_dir/lib"
            -DEXEC_INSTALL_DIR="$stage_dir/bin"
            -DLIBEXEC_INSTALL_DIR="$stage_dir/libexec")

if command -v ccache >/dev/null; then
    cmake_args+=(-DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache)
    export CCACHE_DIR="${CCACHE_DIR:-$CACHE_DIR/ccache}"
fi

log "configuring profile '$profile'"
cmake "${fresh[@]}" "${cmake_args[@]}"

(( configure_only )) && exit 0

# clang occasionally segfaults on this code base (a different file each time),
# and ninja resumes where it stopped, so a failed pass is retried a few times.
# A genuine compile error fails every pass and still stops the build.
attempts="${BUILD_ATTEMPTS:-3}"
for (( attempt = 1; attempt <= attempts; attempt++ )); do
    log "building profile '$profile' (pass $attempt of $attempts)"
    status=0
    ninja -C "$build_dir" "$@" || status=$?
    (( status == 0 )) && exit 0
    # Killed by a signal: someone stopped the build, so do not start it again.
    (( status > 128 )) && die "build of profile '$profile' was interrupted"
done
die "build of profile '$profile' failed after $attempts passes"
