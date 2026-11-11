#!/usr/bin/env bash
# Time tests/bench/ in the engine, without a window, for one or more
# packages.
# The page runs a handful of JavaScript and DOM workloads and reports
# milliseconds for each; lower is better. Each package runs three times and
# the best time per workload is kept. Run it on an otherwise idle machine.
#
#   scripts/bench.sh slim                     # out/blink/slim/package
#   scripts/bench.sh before=<dir> slim        # a saved package beside it
#
# BENCH_ARGS adds switches to every engine it starts, such as
# --enable-features=..., to time one feature on its own.
#
# A package is what scripts/package.sh assembles. Keeping a copy of one
# before a rebuild is how a change is timed against what it replaces.

source "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

(( $# )) || die "usage: $(basename "$0") <profile>|<label>=<package dir>..."
work="$(mktemp -d)"
trap 'kill "${page_server:-}" 2>/dev/null; rm -r "$work"' EXIT
# On a port that was free, and that this server answers on.
start_page_server "$ZEBKIT_ROOT/tests/bench" "$work/server.log"
port="$page_port"

rows=()
for arg in "$@"; do
    if [[ "$arg" == *=* ]]; then
        label="${arg%%=*}" package="$(caller_path "${arg#*=}")"
    else
        require_profile "$arg"
        label="$arg" package="$OUT_DIR/$arg/package"
    fi
    # The engine library, driven by tools/zebkit-run built against it.
    [[ -f "$package/libzebkit.so.1" ]] || die "no engine in $package; run scripts/package.sh first"
    build_against_package "$package" "$work/run-$label" "$ZEBKIT_ROOT/tools/zebkit-run/zebkit-run.c"
    engine=("$work/run-$label" --headless)
    log "benchmarking $label ($package)"
    for run in 1 2 3; do
        # The shell does not exit on its own: wait for the page's report,
        # then stop it. Each run gets a profile directory of its own. No
        # sandbox: it does not change the times, and build machines and
        # containers often cannot give the engine one.
        url="http://127.0.0.1:$port/index.html?profile=$label&run=$run.$(date +%s)$RANDOM"
        "${engine[@]}" --no-sandbox --user-data-dir="$work/data-$label-$run" \
            ${BENCH_ARGS:-} "$url" > "$work/shell-$label-$run.log" 2>&1 &
        browser_pid=$!
        for (( waited = 0; waited < 120; waited++ )); do
            (( $(grep -c "GET /result?profile=$label&" "$work/server.log") >= run )) && break
            sleep 1
        done
        kill "$browser_pid" 2>/dev/null || true
        wait "$browser_pid" 2>/dev/null || true
    done
    # Lines look like: GET /result?profile=slim&loop=12&objects=34...
    best="$(grep -oE "GET /result\?profile=$label&[^ ]+" "$work/server.log" | sed 's/^[^&]*&//' | awk -F'&' '
        { for (i = 1; i <= NF; i++) { split($i, kv, "="); if (!(kv[1] in min) || kv[2] + 0 < min[kv[1]]) min[kv[1]] = kv[2] + 0; if (NR == 1) order[i] = kv[1] } n = NF }
        END { for (i = 1; i <= n; i++) printf "%s%s=%d", (i > 1 ? " " : ""), order[i], min[order[i]] }')"
    [[ -n "$best" ]] || die "no benchmark report from '$label'; the shell printed: $(tail -5 "$work/shell-$label-1.log")"
    rows+=("$label $best")
done

printf '\n%-12s' package
for kv in ${rows[0]#* }; do printf '%9s' "${kv%%=*}"; done
printf '\n'
for row in "${rows[@]}"; do
    printf '%-12s' "${row%% *}"
    for kv in ${row#* }; do printf '%9s' "${kv#*=}"; done
    printf '\n'
done
