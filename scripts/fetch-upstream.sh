#!/usr/bin/env bash
# Get the pinned upstream source into upstream/ and apply this host's patch
# series on top. Re-running is a no-op unless the pin or the series changed.
#
# Linux: the release tarball, verified, extracted afresh.
# macOS: the same release from WebKit's git repository, because the tarball
#        leaves the Mac port out. The tree is updated in place, so a build
#        after a patch change recompiles only what the change reaches.

source "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

stamp="$SRC_DIR/.zebkit-stamp"

# The stamp captures everything that determines the tree's contents.
patch_list() { [[ -f "$PATCH_SERIES" ]] && sed -e 's/#.*//' -e '/^[[:space:]]*$/d' "$PATCH_SERIES" || true; }
patches_hash="$(patch_list | while read -r p; do sha256 "$PATCHES_DIR/$p"; done | sha256 | cut -d' ' -f1)"
if [[ "$ZEBKIT_HOST" != linux ]]; then
    want="$UPSTREAM_GIT_COMMIT $patches_hash"
else
    want="$UPSTREAM_SHA256 $patches_hash"
fi

if [[ -f "$stamp" && "$(cat "$stamp")" == "$want" ]]; then
    log "upstream tree is up to date: $SRC_DIR"
    exit 0
fi

mkdir -p "$CACHE_DIR" "$UPSTREAM_DIR"

fetch_tarball() {
    local tarball="$CACHE_DIR/$(basename "$UPSTREAM_URL")"
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
}

fetch_git() {
    if [[ ! -d "$SRC_DIR/.git" ]]; then
        git init -q "$SRC_DIR"
        git -C "$SRC_DIR" remote add origin "$UPSTREAM_GIT_URL"
        # WebKit's test suites are most of the repository and nothing here
        # builds or runs them.
        git -C "$SRC_DIR" sparse-checkout set --no-cone '/*' '!/LayoutTests/' '!/PerformanceTests/' \
            '!/JSTests/' '!/WebDriverTests/' '!/ManualTests/' '!/Websites/'
    fi
    if ! git -C "$SRC_DIR" cat-file -e "$UPSTREAM_GIT_COMMIT^{commit}" 2>/dev/null; then
        log "fetching $UPSTREAM_GIT_TAG ($UPSTREAM_GIT_COMMIT) from $UPSTREAM_GIT_URL"
        git -C "$SRC_DIR" fetch --depth 1 --filter=blob:none origin "$UPSTREAM_GIT_COMMIT"
    fi

    # The patched tree is put together in an index of its own, and the
    # working tree is then moved to it. Git writes only the files that
    # differ from what is there, and drops whatever was edited by hand.
    local index="$SRC_DIR/.git/zebkit-index" tree
    rm -f "$index" "$index.lock"
    GIT_INDEX_FILE="$index" git -C "$SRC_DIR" read-tree "$UPSTREAM_GIT_COMMIT"
    patch_list | while read -r p; do
        log "applying $p"
        GIT_INDEX_FILE="$index" git -C "$SRC_DIR" apply --cached "$PATCHES_DIR/$p"
    done
    # --missing-ok: the test suites were never fetched, and without it Git
    # would download all of them just to see that they are there.
    tree="$(GIT_INDEX_FILE="$index" git -C "$SRC_DIR" write-tree --missing-ok)"
    rm -f "$index"
    log "checking out $UPSTREAM_GIT_TAG with the patches"
    git -C "$SRC_DIR" read-tree --reset -u "$tree"
    git -C "$SRC_DIR" clean -qfd

    # zebkit's own sources that are built into the engine (src/cdp) are named
    # in WebKit's source lists by a path inside its tree.
    ln -sfn ../../../../src "$SRC_DIR/Source/WebKit/zebkit"
}

if [[ "$ZEBKIT_HOST" == linux ]]; then fetch_tarball; else fetch_git; fi

echo "$want" > "$stamp"
log "ready: $SRC_DIR"
