#!/usr/bin/env bash
# Time tests/bench/ in a headless MiniBrowser for one or more built profiles.
# The page runs a handful of JavaScript and DOM workloads and reports
# milliseconds for each; lower is better. Each profile runs three times and
# the best time per workload is kept. Run it on an otherwise idle machine.
#
#   scripts/bench.sh baseline slim

source "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

(( $# )) || die "usage: $(basename "$0") <profile>...   (one of: $(list_profiles))"
port="${BENCH_PORT:-8775}"

work="$(mktemp -d)"
python3 -u "$ZEBKIT_ROOT/tests/serve.py" "$ZEBKIT_ROOT/tests/bench" "$port" > "$work/server.log" 2>&1 &
server_pid=$!
trap 'kill "$server_pid" 2>/dev/null; rm -r "$work"' EXIT
sleep 1

rows=()
for profile in "$@"; do
    require_profile "$profile"
    stage_profile "$profile"
    log "benchmarking $profile"
    for run in 1 2 3; do
        # MiniBrowser does not exit on its own: wait for the page's report,
        # then stop it.
        "${run_env[@]}" "$build_dir/bin/MiniBrowser" --headless \
            "http://127.0.0.1:$port/index.html?profile=$profile&run=$run.$(date +%s%N)" > /dev/null 2>&1 &
        browser_pid=$!
        for (( waited = 0; waited < 120; waited++ )); do
            (( $(grep -c "GET /result?profile=$profile&" "$work/server.log") >= run )) && break
            sleep 1
        done
        kill "$browser_pid" 2>/dev/null || true
        wait "$browser_pid" 2>/dev/null || true
    done
    # Lines look like: GET /result?profile=slim&loop=12&objects=34...
    best="$(grep -oE "GET /result\?profile=$profile&[^ ]+" "$work/server.log" | sed 's/^[^&]*&//' | awk -F'&' '
        { for (i = 1; i <= NF; i++) { split($i, kv, "="); if (!(kv[1] in min) || kv[2] + 0 < min[kv[1]]) min[kv[1]] = kv[2] + 0; if (NR == 1) order[i] = kv[1] } n = NF }
        END { for (i = 1; i <= n; i++) printf "%s%s=%d", (i > 1 ? " " : ""), order[i], min[order[i]] }')"
    [[ -n "$best" ]] || die "no benchmark report from profile '$profile'"
    rows+=("$profile $best")
done

printf '\n%-12s' profile
for kv in ${rows[0]#* }; do printf '%9s' "${kv%%=*}"; done
printf '\n'
for row in "${rows[@]}"; do
    printf '%-12s' "${row%% *}"
    for kv in ${row#* }; do printf '%9s' "${kv#*=}"; done
    printf '\n'
done
