#!/usr/bin/env bash
# Load every site in assets/WEBSITE-TOP-100.json with one built profile and
# record what tools/pageprobe sees. Results go to out/compat/<profile>/:
# <domain>.json (the probe's report) and <domain>.png (the viewport).
# scripts/compat-report.py then compares two profiles. See docs/archive/webkit/COMPAT.md.
#
#   scripts/compat.sh <profile> [domain...]     # all sites, or just the named ones
#
# COMPAT_JOBS sets how many sites load at once (default 4).

source "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

profile="${1:-}"
require_profile "$profile"
shift

stage_profile "$profile"

build_pageprobe

results="$OUT_DIR/compat/$profile"
mkdir -p "$results"

# probe_site <domain> <url>: one site, one fresh browser process.
probe_site() {
    local domain="$1" url="$2"
    local raw="$results/$domain.raw"
    limited 150 "${run_env[@]}" "$probe" --settle 8 --timeout 90 --snapshot "$raw" "$url" \
        > "$results/$domain.json" 2> "$results/$domain.log" \
        || echo '{"requested":"'"$url"'","probeFailed":true}' > "$results/$domain.json"
    if [[ -s "$raw" ]]; then
        python3 "$ZEBKIT_ROOT/scripts/raw-to-png.py" "$raw" "$results/$domain.png" 2>/dev/null || true
        rm -f "$raw"
    fi
    echo "$domain"
}

sites=()
while IFS= read -r site; do
    sites+=("$site")
done < <(python3 - "$ZEBKIT_ROOT/assets/WEBSITE-TOP-100.json" "$@" <<'PY'
import json, sys
wanted = set(sys.argv[2:])
for site in json.load(open(sys.argv[1]))["sites"]:
    if not wanted or site["domain"] in wanted:
        print(site["domain"], site["url"])
PY
)
(( ${#sites[@]} )) || die "no matching sites"

log "probing ${#sites[@]} sites with profile '$profile'"
jobs_max="${COMPAT_JOBS:-4}"
done_count=0
for entry in "${sites[@]}"; do
    read -r domain url <<< "$entry"
    probe_site "$domain" "$url" > /dev/null &
    # Polled: the shell macOS has cannot wait for whichever ends first.
    while (( $(jobs -rp | wc -l) >= jobs_max )); do
        sleep 0.2
    done
    done_count=$(( done_count + 1 ))
    (( done_count % 10 == 0 )) && log "$done_count of ${#sites[@]} started"
done
wait
log "results in $results"
