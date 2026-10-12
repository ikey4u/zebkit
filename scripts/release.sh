#!/usr/bin/env bash
# Build the release of zebkit for this machine, make the archive of it that
# the Rust crates download, and optionally publish that on GitHub.
#
#   scripts/release.sh              # dist/release, dist/zebkit-<version>-<target>.tar.xz and its checksum
#   scripts/release.sh --publish    # also create the GitHub release v<version> and upload them
#
# A release has one archive for each system. Each is built on its own
# system, with the first form; the checksums all go into engines.txt, the
# archives are brought together in dist/ on one machine, and --publish
# there uploads every archive of this version that engines.txt names. Run
# again later, it adds what the release does not have yet.
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
    Linux-x86_64) target=x86_64-unknown-linux-gnu ;;
    Darwin-arm64) target=aarch64-apple-darwin ;;
    *) die "no release target for $(uname -s)-$(uname -m) yet" ;;
esac

dist="$ZEBKIT_ROOT/dist/release"
"$ZEBKIT_ROOT/scripts/dist.sh" release

name="zebkit-$version-$target"
archive="$ZEBKIT_ROOT/dist/$name.tar.xz"
log "packing $name"
# The same input gives the same archive: fixed order, owner and times.
if [[ "$ZEBKIT_HOST" == macos ]]; then
    # The system's tar has no word for the times, so the files get them;
    # it is kept from adding what Finder knows about each file, and from
    # renaming where the links in the frameworks point (the S).
    find "$dist" -exec touch -h -t 202601010000.00 {} +
    (cd "$dist" && find . | LC_ALL=C sort) > "$archive.list"
    COPYFILE_DISABLE=1 tar -C "$dist" --no-recursion --uid 0 --gid 0 --numeric-owner --no-xattrs --no-mac-metadata \
        -s ",^\.,$name,S" -cf - -T "$archive.list" | xz -6 -T0 > "$archive"
    rm -f "$archive.list"
else
    tar -C "$dist" --sort=name --owner=0 --group=0 --numeric-owner --mtime='2026-01-01 00:00:00Z' \
        --transform "s,^\.,$name," -cf - . | xz -6 -T0 > "$archive"
fi
sum="$(sha256 "$archive" | cut -d' ' -f1)"
echo "$sum  $name.tar.xz" > "$archive.sha256"

# The checksum the crate will insist on for this version and target.
engines="$ZEBKIT_ROOT/rust/zebkit-rs/engines.txt"
{
    grep -E '^#' "$engines" 2>/dev/null || true
    { grep -vE "^#|^$version $target " "$engines" 2>/dev/null || true; echo "$version $target $sum"; } | sort
} > "$engines.new"
mv "$engines.new" "$engines"

log "$archive ($(du -m "$archive" | cut -f1) MiB)"
log "sha256 $sum, recorded in rust/zebkit-rs/engines.txt"

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
