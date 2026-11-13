# Shared paths and helpers. Source this; do not execute it.

set -euo pipefail
# macOS's bash (3.2) takes an empty array for an unset variable under -u,
# which bash 4.4 stopped doing; the scripts expand arrays that may be empty.
if (( BASH_VERSINFO[0] < 4 || (BASH_VERSINFO[0] == 4 && BASH_VERSINFO[1] < 4) )); then
    set +u
fi

# Work from the physical path, so that the same build tree is not reached
# under two spellings. Where the script was called from is kept, for the
# paths it is given (caller_path).
ZEBKIT_CALLER_PWD="$PWD"
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

# caller_path <path>: a path given on the command line, as the caller meant it:
# a relative one is against the directory the script was called from, not the
# checkout this file moves to. Absolute: /..., and on Windows C:/..., C:\...
# and \\server\share.
caller_path() {
    case "$1" in
        /*|'' | [A-Za-z]:[\\/]* | \\\\*) printf '%s\n' "$1" ;;
        *) printf '%s\n' "$ZEBKIT_CALLER_PWD/$1" ;;
    esac
}

CACHE_DIR="$ZEBKIT_ROOT/.cache"
# Apple's and Microsoft's SDKs for building on Linux (scripts/zsdk-create.sh):
# never deleted by a clean. ZEBKIT_SDK_DIR, or a link here, shares them between
# checkouts.
SDK_DIR="${ZEBKIT_SDK_DIR:-$CACHE_DIR/sdk}"
UPSTREAM_DIR="$ZEBKIT_ROOT/upstream"
# Packages and reports per profile. The builds themselves are in
# $SRC_DIR/out/<profile>, where Chromium's tools expect them.
# "blink" is left from the switch from WebKit (2026-10-17), when out/ held both
# engines. It stays: the name is in the Rust crates' search for a package
# (bindings/rust), the examples, the docs, the run paths of the programs that
# --build-only bundles for macOS and Windows, and every machine's existing
# packages and check records; renaming it would orphan those for a word.
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

# report_engine <package dir> <profile>: the line each measurement report
# starts with, of what was measured: "Engine: zebkit <version> (Chromium
# <version>), profile <profile>, measured <date>". The versions are the
# package's own, from its NOTICE (package.sh), so that a package made before a
# bump is not reported as the new one; VERSION and upstream.lock's for a package
# without one. scripts/check-reports.py holds the reports to it.
report_engine() {
    local version="" chromium=""
    if [[ -f "$1/NOTICE" ]]; then
        version="$(sed -n '1s/^zebkit \([^ ]*\)$/\1/p' "$1/NOTICE" | tr -d '\r')"
        chromium="$(sed -n 's/^It contains Chromium \([0-9.]*\) .*/\1/p' "$1/NOTICE" | head -1)"
    fi
    [[ -n "$version" ]] || version="$(tr -d '[:space:]' < "$ZEBKIT_ROOT/VERSION")"
    [[ -n "$chromium" ]] || chromium="$CHROMIUM_VERSION"
    printf 'Engine: zebkit %s (Chromium %s), profile %s, measured %s\n' "$version" "$chromium" "$2" "$(date -u +%Y-%m-%d)"
}

# heavy <command...>: a step that loads the machine (a build, a sync): at the
# lowest CPU and I/O priority, one at a time. The lock is ZEBKIT_HEAVY_LOCK
# (default .cache/heavy.lock; point checkouts that share a machine at one
# file), held by the command and its children; a heavy step inside another
# (release.sh, dist.sh, build.sh) runs under the outer one's. On Linux it also
# waits, up to an hour, while the load is above ZEBKIT_HEAVY_MAX_LOAD (30).
# ZEBKIT_JOBS, default 12, is how many jobs the steps run.
ZEBKIT_HEAVY_LOCK="${ZEBKIT_HEAVY_LOCK:-$CACHE_DIR/heavy.lock}"
# ZEBKIT_JOBS_SET: whether it was given (a Mac's build otherwise fits its memory).
ZEBKIT_JOBS_SET="${ZEBKIT_JOBS:+1}"
ZEBKIT_JOBS="${ZEBKIT_JOBS:-12}"
heavy() {
    local priority=(nice -n 19)
    command -v ionice > /dev/null && priority+=(ionice -c3)
    if [[ -n "${ZEBKIT_HEAVY_HELD:-}" ]]; then
        "${priority[@]}" "$@"
        return
    fi
    local load waited=0 max_load="${ZEBKIT_HEAVY_MAX_LOAD:-30}"
    while [[ -r /proc/loadavg ]] && (( waited < 3600 )); do
        load="$(cut -d' ' -f1 /proc/loadavg)"
        (( ${load%.*} < max_load )) && break
        (( waited % 300 == 0 )) && log "the load is $load, above $max_load: waiting before $1"
        sleep 30
        waited=$(( waited + 30 ))
    done
    if ! command -v flock > /dev/null; then
        ZEBKIT_HEAVY_HELD=1 "${priority[@]}" "$@"
        return
    fi
    mkdir -p "$(dirname "$ZEBKIT_HEAVY_LOCK")"
    (
        exec 9> "$ZEBKIT_HEAVY_LOCK"
        flock -n 9 || { log "waiting for another heavy step to end ($ZEBKIT_HEAVY_LOCK)"; flock 9; }
        ZEBKIT_HEAVY_HELD=1 exec "${priority[@]}" "$@"
    )
}

# python3 for the scripts' own Python: isolated (-I, which ignores
# PYTHONDONTWRITEBYTECODE) and writing no .pyc beside the sources (-B).
python_run=(python3 -I -B)

# free_port: a TCP port of 127.0.0.1 that nothing listens on now; whoever binds
# it next has to check that it got it. Without the "\r" a Windows Python ends
# its lines with, which MSYS's bash keeps.
free_port() {
    "${python_run[@]}" -c 'import socket; s = socket.socket(); s.bind(("127.0.0.1", 0)); print(s.getsockname()[1], end="")' \
        | tr -d '\r'
}

# start_page_server <dir> <log> [cert.pem]: tests/serve.py on a port the system
# gives it (0), in the background; sets page_port and page_server. The port is
# read from the server's first line ("serving http://127.0.0.1:<port>"), which
# it prints once it listens.
start_page_server() {
    local waited line serving='^serving https?://127\.0\.0\.1:([0-9]+)$'
    page_port=
    "${python_run[@]}" -u "$ZEBKIT_ROOT/tests/serve.py" "$1" 0 ${3:+"$3"} > "$2" 2>&1 &
    page_server=$!
    for (( waited = 0; waited < 150; waited++ )); do
        line="$(head -n 1 "$2" 2> /dev/null | tr -d '\r' || true)"
        if [[ "$line" =~ $serving ]]; then
            page_port="${BASH_REMATCH[1]}"
            return 0
        fi
        kill -0 "$page_server" 2> /dev/null || break
        sleep 0.1
    done
    kill "$page_server" 2> /dev/null || true
    wait "$page_server" 2> /dev/null || true
    die "the page server did not start: $(tail -3 "$2")"
}

# start_xvfb <fd> <WxHxD>: a private X server on the first free display, from
# Xvfb itself (-displayfd); sets xvfb_display (":N") and xvfb_pid. Keeps file
# descriptor <fd> open on it; close it after killing the server.
start_xvfb() {
    local number
    eval "exec $1< <(Xvfb -displayfd 1 -screen 0 \"\$2\" -nolisten tcp 2> /dev/null)"
    xvfb_pid=$!
    read -r -t 15 -u "$1" number || die "Xvfb did not start"
    xvfb_display=":$number"
}

# node_cmd, npm_cmd: Node.js at mise.toml's pin, or the PATH's without mise.
# The version is named, so that they work from any directory. The bundle of
# engine-test.sh --build-only has no mise.toml: there, on a test machine, the
# PATH's node, never one mise would download.
node_cmd=(node)
npm_cmd=(npm)
if command -v mise > /dev/null && [[ -f "$ZEBKIT_ROOT/mise.toml" ]]; then
    node_version="$(sed -n 's/^node = "\(.*\)"/\1/p' "$ZEBKIT_ROOT/mise.toml" 2> /dev/null || true)"
    node_cmd=(mise exec "node@${node_version:-24}" -- node)
    npm_cmd=(mise exec "node@${node_version:-24}" -- npm)
fi

# npm_has <dir> <package@version...>: those versions are what <dir>/node_modules holds.
npm_has() {
    local dir="$1" spec name version
    shift
    for spec in "$@"; do
        name="${spec%@*}" version="${spec##*@}"
        [[ "$(tr -d '\r' 2> /dev/null < "$dir/node_modules/$name/package.json" \
            | sed -n 's/^[[:space:]]*"version":[[:space:]]*"\([^"]*\)".*$/\1/p' | head -n 1)" == "$version" ]] \
            || return 1
    done
}

# ensure_npm [--ignore-scripts] <dir> <package@version...>: installs them into <dir>
# unless it has them. An install without --save removes what it is not given:
# name everything the directory holds.
ensure_npm() {
    local options=(--silent --no-audit --no-fund --no-save)
    [[ "$1" == --ignore-scripts ]] && { options+=(--ignore-scripts); shift; }
    local dir="$1"
    shift
    npm_has "$dir" "$@" && return 0
    log "installing $* into $dir"
    mkdir -p "$dir"
    (cd "$dir" && "${npm_cmd[@]}" install "${options[@]}" "$@")
}

# What .cache/playwright-tools holds, for the tests, the benchmark and
# scripts/playwright-sources.sh.
playwright_tools=("playwright-core@$PLAYWRIGHT_CORE_VERSION" "esbuild@$PLAYWRIGHT_ESBUILD_VERSION" "yaml@$PLAYWRIGHT_YAML_VERSION")

# check_versions: VERSION, and every manifest that ships with that version
# (the Rust crates, the Python package, zebkit-mcp), the crates' pins of one
# another (zebkit-rs = "=<version>", and zebkit-build's if it is pinned) and
# the Go binding's BindingVersion say the same.
check_versions() {
    local version manifest found crate
    version="$(tr -d '[:space:]' < "$ZEBKIT_ROOT/VERSION")"
    [[ "$version" =~ ^[0-9]+\.[0-9]+\.[0-9]+ ]] || die "VERSION does not hold a version: '$version'"
    for manifest in bindings/rust/Cargo.toml bindings/python/pyproject.toml tools/zebkit-mcp/Cargo.toml; do
        [[ -f "$ZEBKIT_ROOT/$manifest" ]] || continue
        found="$(sed -n 's/^version = "\(.*\)"/\1/p' "$ZEBKIT_ROOT/$manifest" | head -1)"
        [[ "$found" == "$version" ]] || die "VERSION says $version but $manifest says ${found:-nothing}"
    done
    for manifest in bindings/rust/*/Cargo.toml; do
        for crate in zebkit-rs zebkit-build; do
            found="$(sed -n "s/^$crate = .*version = \"=\([^\"]*\)\".*/\1/p" "$manifest" | head -1)"
            [[ -z "$found" || "$found" == "$version" ]] \
                || die "VERSION says $version but ${manifest#"$ZEBKIT_ROOT"/} pins $crate =$found"
        done
    done
    manifest=bindings/golang/zebkit.go
    if [[ -f "$ZEBKIT_ROOT/$manifest" ]]; then
        found="$(sed -n 's/^const BindingVersion = "\(.*\)"/\1/p' "$ZEBKIT_ROOT/$manifest" | head -1)"
        [[ "$found" == "$version" ]] || die "VERSION says $version but $manifest's BindingVersion is ${found:-nothing}"
    fi
}

# source_tree_key: what the checkout holds, tracked or not (ignored files aside),
# as one git tree hash; engines.txt, which a release writes after its checks, is
# left out. The same key is the same sources.
source_tree_key() {
    local index key
    index="$(mktemp "$TMPDIR/tree-key.XXXXXX")"
    rm -f "$index"
    key="$(cd "$ZEBKIT_ROOT" && GIT_INDEX_FILE="$index" git add -A -- . ':(exclude)bindings/rust/zebkit-rs/engines.txt' \
        && GIT_INDEX_FILE="$index" git write-tree)" || { rm -f "$index"; die "no key for the sources: git add failed"; }
    rm -f "$index"
    [[ "$key" =~ ^[0-9a-f]{40,64}$ ]] || die "no key for the sources: git write-tree gave '$key'"
    printf '%s\n' "$key"
}

list_profiles() {
    find "$PROFILES_DIR" -maxdepth 1 -name '*.gn' ! -name '_*' -exec basename {} .gn \; | sort | paste -sd' ' -
}

# limited <seconds> <command...>: run a command, and end it if it is still
# running after that long, killed 10 s later if it ignores that. macOS has no
# timeout(1); an alarm that survives exec does the same.
limited() {
    if command -v timeout >/dev/null; then
        timeout -k 10 "$@"
    else
        perl -e 'alarm shift; exec @ARGV or die "$ARGV[0]: $!\n"' "$@"
    fi
}

# What a program is built for: the system this runs on, or macOS for a profile
# that builds it, cross-compiled here on Linux as the engine is (docs/MACOS.md).
ZEBKIT_TARGET="$ZEBKIT_HOST"
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

# engine_link_options <package dir>: how a program links the engine, as the
# array engine_link (paths with spaces stay one word each). Its rpaths are the
# package unless the array ZEBKIT_TARGET_RPATHS, for programs that move with
# it, names others.
ZEBKIT_TARGET_RPATHS=()
engine_link_options() {
    local rpath rpaths=("$1")
    if [[ "$ZEBKIT_TARGET" == windows ]]; then
        # A DLL is found beside the program; no run paths.
        engine_link=("$1/zebkit.dll.lib")
        return
    elif [[ "$ZEBKIT_TARGET" == macos ]]; then
        engine_link=("-F$1" -framework Zebkit)
    else
        engine_link=("-L$1" -l:libzebkit.so.1)
    fi
    (( ${#ZEBKIT_TARGET_RPATHS[@]} )) && rpaths=("${ZEBKIT_TARGET_RPATHS[@]}")
    for rpath in "${rpaths[@]}"; do engine_link+=("-Wl,-rpath,$rpath"); done
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
    engine_link_options "$package"
    target_cc -std=c11 -O1 -Wall -Werror -I"$package/include" "${sources[@]}" "${options[@]}" \
        "${engine_link[@]}" -o "$output"
}
