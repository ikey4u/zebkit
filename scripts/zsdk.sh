#!/usr/bin/env bash
# Get zsdk, the SDKs for building macOS, iOS and Windows on Linux, at the
# commit and versions upstream.lock pins.
#
#   scripts/zsdk.sh macos|ios|windows ...   check out those platforms' pinned versions
#
# Into .cache/zsdk, a partial clone with only the pinned directories checked
# out (each version is hundreds of megabytes). ZSDK_DIR, or a link at
# .cache/zsdk, is a checkout of one's own: it is used as it is, and only
# warned about when it is at another commit.

source "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

(( $# )) || die "usage: $(basename "$0") macos|ios|windows ..."
dirs=(scripts)
for platform in "$@"; do
    case "$platform" in
        macos) dirs+=("macos/$ZSDK_MACOS_VERSION") ;;
        ios) dirs+=("ios/$ZSDK_IOS_VERSION") ;;
        windows) dirs+=("windows/$ZSDK_WINDOWS_VERSION") ;;
        *) die "usage: $(basename "$0") macos|ios|windows ..." ;;
    esac
done

if [[ "$ZSDK_DIR" == "$CACHE_DIR/zsdk" && ! -L "$ZSDK_DIR" ]]; then
    if [[ ! -d "$ZSDK_DIR/.git" ]]; then
        log "cloning zsdk"
        rm -rf "$ZSDK_DIR"
        git clone -q --filter=blob:none --no-checkout "$ZSDK_URL" "$ZSDK_DIR"
    fi
    git -C "$ZSDK_DIR" cat-file -e "$ZSDK_COMMIT^{commit}" 2> /dev/null \
        || git -C "$ZSDK_DIR" fetch -q --filter=blob:none origin
    # Whatever it held before, and these.
    mapfile -t held < <(git -C "$ZSDK_DIR" sparse-checkout list 2> /dev/null || true)
    git -C "$ZSDK_DIR" sparse-checkout set --cone "${held[@]}" "${dirs[@]}"
    if [[ "$(git -C "$ZSDK_DIR" rev-parse -q --verify HEAD || true)" != "$ZSDK_COMMIT" ]] \
        || [[ -n "$(git -C "$ZSDK_DIR" status --porcelain --untracked-files=no)" ]]; then
        log "checking out zsdk ${ZSDK_COMMIT:0:12}: ${dirs[*]:1}"
        git -C "$ZSDK_DIR" checkout -q --force "$ZSDK_COMMIT"
    fi
else
    [[ -d "$ZSDK_DIR" ]] || die "ZSDK_DIR is $ZSDK_DIR, which does not exist"
    head="$(git -C "$ZSDK_DIR" rev-parse -q --verify HEAD 2> /dev/null || true)"
    [[ "$head" == "$ZSDK_COMMIT" ]] \
        || log "note: $ZSDK_DIR is at ${head:-no commit}, upstream.lock pins zsdk $ZSDK_COMMIT"
fi

"$ZSDK_DIR/scripts/check.sh" "${dirs[@]:1}" >&2 \
    || die "zsdk in $ZSDK_DIR does not have what upstream.lock pins (above)"
