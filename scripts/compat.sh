#!/usr/bin/env bash
# Load every site in assets/WEBSITE-TOP-100.json with one built profile and
# record what tools/pageprobe sees. Results go to out/compat/<profile>/:
# <domain>.json (the probe's report) and <domain>.png (the viewport).
# scripts/compat-report.py then compares two profiles. See docs/COMPAT.md.
#
#   scripts/compat.sh <profile> [domain...]     # all sites, or just the named ones
#
# COMPAT_JOBS sets how many sites load at once (default 4).

source "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

profile="${1:-}"
require_profile "$profile"
shift

stage_profile "$profile"

# Build the probe against this profile's installed headers and libraries.
probe="$build_dir/tools/pageprobe"
source_file="$ZEBKIT_ROOT/tools/pageprobe/pageprobe.c"
pc_path="$stage_dir/lib/pkgconfig"
[[ "${run_env[*]}" == *"$OUT_DIR/deps/prefix"* ]] && pc_path+=":$OUT_DIR/deps/prefix/lib/pkgconfig"
if [[ ! -x "$probe" || "$source_file" -nt "$probe" ]]; then
    log "building pageprobe for $profile"
    mkdir -p "$(dirname "$probe")"
    # shellcheck disable=SC2046
    PKG_CONFIG_PATH="$pc_path" cc -O1 -Wall -Wno-unused-parameter -o "$probe" "$source_file" \
        $(PKG_CONFIG_PATH="$pc_path" pkg-config --cflags --libs wpe-webkit-2.0 wpe-platform-headless-2.0)
fi

results="$OUT_DIR/compat/$profile"
mkdir -p "$results"

# probe_site <domain> <url>: one site, one fresh browser process.
probe_site() {
    local domain="$1" url="$2"
    local raw="$results/$domain.raw"
    "${run_env[@]}" timeout 150 "$probe" --settle 8 --timeout 90 --snapshot "$raw" "$url" \
        > "$results/$domain.json" 2> "$results/$domain.log" \
        || echo '{"requested":"'"$url"'","probeFailed":true}' > "$results/$domain.json"
    if [[ -s "$raw" ]]; then
        python3 - "$raw" "$results/$domain.png" <<'PY' 2>/dev/null || true
import sys
from PIL import Image
with open(sys.argv[1], "rb") as f:
    width, height, stride = map(int, f.readline().split())
    Image.frombuffer("RGBA", (width, height), f.read(), "raw", "BGRA", stride, 1).convert("RGB").save(sys.argv[2])
PY
        rm -f "$raw"
    fi
    echo "$domain"
}

mapfile -t sites < <(python3 - "$ZEBKIT_ROOT/assets/WEBSITE-TOP-100.json" "$@" <<'PY'
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
    while (( $(jobs -rp | wc -l) >= jobs_max )); do
        wait -n || true
    done
    done_count=$(( done_count + 1 ))
    (( done_count % 10 == 0 )) && log "$done_count of ${#sites[@]} started"
done
wait
log "results in $results"
