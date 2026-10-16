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

log() { printf '\033[1m==> %s\033[0m\n' "$*" >&2; }
die() { printf 'error: %s\n' "$*" >&2; exit 1; }

CACHE_DIR="$ZEBKIT_ROOT/.cache"
UPSTREAM_DIR="$ZEBKIT_ROOT/upstream"
OUT_DIR="$ZEBKIT_ROOT/out"
TOOLCHAIN_DIR="$OUT_DIR/toolchain"
PATCHES_DIR="$ZEBKIT_ROOT/patches"
REPORTS_DIR="$ZEBKIT_ROOT/docs/reports"

# What differs between the hosts. Linux builds WebKit's WPE port with CMake
# from the release tarball. macOS builds WebKit's Mac port with Xcode from
# the same release in git, with its own patch series and its own profiles,
# which are Xcode configuration files.
#
# Windows is not a host: WebKit's Windows port is cross-built on Linux, by
# setting ZEBKIT_TARGET=windows (docs/WINDOWS.md). ZEBKIT_HOST is then
# "windows", and everything of that build is under out/windows.
case "${ZEBKIT_TARGET:-$(uname -s)}" in
    Linux)
        ZEBKIT_HOST=linux
        SRC_DIR="$UPSTREAM_DIR/$UPSTREAM_NAME-$UPSTREAM_VERSION"
        PATCH_SERIES="$PATCHES_DIR/series"
        PROFILES_DIR="$ZEBKIT_ROOT/profiles"
        PROFILE_SUFFIX=cmake
        ;;
    Darwin)
        ZEBKIT_HOST=macos
        SRC_DIR="$UPSTREAM_DIR/webkit-$UPSTREAM_VERSION-macos"
        PATCH_SERIES="$PATCHES_DIR/series.macos"
        PROFILES_DIR="$ZEBKIT_ROOT/profiles/macos"
        PROFILE_SUFFIX=xcconfig
        # The reports of the two systems are of the same names.
        REPORTS_DIR="$ZEBKIT_ROOT/docs/reports/macos"
        ;;
    windows)
        [[ "$(uname -s)" == Linux ]] || die "zebkit for Windows is built on Linux"
        ZEBKIT_HOST=windows
        SRC_DIR="$UPSTREAM_DIR/webkit-$UPSTREAM_VERSION-windows"
        PATCH_SERIES="$PATCHES_DIR/series.windows"
        PROFILES_DIR="$ZEBKIT_ROOT/profiles/windows"
        PROFILE_SUFFIX=cmake
        OUT_DIR="$OUT_DIR/windows"
        REPORTS_DIR="$ZEBKIT_ROOT/docs/reports/windows"
        # The compiler, Microsoft's SDK and vcpkg: scripts/fetch-toolchain-windows.sh.
        WINDOWS_TOOLCHAIN_DIR="$TOOLCHAIN_DIR/windows"
        # The other scripts would do to it what they do on Linux.
        case "$(basename "$0")" in
            fetch-upstream.sh|fetch-toolchain-windows.sh|build.sh|build-windows.sh|wine.sh|cdp.sh) ;;
            *) die "$(basename "$0") is not ported to Windows yet; see docs/WINDOWS.md" ;;
        esac
        ;;
    *) die "zebkit is not ported to ${ZEBKIT_TARGET:-$(uname -s)}" ;;
esac

# linux_only: for the scripts that have no macOS counterpart yet.
linux_only() {
    [[ "$ZEBKIT_HOST" == linux ]] || die "$(basename "$0") is not ported to $ZEBKIT_HOST yet; see docs/"
}

# sha256 [file...]: like sha256sum, which macOS does not have.
sha256() {
    if command -v sha256sum >/dev/null; then sha256sum "$@"; else shasum -a 256 "$@"; fi
}

# require_profile <name>: fail unless the host has a profile of that name.
require_profile() {
    [[ -n "${1:-}" ]] || die "usage: $(basename "$0") <profile>   (one of: $(list_profiles))"
    [[ -f "$PROFILES_DIR/$1.$PROFILE_SUFFIX" ]] || die "unknown profile '$1' (have: $(list_profiles))"
}

list_profiles() {
    find "$PROFILES_DIR" -maxdepth 1 -name "*.$PROFILE_SUFFIX" ! -name '_*' -exec basename {} ".$PROFILE_SUFFIX" \; | sort | paste -sd' ' -
}

# stage_profile <profile>: install a built profile into out/<profile>/stage
# and set build_dir, stage_dir and run_env (the command prefix that lets a
# program find the engine and, where used, the libraries from deps/). On
# macOS it sets products_dir, where the frameworks are, instead of stage_dir.
stage_profile() {
    build_dir="$OUT_DIR/$1"
    stage_dir="$build_dir/stage"

    # For Windows there is nothing to install either, and what was built
    # runs under Wine (scripts/wine.sh), which finds the libraries itself.
    if [[ "$ZEBKIT_HOST" == windows ]]; then
        [[ -f "$build_dir/bin/WebKit2.dll" ]] || die "profile '$1' is not built; run ZEBKIT_TARGET=windows scripts/build.sh $1"
        run_env=("$ZEBKIT_ROOT/scripts/wine.sh" "$1")
        return
    fi

    # On macOS there is nothing to install: the frameworks are used from
    # where Xcode put them, and the engine's helper processes, which launchd
    # starts, are told where through the __XPC_ variables.
    if [[ "$ZEBKIT_HOST" == macos ]]; then
        products_dir="$build_dir/Release"
        [[ -d "$products_dir/WebKit.framework" ]] || die "profile '$1' is not built; run scripts/build.sh $1"
        run_env=(env "DYLD_FRAMEWORK_PATH=$products_dir" "DYLD_LIBRARY_PATH=$products_dir"
                     "__XPC_DYLD_FRAMEWORK_PATH=$products_dir" "__XPC_DYLD_LIBRARY_PATH=$products_dir")
        return
    fi
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

# build_pageprobe: after stage_profile, build tools/pageprobe for the
# profile if it is not up to date, and set probe to it. It is what loads a
# page in the engine without a window and reports what happened.
build_pageprobe() {
    probe="$build_dir/tools/pageprobe"
    local directory="$ZEBKIT_ROOT/tools/pageprobe" source_file engine pc_path file stale=0
    if [[ "$ZEBKIT_HOST" == macos ]]; then
        source_file="$directory/pageprobe.m"
        engine="$products_dir/WebKit.framework/WebKit"
    else
        source_file="$directory/pageprobe.c"
        engine="$stage_dir/lib/pkgconfig/wpe-webkit-2.0.pc"
    fi
    # Rebuilt when its source changes, and when the engine it links against does.
    for file in "$source_file" "$directory/probe-scripts.h" "$engine"; do
        [[ -x "$probe" && ! "$file" -nt "$probe" ]] || stale=1
    done
    (( stale )) || return 0
    log "building pageprobe for $(basename "$build_dir")"
    mkdir -p "$build_dir/tools"
    if [[ "$ZEBKIT_HOST" == macos ]]; then
        clang -fobjc-arc -O1 -Wall -Wno-deprecated-declarations "$source_file" -o "$probe" \
            -F"$products_dir" -framework WebKit -framework Cocoa
    else
        pc_path="$stage_dir/lib/pkgconfig"
        [[ "${run_env[*]}" == *"$OUT_DIR/deps/prefix"* ]] && pc_path+=":$OUT_DIR/deps/prefix/lib/pkgconfig"
        # shellcheck disable=SC2046
        cc -O1 -Wall -Wno-unused-parameter -o "$probe" "$source_file" \
            $(PKG_CONFIG_PATH="$pc_path" pkg-config --cflags --libs wpe-webkit-2.0 wpe-platform-headless-2.0)
    fi
}

# limited <seconds> <command...>: run a command, and end it if it is still
# running after that long. The timeout of coreutils, which macOS lacks.
limited() {
    if command -v timeout > /dev/null; then
        timeout "$@"
    else
        perl -e 'alarm shift; exec @ARGV or die "cannot run $ARGV[0]: $!\n"' "$@"
    fi
}
