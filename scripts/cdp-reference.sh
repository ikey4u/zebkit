#!/usr/bin/env bash
# Run the raw protocol checks (tests/cdp/protocol.py) against Chrome itself.
#
#   scripts/cdp-reference.sh [check names...]
#
# Chrome is the reference implementation of the protocol, so this is what
# says whether the checks are right: one that fails here asserts something
# Chrome does not do. Run it whenever a check is added or changed. Checks of
# things zebkit does on purpose and Chrome does not are skipped.
#
# Needs a Chrome or Chromium: $CHROME, or the first of google-chrome,
# chromium and chromium-browser on the PATH. It is not part of this
# repository and nothing here downloads it.

source "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

chrome="${CHROME:-}"
if [[ -z "$chrome" ]]; then
    for candidate in google-chrome chromium chromium-browser; do
        if command -v "$candidate" > /dev/null; then
            chrome="$candidate"
            break
        fi
    done
fi
[[ -n "$chrome" ]] || die "no Chrome found; set CHROME to its path"

port="${CDP_PORT:-9343}"
page_port="${CDP_PAGE_PORT:-8781}"
other_port=$(( page_port + 1 ))

work="$(mktemp -d)"
pids=()
cleanup() {
    kill "${pids[@]}" 2>/dev/null
    wait 2>/dev/null
    # Chrome's helper processes write to the profile a moment longer.
    for _ in 1 2 3 4 5; do
        rm -rf "$work" 2>/dev/null && return
        sleep 0.5
    done
}
trap cleanup EXIT

python3 -u "$ZEBKIT_ROOT/tests/serve.py" "$ZEBKIT_ROOT/tests/cdp/pages" "$page_port" > "$work/pages.log" 2>&1 &
pids+=($!)
python3 -u "$ZEBKIT_ROOT/tests/serve.py" "$ZEBKIT_ROOT/tests/cdp/pages" "$other_port" > "$work/other.log" 2>&1 &
pids+=($!)

"$chrome" --headless=new --no-sandbox --disable-gpu --disable-extensions --no-first-run --window-size=800,600 \
    --remote-debugging-port="$port" --user-data-dir="$work/profile" "http://127.0.0.1:$page_port/index.html" \
    > "$work/chrome.log" 2>&1 &
pids+=($!)
for _ in $(seq 100); do
    curl -fs -m 2 "http://127.0.0.1:$port/json/version" > /dev/null && break
    sleep 0.2
done
curl -fs -m 2 "http://127.0.0.1:$port/json/version" > /dev/null || die "Chrome's endpoint did not come up on port $port"

log "raw protocol checks against $("$chrome" --version)"
CDP_REFERENCE=1 python3 -I "$ZEBKIT_ROOT/tests/cdp/protocol.py" "$port" "$page_port" "$other_port" "$@" \
    || die "Chrome fails checks that zebkit is held to; the checks are wrong"
log "PASS"
