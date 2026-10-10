#!/usr/bin/env bash
# Download the pinned upstream tarball, verify it, extract it into upstream/
# and apply patches/series on top. Re-running is a no-op unless the pin or
# the patch series changed, in which case the tree is rebuilt from scratch.

source "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

tarball="$CACHE_DIR/$(basename "$UPSTREAM_URL")"
stamp="$SRC_DIR/.zebkit-stamp"
series="$PATCHES_DIR/series"

# The stamp captures everything that determines the tree's contents.
patch_list() { [[ -f "$series" ]] && sed -e 's/#.*//' -e '/^\s*$/d' "$series" || true; }
want="$UPSTREAM_SHA256 $(patch_list | while read -r p; do sha256sum "$PATCHES_DIR/$p"; done | sha256sum | cut -d' ' -f1)"

if [[ -f "$stamp" && "$(cat "$stamp")" == "$want" ]]; then
    log "upstream tree is up to date: $SRC_DIR"
    exit 0
fi

mkdir -p "$CACHE_DIR" "$UPSTREAM_DIR"

if [[ ! -f "$tarball" ]] || ! echo "$UPSTREAM_SHA256  $tarball" | sha256sum -c --status; then
    log "downloading $UPSTREAM_URL"
    curl -fL --retry 3 -o "$tarball.part" "$UPSTREAM_URL"
    mv "$tarball.part" "$tarball"
fi
echo "$UPSTREAM_SHA256  $tarball" | sha256sum -c --status || die "checksum mismatch for $tarball"

log "extracting into $SRC_DIR"
rm -rf "$SRC_DIR"
tar -xf "$tarball" -C "$UPSTREAM_DIR"
[[ -d "$SRC_DIR" ]] || die "tarball did not contain $(basename "$SRC_DIR")/"

patch_list | while read -r p; do
    log "applying $p"
    patch -d "$SRC_DIR" -p1 --no-backup-if-mismatch < "$PATCHES_DIR/$p"
done

echo "$want" > "$stamp"
log "ready: $SRC_DIR"
