#!/usr/bin/env bash
# The same interface, a list of rows, as native widgets (zebkit.ui) and as
# a web page in a view, run by zebkit-js against a package: the time until
# it is laid out, the time to update every row, and the memory of all the
# engine's processes (PSS, which counts shared pages once).
#
#   scripts/ui-bench.sh <profile> [rows]

source "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

profile="${1:-}"
require_profile "$profile"
rows="${2:-1000}"
package="$OUT_DIR/$profile/package"
build_against_package "$package" "$OUT_DIR/tools/zebkit-js" "$ZEBKIT_ROOT/tools/zebkit-js/zebkit-js.c"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

# The PSS of a process and its descendants, in MiB.
pss() {
    local pids=("$1") all=() pid
    while (( ${#pids[@]} )); do
        pid="${pids[0]}"; pids=("${pids[@]:1}"); all+=("$pid")
        pids+=($(cat /proc/"$pid"/task/*/children 2>/dev/null || true))
    done
    for pid in "${all[@]}"; do
        awk '/^Pss:/ { print $2 }' /proc/"$pid"/smaps_rollup 2>/dev/null || true
    done | awk '{ s += $1 } END { printf "%.0f", s / 1024 }'
}

printf '%-8s %10s %10s %10s %10s\n' kind "shown ms" "update ms" "PSS MiB" processes
for kind in native web; do
    best=""
    for run in 1 2 3; do
        "$OUT_DIR/tools/zebkit-js" --headless --no-sandbox --user-data-dir="$work/$kind-$run" \
            "$ZEBKIT_ROOT/tests/bench/ui/$kind.mjs" "$rows" > "$work/$kind.out" 2> /dev/null &
        pid=$!
        for (( i = 0; i < 300; i++ )); do
            grep -q '^RESULT' "$work/$kind.out" && break
            sleep 0.1
        done
        sleep 2  # settled
        result="$(grep '^RESULT' "$work/$kind.out" | head -1 | cut -d' ' -f2)"
        memory="$(pss "$pid")"
        count="$(pgrep -c -P "$pid" || true)"
        kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null || true
        [[ -n "$result" ]] || die "$kind printed no result"
        shown="$(python3 -c "import json,sys; print(json.loads(sys.argv[1])['shown'])" "$result")"
        updated="$(python3 -c "import json,sys; print(json.loads(sys.argv[1])['updated'])" "$result")"
        line="$shown $updated $memory $count"
        if [[ -z "$best" || "$shown" -lt "${best%% *}" ]]; then best="$line"; fi
    done
    read -r shown updated memory count <<< "$best"
    printf '%-8s %10s %10s %10s %10s\n' "$kind" "$shown" "$updated" "$memory" "$((count + 1))"
done
