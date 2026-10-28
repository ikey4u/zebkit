#!/usr/bin/env bash
# Generate src/engine/playwright/ from Playwright at the commit upstream.lock
# pins (PLAYWRIGHT_COMMIT): its injected script and its US keyboard layout,
# which zebkit's automation runs (docs/AUTOMATION.md). The output is checked
# in; run this when the pin moves.
#
#   scripts/playwright-sources.sh [playwright checkout]
#
# Without a checkout, Playwright is fetched at the pin into .cache/playwright.
# Needs Node.js (mise's node@24) and the network once, for esbuild.

source "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

checkout="${1:-$CACHE_DIR/playwright}"
if [[ -z "${1:-}" ]]; then
    if [[ ! -d "$checkout/.git" ]]; then
        log "fetching Playwright $PLAYWRIGHT_COMMIT"
        git init -q "$checkout"
        git -C "$checkout" remote add origin "$PLAYWRIGHT_URL"
    fi
    if [[ "$(git -C "$checkout" rev-parse HEAD 2>/dev/null)" != "$PLAYWRIGHT_COMMIT" ]]; then
        git -C "$checkout" fetch -q --depth 1 origin "$PLAYWRIGHT_COMMIT"
        git -C "$checkout" checkout -q --detach FETCH_HEAD
    fi
fi
head="$(git -C "$checkout" rev-parse HEAD)"
[[ "$head" == "$PLAYWRIGHT_COMMIT" ]] || die "$checkout is at $head, not the pinned $PLAYWRIGHT_COMMIT"

node=(node)
command -v mise > /dev/null && node=(mise exec node@24 -- node)
tools="$CACHE_DIR/playwright-tools"
if [[ ! -d "$tools/node_modules/esbuild" || ! -d "$tools/node_modules/yaml" ]]; then
    log "installing esbuild $PLAYWRIGHT_ESBUILD_VERSION and yaml $PLAYWRIGHT_YAML_VERSION"
    mkdir -p "$tools"
    (cd "$tools" && "${node[@]/%node/npm}" install --silent --no-save \
        "esbuild@$PLAYWRIGHT_ESBUILD_VERSION" "yaml@$PLAYWRIGHT_YAML_VERSION")
fi

"${node[@]}" "$ZEBKIT_ROOT/scripts/playwright-sources.mjs" "$checkout" "$tools/node_modules/esbuild" "$PLAYWRIGHT_COMMIT"
cp "$checkout/LICENSE" "$ZEBKIT_ROOT/src/engine/playwright/LICENSE"
cp "$checkout/NOTICE" "$ZEBKIT_ROOT/src/engine/playwright/NOTICE"
log "src/engine/playwright/ from Playwright $PLAYWRIGHT_COMMIT"
