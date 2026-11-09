#!/usr/bin/env bash
# Build the release of zebkit for this machine, make the archive of it that
# the Rust crates download, and optionally publish that on GitHub.
#
#   scripts/release.sh              # dist/release, dist/release-macos, dist/release-windows, dist/zebkit-<version>-<target>.tar.xz and checksums
#   scripts/release.sh --publish    # also create the GitHub release v<version> and upload them
#
# A release has one archive for each system, all built here on Linux: the
# Linux engine, and the macOS and Windows ones cross-built (docs/MACOS.md,
# docs/WINDOWS.md). Each archive also holds zebkit-run, zebkit-js and
# zebkit-mcp, built here from its package (build_tools below). The
# checksums all go into engines.txt, and --publish uploads every archive of
# this version that engines.txt names. Run again later, it adds what the
# release does not have yet.
#
# "mise run r" and "mise run r:github" are these two.
#
# The build is scripts/dist.sh release, which does nothing when it is up to
# date. The version is the one in VERSION, which is also the version of the
# crates in rust/. The archive is dist/release under one top directory. Its
# SHA-256 is written into bindings/rust/zebkit-rs/engines.txt, where the crate's
# build script looks it up, so an archive is published together with the
# commit that names its checksum: build, commit, then publish.
#
# --publish needs the GitHub CLI (gh), logged in with access to the
# repository of the "origin" remote. "mise run r:github" brings gh with it
# (mise.toml); GH_TOKEN in the environment stands in for a login. Crates
# can only download from a release that anyone can read: the repository has
# to be public.

source "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

publish=0
[[ "${1:-}" == --publish ]] && publish=1

version="$(tr -d '[:space:]' < "$ZEBKIT_ROOT/VERSION")"
[[ "$version" =~ ^[0-9]+\.[0-9]+\.[0-9]+ ]] || die "VERSION does not hold a version: '$version'"
crate_version="$(sed -n 's/^version = "\(.*\)"/\1/p' "$ZEBKIT_ROOT/bindings/rust/Cargo.toml" | head -1)"
[[ "$crate_version" == "$version" ]] || die "VERSION says $version but bindings/rust/Cargo.toml says $crate_version"

case "$(uname -s)-$(uname -m)" in
    Linux-x86_64) ;;
    Darwin-*) die "the release is built on Linux, the macOS engine with it (docs/MACOS.md)" ;;
    *) die "no release for $(uname -s)-$(uname -m) yet" ;;
esac

# Asked for before the build, which is long, and not after it.
command -v xz > /dev/null || die "xz is missing; \"mise run r\" brings it (mise.toml), or install it"
[[ -d "$SDK_DIR/macos/xcode" ]] \
    || die "the macOS engine needs Apple's SDK laid out first: scripts/macos-toolchain.sh (docs/MACOS.md)"
[[ -d "$SDK_DIR/windows" ]] \
    || die "the Windows engine needs MSVC and the Windows SDK laid out first: scripts/windows-toolchain.sh (docs/WINDOWS.md)"

engines="$ZEBKIT_ROOT/bindings/rust/zebkit-rs/engines.txt"

# Packs $1, a release directory, as the archive for target $2, and records
# its checksum: the one the crate will insist on for this version and target.
pack() {
    local dist="$1" target="$2"
    local name="zebkit-$version-$target"
    local archive="$ZEBKIT_ROOT/dist/$name.tar.xz"
    log "packing $name"
    # The same input gives the same archive: fixed order, owner and times.
    # S: the links inside a framework keep where they point. What Finder
    # leaves in a directory it has shown is never shipped.
    tar -C "$dist" --exclude=.DS_Store --exclude='._*' --sort=name --owner=0 --group=0 --numeric-owner --mtime='2026-01-01 00:00:00Z' \
        --transform "s,^\.,$name,S" -cf - . | xz -6 -T0 > "$archive"
    local sum
    sum="$(sha256 "$archive" | cut -d' ' -f1)"
    echo "$sum  $name.tar.xz" > "$archive.sha256"
    {
        grep -E '^#' "$engines" 2>/dev/null || true
        { grep -vE "^#|^$version $target " "$engines" 2>/dev/null || true; echo "$version $target $sum"; } | sort
    } > "$engines.new"
    mv "$engines.new" "$engines"
    log "$archive ($(du -m "$archive" | cut -f1) MiB), sha256 $sum, recorded in bindings/rust/zebkit-rs/engines.txt"
}

# The tools README.md tells users to run, built against the release directory
# $1 for system $2 and put into it beside the engine, so that they run from the
# unpacked archive as they are: zebkit-run and zebkit-js (C), zebkit-mcp (Rust,
# at --release with its lock file). Linux: a run path of $ORIGIN, glibc 2.17's
# symbols at most (CentOS 7's, with Chromium's clang). macOS: of
# @loader_path, to Zebkit.framework beside them. Windows: zebkit.dll is found
# beside them, and they are the engine's sandboxed children (docs/WINDOWS.md).
build_tools() {
    local dist="$1" system="$2" exe="" rust_target tool cc link rust_linker rust_flags
    local work="$OUT_DIR/release-tools"
    mkdir -p "$work"
    # Where the sources and crates were is nobody's business in a shipped program.
    rust_flags="--remap-path-prefix=$ZEBKIT_ROOT=zebkit --remap-path-prefix=${CARGO_HOME:-$HOME/.cargo}=cargo"
    case "$system" in
        linux)
            # CentOS 7's glibc 2.17 and libgcc: a newer glibc gives Rust's std
            # versioned weak references (getrandom, GLIBC_2.25) that glibc 2.17's
            # dynamic linker refuses.
            local root="$OUT_DIR/glibc-2.17/root"
            [[ -f "$root/.ready" ]] || "$ZEBKIT_ROOT/scripts/glibc-test.sh" --root
            llvm_bin="$SRC_DIR/third_party/llvm-build/Release+Asserts/bin"
            cc=("$llvm_bin/clang" --target=x86_64-linux-gnu --sysroot="$root" --gcc-toolchain="$root/usr" -fuse-ld=lld)
            link=(-L"$dist" -l:libzebkit.so.1 '-Wl,-rpath,$ORIGIN' -s)
            rust_target=x86_64-unknown-linux-gnu
            ;;
        macos)
            set_target_for_profile slim-macos
            cc=("$llvm_bin/clang" --target=arm64-apple-macos13 -isysroot "$macos_sdk" -fuse-ld=lld)
            link=(-F"$dist" -framework Zebkit -Wl,-rpath,@loader_path -Wl,-S -Wl,-x)
            rust_target=aarch64-apple-darwin
            ;;
        windows)
            set_target_for_profile slim-windows
            cc=("$llvm_bin/clang" --target=x86_64-pc-windows-msvc -Xmicrosoft-windows-sys-root "$windows_sysroot"
                -fms-runtime-lib=static -fuse-ld=lld -D_CRT_SECURE_NO_WARNINGS)
            # /Brepro: the link's time is not written into the program, a hash of
            # it is, so that the same tree gives the same archive.
            link=(-DZEBKIT_WIN_SANDBOX "$dist/zebkit.dll.lib" "$dist/zebkit_sandbox.lib" -Wl,/STACK:0x800000 -Wl,/Brepro)
            rust_target=x86_64-pc-windows-msvc
            exe=.exe
            [[ -f "$dist/zebkit_sandbox.lib" ]] || die "no zebkit_sandbox.lib in $dist for the tools"
            ;;
    esac
    for tool in zebkit-run zebkit-js; do
        log "building $tool for $system"
        "${cc[@]}" -std=c11 -O2 -Wall -Werror -I"$dist/include" "$ZEBKIT_ROOT/tools/$tool/$tool.c" "${link[@]}" -o "$dist/$tool$exe" \
            || die "$tool did not build for $system"
    done

    log "building zebkit-mcp for $system"
    command -v cargo > /dev/null || die "zebkit-mcp needs cargo (Rust)"
    rustup target list --installed 2> /dev/null | grep -qx "$rust_target" \
        || die "zebkit-mcp needs Rust's $rust_target target: rustup target add $rust_target"
    local rust_env=(ZEBKIT_DIR="$dist" ZEBKIT_MCP_SHIPPED=1 CARGO_TARGET_DIR="$work/target")
    case "$system" in
        windows)
            rust_env+=(CARGO_TARGET_X86_64_PC_WINDOWS_MSVC_LINKER="$llvm_bin/lld-link")
            rust_flags+=" -C target-feature=+crt-static -C link-arg=/winsysroot:$windows_sysroot -C link-arg=/Brepro -C link-arg=/DEBUG:NONE"
            ;;
        *)
            # Named for what it runs: cargo links again when that changes.
            local wrapper
            wrapper="$(printf '#!/bin/sh\nexec %s "$@"\n' "$(printf '"%s" ' "${cc[@]}")")"
            rust_linker="$work/linker-$system-$(sha256 <<< "$wrapper" | cut -c1-12)"
            printf '%s\n' "$wrapper" > "$rust_linker"
            chmod +x "$rust_linker"
            local upper
            upper="$(tr 'a-z-' 'A-Z_' <<< "$rust_target")"
            rust_env+=("CARGO_TARGET_${upper}_LINKER=$rust_linker")
            [[ "$system" == macos ]] && rust_env+=(SDKROOT="$macos_sdk" MACOSX_DEPLOYMENT_TARGET=13.0)
            ;;
    esac
    (cd "$ZEBKIT_ROOT/tools/zebkit-mcp" && env "${rust_env[@]}" RUSTFLAGS="$rust_flags" \
        cargo build -q --release --locked --target "$rust_target" -j "${ZEBKIT_JOBS:-12}") \
        || die "zebkit-mcp did not build for $system"
    cp "$work/target/$rust_target/release/zebkit-mcp$exe" "$dist/zebkit-mcp$exe"
    # The crates zebkit-mcp is made of, after the engine's notices (package.sh).
    [[ -f "$dist/THIRD_PARTY_NOTICES.txt" ]] || die "no THIRD_PARTY_NOTICES.txt in $dist"
    python3 -I "$ZEBKIT_ROOT/scripts/third-party-notices.py" crates "$ZEBKIT_ROOT/tools/zebkit-mcp/Cargo.toml" \
        "$rust_target" zebkit-mcp "$work/crates-$system.txt" >&2 || die "no notices for zebkit-mcp's crates"
    { printf '\n'; cat "$work/crates-$system.txt"; } >> "$dist/THIRD_PARTY_NOTICES.txt"

    for tool in zebkit-run zebkit-js zebkit-mcp; do
        # This machine's paths stay here.
        if grep -aqF -e "$ZEBKIT_ROOT" -e "$HOME/" "$dist/$tool$exe"; then
            die "$tool for $system names a path of this machine's: $(grep -aoF -e "$ZEBKIT_ROOT" -e "$HOME/" "$dist/$tool$exe" | sort -u | paste -sd' ' -)"
        fi
        if [[ "$system" == linux ]]; then
            local newest
            newest="$(objdump -T "$dist/$tool" | grep -o 'GLIBC_[0-9.]*' | sed 's/GLIBC_//' | sort -uV | tail -1)"
            [[ "$(printf '%s\n' 2.17 "$newest" | sort -V | tail -1)" == 2.17 ]] \
                || die "$tool needs glibc $newest, newer than 2.17"
        fi
    done
    log "tools for $system: $(cd "$dist" && du -ch zebkit-run$exe zebkit-js$exe zebkit-mcp$exe | tail -1 | cut -f1)"
}

# Linux: dist/release.
"$ZEBKIT_ROOT/scripts/dist.sh" release
build_tools "$ZEBKIT_ROOT/dist/release" linux
pack "$ZEBKIT_ROOT/dist/release" x86_64-unknown-linux-gnu

# macOS on Apple silicon, cross-built here: dist/release-macos.
"$ZEBKIT_ROOT/scripts/build.sh" slim-macos
"$ZEBKIT_ROOT/scripts/package.sh" slim-macos
macos="$ZEBKIT_ROOT/dist/release-macos"
[[ -e "$OUT_DIR/slim-macos/package/Zebkit.framework/Versions/A/Zebkit" ]] || die "packaging left no Zebkit.framework"
mkdir -p "$macos"
find "$macos" -mindepth 1 -delete
cp -a "$OUT_DIR/slim-macos/package/." "$macos/"
build_tools "$macos" macos
log "release build of zebkit for macOS arm64: $macos ($(du -sm "$macos" | cut -f1) MiB)"
pack "$macos" aarch64-apple-darwin

# Windows x64, cross-built here: dist/release-windows. zebkit_sandbox.lib is
# in it, for the application's executable (docs/WINDOWS.md).
"$ZEBKIT_ROOT/scripts/build.sh" slim-windows
"$ZEBKIT_ROOT/scripts/build.sh" slim-windows-sandbox zebkit/engine:zebkit_sandbox
"$ZEBKIT_ROOT/scripts/package.sh" slim-windows
windows="$ZEBKIT_ROOT/dist/release-windows"
[[ -e "$OUT_DIR/slim-windows/package/zebkit.dll" ]] || die "packaging left no zebkit.dll"
mkdir -p "$windows"
find "$windows" -mindepth 1 -delete
cp -a "$OUT_DIR/slim-windows/package/." "$windows/"
build_tools "$windows" windows
log "release build of zebkit for Windows x64: $windows ($(du -sm "$windows" | cut -f1) MiB)"
pack "$windows" x86_64-pc-windows-msvc

(( publish )) || exit 0
command -v gh > /dev/null || die "publishing needs the GitHub CLI (gh); 'mise run r:github' provides it"
gh auth status > /dev/null 2>&1 || die "gh is not logged in: run 'mise exec -- gh auth login' once, or set GH_TOKEN"
[[ -z "$(git -C "$ZEBKIT_ROOT" status --porcelain)" ]] || die "commit bindings/rust/zebkit-rs/engines.txt and everything else first, then run this again: the release is made from a commit"
tag="v$version"

# Every archive of this version that is here and is the one engines.txt
# names. The one just built is among them.
files=()
while read -r _ other sum; do
    file="$ZEBKIT_ROOT/dist/zebkit-$version-$other.tar.xz"
    if [[ ! -f "$file" ]]; then
        log "no archive for $other in dist/: it is not uploaded"
    elif [[ "$(sha256 "$file" | cut -d' ' -f1)" != "$sum" ]]; then
        die "$file is not the archive engines.txt names for $other"
    else
        echo "$sum  $(basename "$file")" > "$file.sha256"
        files+=("$file" "$file.sha256")
    fi
done < <(grep -E "^$version " "$engines")

if gh release view "$tag" > /dev/null 2>&1; then
    log "adding to the release $tag"
    gh release upload "$tag" "${files[@]}" --clobber
else
    log "publishing $tag"
    # A version with a suffix (0.1.0-alpha.1) is a pre-release on GitHub too.
    prerelease=()
    [[ "$version" == *-* ]] && prerelease=(--prerelease)
    targets="$(grep -E "^$version " "$engines" | cut -d' ' -f2 | paste -sd' ' - | sed 's/ /, /g')"
    gh release create "$tag" "${files[@]}" ${prerelease[@]+"${prerelease[@]}"} --title "zebkit $version" --target "$(git -C "$ZEBKIT_ROOT" rev-parse HEAD)" \
        --notes "zebkit $version for $targets. The Rust crates zebkit and zebkit-rs $version download these archives when they are built."
fi
log "published: $(gh release view "$tag" --json url --jq .url)"
