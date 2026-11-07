#!/usr/bin/env bash
# Publish the Rust crates of bindings/rust to crates.io: zebkit-rs,
# zebkit-build and zebkit, at the version in VERSION.
#
#   scripts/publish-crates.sh            # mise run r:crate
#   scripts/publish-crates.sh --dry-run  # every check and the packaging, no upload
#
# The token is crates.io's API token in CARGO_REGISTRY_TOKEN, which cargo
# reads itself; it is never written anywhere. The crates download the engine
# from the GitHub release v<VERSION> and check it against engines.txt, so that
# release is published first (mise run r:github) and engines.txt names its
# three archives.
source "$(dirname "$0")/lib.sh"

dry=0
[[ "${1:-}" == --dry-run ]] && dry=1

crates="$ZEBKIT_ROOT/bindings/rust"
version="$(tr -d '[:space:]' < "$ZEBKIT_ROOT/VERSION")"
crate_version="$(sed -n 's/^version = "\(.*\)"/\1/p' "$crates/Cargo.toml" | head -1)"
[[ "$crate_version" == "$version" ]] || die "VERSION says $version but bindings/rust/Cargo.toml says $crate_version"

for target in x86_64-unknown-linux-gnu aarch64-apple-darwin x86_64-pc-windows-msvc; do
    grep -q "^$version $target " "$crates/zebkit-rs/engines.txt" \
        || die "engines.txt has no $version archive for $target: build and publish the release first (mise run r:github)"
done
if (( ! dry )); then
    [[ -n "${CARGO_REGISTRY_TOKEN:-}" ]] || die "set CARGO_REGISTRY_TOKEN to a crates.io API token"
    [[ -z "$(git -C "$ZEBKIT_ROOT" status --porcelain -- bindings/rust)" ]] \
        || die "bindings/rust has changes not committed: a crate is published from what is committed"
    if command -v gh > /dev/null; then
        gh release view "v$version" > /dev/null 2>&1 \
            || die "no GitHub release v$version: the crates would download an engine that is not there"
    fi
fi

# cargo checks that each packaged crate builds before it uploads it. Packaged,
# zebkit-rs is outside this repository and would download the release's engine;
# a local package of the same version is used for that build instead.
if [[ -z "${ZEBKIT_DIR:-}" ]]; then
    for dir in "$ZEBKIT_ROOT/dist/release" "$ZEBKIT_ROOT/out/blink/slim/package"; do
        [[ -e "$dir/libzebkit.so.1" ]] && { export ZEBKIT_DIR="$dir"; break; }
    done
fi
[[ -n "${ZEBKIT_DIR:-}" ]] || die "no engine to verify the crates against: build one (mise run r) or set ZEBKIT_DIR"

cd "$crates"
if (( dry )); then
    log "packaging zebkit-rs, zebkit-build and zebkit $version (dry run)"
    # Without the token a dry run cannot see what crates.io holds; it packs
    # and builds each crate, the dependents against their local siblings.
    cargo package --workspace --allow-dirty
    log "packed: $crates/target/package"
else
    log "publishing zebkit-rs, zebkit-build and zebkit $version to crates.io"
    cargo publish --workspace
    log "published: https://crates.io/crates/zebkit/$version"
fi
