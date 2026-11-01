#!/usr/bin/env bash
# Set up the Linux machine to build zebkit for Windows (profiles/slim-windows.gn),
# as Chromium's own builders compile for Windows on Linux: Chromium's clang-cl
# and lld-link target Windows; what only Microsoft has is MSVC's C++ library
# and the Windows SDK.
#
#   scripts/windows-toolchain.sh          fetch and lay out what is missing, and link it in
#   scripts/windows-toolchain.sh --link   only link in what is there
#   scripts/windows-toolchain.sh --no-link   fetch and lay out, leaving the tree alone
#
# Running it means accepting Microsoft's licence terms for Visual Studio and
# the Windows SDK. Those files stay on this machine, in $SDK_DIR/windows
# (scripts/lib.sh), which nothing cleans: never in the repository, nor in a package.
#
# From Microsoft, by SHA-256 (scripts/windows-toolchain.py): MSVC and the DIA
# SDK from Visual Studio's manifest, the SDK and the debuggers from NuGet.
# The rest comes from where Chromium's DEPS says, at the versions it pins, for
# a checkout that builds Windows on Linux: clang's runtime library for Windows,
# the resource compiler for Linux, Windows' PGO profile, and the libraries only
# Windows builds use. .gclient keeps target_os = ["linux"]: a sync for Windows
# would fetch Chrome's updaters and more, and hooks that reset nothing here are
# all that is needed. All of it is linked into the tree;
# scripts/fetch-upstream.sh links it again after a sync.

source "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

windows="$SDK_DIR/windows"
[[ -f "$SRC_DIR/DEPS" ]] || die "no upstream tree; run scripts/fetch-upstream.sh first"
[[ "$ZEBKIT_HOST" == linux ]] || die "this sets up a Linux machine"

# What DEPS pins for a Windows checkout and the build asks for.
read_pins() {
    (cd "$SRC_DIR" && python3 -I -c '
g = {"Var": lambda name: "{%s}" % name, "Str": str}
exec(open("DEPS").read(), g)
deps, chromium = g["deps"], g["vars"]["chromium_git"]
for path in ("third_party/microsoft_dxheaders/src", "third_party/microsoft_webauthn/src",
             "third_party/microsoft_multimedia_redirection/src"):
    entry = deps["src/" + path]
    url = entry["url"] if isinstance(entry, dict) else entry
    repository, revision = url.replace("{chromium_git}", chromium).rsplit("@", 1)
    print("git", path, repository, revision)
package = deps["src/third_party/wtl"]["packages"][0]
print("cipd", "third_party/wtl", package["package"], package["version"])
for item in deps["src/third_party/llvm-build/Release+Asserts"]["objects"]:
    if "win-runtime-library" in item["object_name"]:
        print("gcs", item["object_name"], item["sha256sum"])
')
}

fetch_pinned() {
    local ensure="$windows/cipd.ensure"
    mkdir -p "$windows/deps"
    : > "$ensure"
    while read -r kind a b c; do
        case "$kind" in
            git)
                local dir="$windows/deps/$(echo "$a" | tr / _)"
                if [[ "$(git -C "$dir" rev-parse HEAD 2>/dev/null || true)" != "$c" ]]; then
                    log "fetching $a"
                    rm -rf "$dir"
                    git init -q "$dir"
                    git -C "$dir" fetch -q --depth 1 "$b" "$c"
                    git -C "$dir" checkout -q FETCH_HEAD
                fi
                ;;
            cipd) printf '@Subdir %s\n%s %s\n' "$(echo "$a" | tr / _)" "$b" "$c" >> "$ensure" ;;
            gcs)
                if [[ ! -d "$windows/runtime" ]]; then
                    log "fetching $a"
                    curl -sSfLo "$windows/runtime.tar.xz" "https://commondatastorage.googleapis.com/chromium-browser-clang/$a"
                    [[ "$(sha256 "$windows/runtime.tar.xz" | cut -d' ' -f1)" == "$b" ]] || die "$a: wrong SHA-256"
                    mkdir -p "$windows/runtime"
                    tar -xJf "$windows/runtime.tar.xz" -C "$windows/runtime"
                    rm "$windows/runtime.tar.xz"
                fi
                ;;
        esac
    done <<< "$(read_pins)"
    log "fetching what CIPD has for Windows"
    cipd ensure -root "$windows/cipd" -ensure-file "$ensure" >/dev/null

    # The resource compiler for Linux, by the SHA-1 Chromium records, as its
    # rc_linux hook fetches it.
    local rc_sha1
    rc_sha1="$(cat "$SRC_DIR/build/toolchain/win/rc/linux64/rc.sha1")"
    if [[ ! -x "$windows/rc" ]]; then
        log "fetching the resource compiler"
        curl -sSfLo "$windows/rc.partial" "https://commondatastorage.googleapis.com/chromium-browser-clang/rc/$rc_sha1"
        [[ "$(sha1sum "$windows/rc.partial" | cut -d' ' -f1)" == "$rc_sha1" ]] || die "rc: wrong SHA-1"
        chmod +x "$windows/rc.partial"
        mv "$windows/rc.partial" "$windows/rc"
    fi

    # Windows' PGO profile. Not with tools/update_pgo_profiles.py, which also
    # deletes the profiles of other systems that it thinks are unused.
    local profile
    profile="$(cat "$SRC_DIR/chrome/build/win64.pgo.txt")"
    if [[ ! -f "$windows/pgo/$profile" ]]; then
        log "fetching $profile"
        mkdir -p "$windows/pgo"
        curl -sSfLo "$windows/pgo/$profile.partial" \
            "https://storage.googleapis.com/chromium-optimization-profiles/pgo_profiles/$profile"
        mv "$windows/pgo/$profile.partial" "$windows/pgo/$profile"
    fi
}

# link_dir <cache dir> <path in the tree>: the empty directory the checkout
# left there (a dependency not fetched for Linux) becomes the link.
link_dir() {
    local target="$SRC_DIR/$2"
    if [[ -d "$target" && ! -L "$target" ]]; then
        rmdir "$target" || die "$2 is not empty; leaving it"
    fi
    mkdir -p "$(dirname "$target")"
    ln -sfn "$1" "$target"
}

link_in() {
    [[ -f "$windows/win_toolchain.json" ]] || die "no toolchain in $windows; run $(basename "$0") without --link"
    ln -sfn "$windows/win_toolchain.json" "$SRC_DIR/build/win_toolchain.json"
    ln -sfn "$windows/rc" "$SRC_DIR/build/toolchain/win/rc/linux64/rc"
    local profile
    profile="$(cat "$SRC_DIR/chrome/build/win64.pgo.txt")"
    mkdir -p "$SRC_DIR/chrome/build/pgo_profiles"
    ln -sfn "$windows/pgo/$profile" "$SRC_DIR/chrome/build/pgo_profiles/$profile"
    while read -r kind a _; do
        case "$kind" in
            git) link_dir "$windows/deps/$(echo "$a" | tr / _)" "$a" ;;
            # WTL's package puts its headers beside files of Chromium's own.
            cipd) link_dir "$windows/cipd/$(echo "$a" | tr / _)/include" "$a/include" ;;
        esac
    done <<< "$(read_pins)"
    # With their times, and only what is not there yet, as for macOS: a
    # compiler's runtime with new times is a new toolchain to the build.
    cp -a -u "$windows/runtime/." "$SRC_DIR/third_party/llvm-build/Release+Asserts/"
}

if [[ "${1:-}" == --link ]]; then
    link_in
    exit 0
fi

mkdir -p "$windows"
python3 -I "$ZEBKIT_ROOT/scripts/windows-toolchain.py" "$windows" "$SRC_DIR"
fetch_pinned
[[ "${1:-}" == --no-link ]] || link_in
log "$(tr '\n' ';' < "$windows/VERSIONS")"
log "ready to build profiles/slim-windows.gn"
