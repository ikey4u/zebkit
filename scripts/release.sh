#!/usr/bin/env bash
# Build the release of zebkit for this machine, make the archive of it that
# the Rust crates download, and optionally publish that on GitHub.
#
#   scripts/release.sh              # dist/release, dist/release-macos, dist/zebkit-<version>-<target>.tar.xz and checksums
#   scripts/release.sh --publish    # also create the GitHub release v<version> and upload them
#
# A release has one archive for each system, all built here on Linux: the
# Linux engine, and the macOS one cross-built (docs/MACOS.md). The
# checksums all go into engines.txt, and --publish uploads every archive of
# this version that engines.txt names. Run again later, it adds what the
# release does not have yet.
#
# "mise run r" and "mise run r:github" are these two.
#
# The build is scripts/dist.sh release, which does nothing when it is up to
# date. The version is the one in VERSION, which is also the version of the
# crates in rust/. The archive is dist/release under one top directory. Its
# SHA-256 is written into rust/zebkit-rs/engines.txt, where the crate's
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
crate_version="$(sed -n 's/^version = "\(.*\)"/\1/p' "$ZEBKIT_ROOT/rust/Cargo.toml" | head -1)"
[[ "$crate_version" == "$version" ]] || die "VERSION says $version but rust/Cargo.toml says $crate_version"

case "$(uname -s)-$(uname -m)" in
    Linux-x86_64) ;;
    Darwin-*) die "the release is built on Linux, the macOS engine with it (docs/MACOS.md)" ;;
    *) die "no release for $(uname -s)-$(uname -m) yet" ;;
esac

# Asked for before the build, which is long, and not after it.
command -v xz > /dev/null || die "xz is missing; \"mise run r\" brings it (mise.toml), or install it"
[[ -d "$SDK_DIR/macos/xcode" ]] \
    || die "the macOS engine needs Apple's SDK laid out first: scripts/macos-toolchain.sh (docs/MACOS.md)"

engines="$ZEBKIT_ROOT/rust/zebkit-rs/engines.txt"

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
    log "$archive ($(du -m "$archive" | cut -f1) MiB), sha256 $sum, recorded in rust/zebkit-rs/engines.txt"
}

# Linux: dist/release.
"$ZEBKIT_ROOT/scripts/dist.sh" release
pack "$ZEBKIT_ROOT/dist/release" x86_64-unknown-linux-gnu

# macOS on Apple silicon, cross-built here: dist/release-macos.
"$ZEBKIT_ROOT/scripts/build.sh" slim-macos
"$ZEBKIT_ROOT/scripts/package.sh" slim-macos
macos="$ZEBKIT_ROOT/dist/release-macos"
[[ -e "$OUT_DIR/slim-macos/package/Zebkit.framework/Versions/A/Zebkit" ]] || die "packaging left no Zebkit.framework"
mkdir -p "$macos"
find "$macos" -mindepth 1 -delete
cp -a "$OUT_DIR/slim-macos/package/." "$macos/"
log "release build of zebkit for macOS arm64: $macos ($(du -sm "$macos" | cut -f1) MiB)"
pack "$macos" aarch64-apple-darwin

(( publish )) || exit 0
command -v gh > /dev/null || die "publishing needs the GitHub CLI (gh); 'mise run r:github' provides it"
gh auth status > /dev/null 2>&1 || die "gh is not logged in: run 'mise exec -- gh auth login' once, or set GH_TOKEN"
[[ -z "$(git -C "$ZEBKIT_ROOT" status --porcelain)" ]] || die "commit rust/zebkit-rs/engines.txt and everything else first, then run this again: the release is made from a commit"
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
