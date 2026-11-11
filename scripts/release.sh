#!/usr/bin/env bash
# Build the release of zebkit for Linux, macOS and Windows, all on this Linux
# machine, make the archives of it that the Rust crates download, and
# optionally publish them on GitHub.
#
#   scripts/release.sh                 # dist/release, dist/release-macos, dist/release-windows, dist/zebkit-<version>-<target>.tar.xz and checksums
#   scripts/release.sh --publish       # also create the GitHub release v<version> and upload them
#   scripts/release.sh --publish-only  # publish what an earlier run built, without building
#
# A release has one archive for each system, all built here on Linux: the
# Linux engine, and the macOS and Windows ones cross-built (docs/MACOS.md,
# docs/WINDOWS.md). Each archive also holds zebkit-run, zebkit-js and
# zebkit-mcp, built here from its package (build_tools below). The
# checksums all go into engines.txt, and --publish uploads every archive of
# this version that engines.txt names. Once the tag v<version> exists (here or
# on origin) and its release has archives, nothing is built or packed for that
# version again, and its lines of engines.txt are never rewritten: the run
# stops before building, bump VERSION first. --publish-only, which builds
# nothing, still adds what the release lacks from dist/; it never replaces an
# archive the release has: one whose bytes differ (by the release's own
# .sha256, or GitHub's digest of the archive when an interrupted upload left
# no .sha256) stops it, since the crates published with that release insist
# on the first one. A version is published once.
#
# "mise run r" and "mise run r:github" are these two.
#
# The build is scripts/dist.sh release, which does nothing when it is up to
# date. The version is the one in VERSION, which is also the version of the
# crates in bindings/rust/, of the Python package and of zebkit-mcp. The archive is dist/release under one top directory. Its
# SHA-256 is written into bindings/rust/zebkit-rs/engines.txt, where the crate's
# build script looks it up, so an archive is published together with the
# commit that names its checksum: build, commit, then publish.
#
# --publish needs the GitHub CLI (gh), logged in with access to the
# repository of the "origin" remote. "mise run r:github" brings gh with it
# (mise.toml); GH_TOKEN in the environment stands in for a login. Crates
# can only download from a release that anyone can read: the repository has
# to be public.
#
# --publish also needs the checks passed on these sources: "mise run check"
# (scripts/check.sh) records them, strict and of out/blink/slim/package as its
# stamp is now. ZEBKIT_RELEASE_UNCHECKED=1 publishes without, and says so.
# An interrupted "gh release create" leaves a draft without a tag: the next run
# says so; delete the draft (gh release delete v<version>) and run again.

source "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

publish=0
publish_only=0
case "${1:-}" in
    "") ;;
    --publish) publish=1 ;;
    --publish-only) publish=1 publish_only=1 ;;
    *) die "usage: $(basename "$0") [--publish|--publish-only]" ;;
esac

version="$(tr -d '[:space:]' < "$ZEBKIT_ROOT/VERSION")"
check_versions

case "$(uname -s)-$(uname -m)" in
    Linux-x86_64) ;;
    Darwin-*) die "the release is built on Linux, the macOS engine with it (docs/MACOS.md)" ;;
    *) die "no release for $(uname -s)-$(uname -m) yet" ;;
esac

engines="$ZEBKIT_ROOT/bindings/rust/zebkit-rs/engines.txt"
rust_targets=(x86_64-unknown-linux-gnu aarch64-apple-darwin x86_64-pc-windows-msvc)
tag="v$version"

# tag_exists: v<version> is a tag here or on origin.
tag_exists() {
    git -C "$ZEBKIT_ROOT" rev-parse -q --verify "refs/tags/$tag" > /dev/null && return 0
    [[ -n "$(git -C "$ZEBKIT_ROOT" ls-remote --tags origin "refs/tags/$tag" 2> /dev/null || true)" ]]
}
# release_has_archives: the GitHub release v<version> holds archives of
# zebkit's; unknown (no gh, or not logged in) counts as yes.
release_has_archives() {
    command -v gh > /dev/null && gh auth status > /dev/null 2>&1 || return 0
    local names
    names="$(gh release view "$tag" --json assets --jq '.assets[].name' 2> /dev/null)" || return 1
    grep -q "^zebkit-$version-.*\.tar\.xz$" <<< "$names"
}
# A version whose tag exists is published: its archives, and the lines of
# engines.txt that name them, are never made again. Only --publish-only, which
# uploads what the release lacks and checks what it has against the release's
# checksums, goes on.
if (( ! publish_only )) && tag_exists; then
    if release_has_archives; then
        die "$tag exists and its release has archives: $version is published; bump VERSION first (--publish-only uploads archives it lacks, the same bytes)"
    fi
    log "note: $tag exists, but its release has no archives of zebkit's: building them"
fi

# checks_passed: scripts/check.sh's record for checked_key says a strict pass,
# of slim's package as out/blink/slim/package.stamp has it now; the reason
# why not on standard error.
checks_passed() {
    local record="$OUT_DIR/check/passed-$checked_key" stamp="$OUT_DIR/slim/package.stamp"
    [[ -f "$record" ]] || { log "no out/blink/check/passed-$checked_key: the checks did not pass on these sources"; return 1; }
    grep -qx 'profile slim' "$record" || { log "the checks recorded were not of the slim profile"; return 1; }
    grep -qx 'strict 1' "$record" || { log "the checks recorded were not strict (ENGINE_TEST_STRICT=0)"; return 1; }
    [[ -s "$stamp" ]] && grep -qxF "package $(cat "$stamp")" "$record" \
        || { log "out/blink/slim/package is not the package the checks tested"; return 1; }
}
unchecked=

# All of it asked for before the build, which is long, and not after it.
if (( ! publish_only )); then
    command -v xz > /dev/null || die "xz is missing; \"mise run r\" brings it (mise.toml), or install it"
    [[ -d "$SDK_DIR/macos/xcode" ]] \
        || die "the macOS engine needs Apple's SDK laid out first: scripts/zsdk-create.sh (docs/MACOS.md)"
    [[ -d "$SDK_DIR/windows" ]] \
        || die "the Windows engine needs MSVC and the Windows SDK laid out first: scripts/zsdk-create.sh (docs/WINDOWS.md)"
    command -v cargo > /dev/null || die "zebkit-mcp needs cargo (Rust)"
    # Captured, then searched: grep -q stops reading, and under pipefail rustup's
    # broken pipe would fail the test.
    installed_targets="$(rustup target list --installed 2> /dev/null || true)"
    for rust_target in "${rust_targets[@]}"; do
        grep -qx "$rust_target" <<< "$installed_targets" \
            || die "zebkit-mcp needs Rust's $rust_target target: rustup target add $rust_target"
    done
fi
if (( publish )); then
    command -v gh > /dev/null || die "publishing needs the GitHub CLI (gh); 'mise run r:github' provides it"
    gh auth status > /dev/null 2>&1 || die "gh is not logged in: run 'mise exec -- gh auth login' once, or set GH_TOKEN"
    # The checks of scripts/check.sh, on the same sources: strict, of slim's
    # package (what dist/release is), as package.sh's stamp has it now.
    checked_key="$(source_tree_key)"
    [[ -n "$checked_key" ]] || die "no key for the sources"
    if ! checks_passed; then
        [[ "${ZEBKIT_RELEASE_UNCHECKED:-}" == 1 ]] \
            || die "no passed checks for these sources: run 'mise run check' first (ZEBKIT_RELEASE_UNCHECKED=1 publishes without)"
        log "publishing WITHOUT the checks (ZEBKIT_RELEASE_UNCHECKED=1)"
        unchecked=1
    fi
    # A release that is there already was made from an earlier commit of this branch.
    if gh release view "$tag" > /dev/null 2>&1; then
        # gh release create makes a draft, uploads, then publishes: an
        # interrupted one leaves the draft, and no tag.
        if [[ "$(gh release view "$tag" --json isDraft --jq .isDraft 2> /dev/null || true)" == true ]]; then
            die "$tag is a draft, left by an interrupted upload: delete it (gh release delete $tag --yes) and run again"
        fi
        # The commit, also of an annotated tag (its peeled line sorts last).
        tagged="$(git -C "$ZEBKIT_ROOT" ls-remote --tags origin "refs/tags/$tag" "refs/tags/$tag^{}")" \
            || die "could not read the tags of origin"
        tagged="$(sort -k2 <<< "$tagged" | tail -1 | cut -f1)"
        [[ -n "$tagged" ]] || die "the release $tag has no tag on origin"
        git -C "$ZEBKIT_ROOT" cat-file -e "$tagged^{commit}" 2> /dev/null \
            || die "$tag's commit $tagged is not here: git fetch --tags origin"
        git -C "$ZEBKIT_ROOT" merge-base --is-ancestor "$tagged" HEAD \
            || die "$tag was made from $tagged, which is not in this branch's history"
    fi
fi

# Packs $1, a release directory, as the archive for target $2, and records
# its checksum: the one the crate will insist on for this version and target.
pack() {
    local dist="$1" target="$2"
    local name="zebkit-$version-$target"
    local archive="$ZEBKIT_ROOT/dist/$name.tar.xz"
    # What the archive is under and what it holds under theirs (package.sh), with
    # the tools' crates appended once (build_tools).
    local notice
    for notice in LICENSE NOTICE THIRD_PARTY_NOTICES.txt; do
        [[ -s "$dist/$notice" ]] || die "no $notice in $dist"
    done
    [[ "$(grep -c '^The Rust crates compiled into zebkit-mcp\.$' "$dist/THIRD_PARTY_NOTICES.txt")" == 1 ]] \
        || die "$dist/THIRD_PARTY_NOTICES.txt does not list zebkit-mcp's crates exactly once"
    log "packing $name"
    # The same input gives the same archive: fixed order, owner and times.
    # S: the links inside a framework keep where they point. What Finder
    # leaves in a directory it has shown is never shipped.
    tar -C "$dist" --exclude=.DS_Store --exclude='._*' --sort=name --owner=0 --group=0 --numeric-owner --mtime='2026-01-01 00:00:00Z' \
        --transform "s,^\.,$name,S" -cf - . | xz -6 -T0 > "$archive"
    local sum
    sum="$(sha256 "$archive" | cut -d' ' -f1)"
    echo "$sum  $name.tar.xz" > "$archive.sha256"
    local before
    before="$(grep -E "^$version $target " "$engines" 2> /dev/null | cut -d' ' -f3 || true)"
    if [[ -n "$before" && "$before" != "$sum" ]]; then
        # The release's own lines are kept (its crates insist on them).
        if tag_exists && release_has_archives; then
            die "$tag is published and $version $target was packed with another checksum: bump VERSION"
        fi
        log "note: $version $target was packed before with another checksum"
    fi
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
    (cd "$ZEBKIT_ROOT/tools/zebkit-mcp" && heavy env "${rust_env[@]}" RUSTFLAGS="$rust_flags" \
        cargo build -q --release --locked --target "$rust_target" -j "$ZEBKIT_JOBS") \
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

if (( ! publish_only )); then
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
fi

(( publish )) || exit 0
[[ -z "$(git -C "$ZEBKIT_ROOT" status --porcelain)" ]] || die "commit bindings/rust/zebkit-rs/engines.txt and everything else first, then run this again: the release is made from a commit"
[[ "$(source_tree_key)" == "$checked_key" ]] || die "the sources changed since the checks; run them again"
# The build (dist.sh) repackages only what changed: the package is still the one tested.
[[ -n "$unchecked" ]] || checks_passed || die "the checks do not cover what is published; run 'mise run check' again"
[[ -n "$(git -C "$ZEBKIT_ROOT" for-each-ref --contains HEAD refs/remotes/origin)" ]] \
    || die "push first: HEAD is on no branch of origin, and the release is made from it"

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

(( ${#files[@]} )) || die "no archive of $version to publish in dist/"

if gh release view "$tag" > /dev/null 2>&1; then
    # Only what the release does not have; what it has must be these bytes.
    # Each asset's name and GitHub's digest of it ("sha256:..."), when it has one.
    assets="$(gh release view "$tag" --json assets --jq '.assets[] | .name + " " + (.digest // "")')" \
        || die "could not list the assets of $tag"
    remote="$(cut -d' ' -f1 <<< "$assets")"
    sums="$(mktemp -d "$TMPDIR/release-sums.XXXXXX")"
    missing=()
    for file in "${files[@]}"; do
        name="$(basename "$file")"
        if ! grep -qxF "$name" <<< "$remote"; then
            missing+=("$file")
            continue
        fi
        archive_name="${name%.sha256}"
        checksum="$archive_name.sha256"
        # What the release's archive is: its .sha256; without one (an upload
        # stopped between the two), GitHub's digest of the archive, or the
        # archive itself, downloaded and hashed.
        if grep -qxF "$checksum" <<< "$remote"; then
            [[ -f "$sums/$checksum" ]] || gh release download "$tag" -p "$checksum" -D "$sums" \
                || die "could not download $checksum from $tag"
            published="$(cut -d' ' -f1 "$sums/$checksum")"
        else
            published="$(awk -v name="$archive_name" '$1 == name && $2 ~ /^sha256:/ { print substr($2, 8) }' <<< "$assets")"
            if [[ -z "$published" ]]; then
                log "$tag has $archive_name but not $checksum: downloading it to hash it"
                [[ -f "$sums/$archive_name" ]] || gh release download "$tag" -p "$archive_name" -D "$sums" \
                    || die "could not download $archive_name from $tag"
                published="$(sha256 "$sums/$archive_name" | cut -d' ' -f1)"
            fi
        fi
        [[ "$published" == "$(cut -d' ' -f1 "$ZEBKIT_ROOT/dist/$checksum")" ]] \
            || die "$tag has another $archive_name than dist/ (by its checksum): a published archive is never replaced; bump VERSION"
    done
    rm -rf "$sums"
    if (( ${#missing[@]} )); then
        log "adding to the release $tag: $(for file in "${missing[@]}"; do basename "$file"; done | paste -sd' ' -)"
        gh release upload "$tag" "${missing[@]}"
    else
        log "the release $tag has all of it already"
    fi
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
