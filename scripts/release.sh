#!/usr/bin/env bash
# Build the release of zebkit for this machine, make the archive of it that
# the Rust crates download, and optionally publish that on GitHub.
#
#   scripts/release.sh              # dist/release, dist/zebkit-<version>-<target>.tar.xz and its checksum
#   scripts/release.sh --publish    # also create the GitHub release v<version> and upload them
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
    *) die "no release target for $(uname -s)-$(uname -m) yet" ;;
esac

dist="$ZEBKIT_ROOT/dist/release"
"$ZEBKIT_ROOT/scripts/dist.sh" release

name="zebkit-$version-$target"
archive="$ZEBKIT_ROOT/dist/$name.tar.xz"
log "packing $name"
# The same input gives the same archive: fixed order, owner and times.
tar -C "$dist" --sort=name --owner=0 --group=0 --numeric-owner --mtime='2026-01-01 00:00:00Z' \
    --transform "s,^\.,$name," -cf - . | xz -6 -T0 > "$archive"
sum="$(sha256sum "$archive" | cut -d' ' -f1)"
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
log "publishing $tag"
# A version with a suffix (0.1.0-alpha.1) is a pre-release on GitHub too.
prerelease=()
[[ "$version" == *-* ]] && prerelease=(--prerelease)
gh release create "$tag" "$archive" "$archive.sha256" "${prerelease[@]}" --title "zebkit $version" --target "$(git -C "$ZEBKIT_ROOT" rev-parse HEAD)" \
    --notes "zebkit $version for $target. The Rust crates zebkit and zebkit-rs $version download this archive when they are built."
log "published: $(gh release view "$tag" --json url --jq .url)"
