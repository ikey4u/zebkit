#!/usr/bin/env bash
# What web pages cost in zebkit and in Electron: CPU while a page loads, CPU
# once it is shown and at rest, memory, processes. The same page, the same
# user agent and the same display for both; that each loaded is read over
# CDP. Electron is the release upstream.lock pins, the latest stable one.
#
#   scripts/web-bench.sh                 # the local pages, zebkit and Electron
#   scripts/web-bench.sh --sites         # and the sites of tests/bench/web/sites.txt (network)
#   scripts/web-bench.sh --check         # the local pages in zebkit, against tests/bench/web/limits.json
#   scripts/web-bench.sh --profile slim-dev ...
#
# The report goes to docs/reports/web-bench.md and .json (not with --check).
# --check fails when zebkit is over a limit: a page costs more CPU or
# memory, or more processes, than it did when the limits were set. Raising
# a limit is a decision to write in the commit that does it.
#
# Needs Xvfb, Python 3, out/blink/<profile>/package (scripts/package.sh) and
# Electron unpacked by scripts/electron-baseline.sh. Each page is shown for
# BENCH_LOAD (10) seconds of loading and BENCH_REST (20) seconds at rest.

source "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

profile=slim
sites=0
check=0
while (( $# )); do
    case "$1" in
        --sites) sites=1 ;;
        --check) check=1 ;;
        --profile) profile="$2"; shift ;;
        *) die "usage: $(basename "$0") [--sites] [--check] [--profile PROFILE]" ;;
    esac
    shift
done

bench="$ZEBKIT_ROOT/tests/bench/web"
package="$OUT_DIR/$profile/package"
work="$OUT_DIR/web-bench"
[[ -d "$package" ]] || die "no package in $package: scripts/package.sh $profile"
command -v Xvfb > /dev/null || die "Xvfb is missing"

# zebkit: tools/zebkit-run against the package.
mkdir -p "$work"
build_against_package "$package" "$work/zebkit-run" "$ZEBKIT_ROOT/tools/zebkit-run/zebkit-run.c"

# Electron: the pinned release with tests/bench/web/electron-app as its
# application (a packaged app in resources/app comes before an argument).
electron_dir="$OUT_DIR/electron/package"
if (( ! check )); then
    [[ -x "$electron_dir/electron" ]] || die "no Electron in $electron_dir: scripts/electron-baseline.sh"
    rm -rf "$work/electron"
    cp -a "$electron_dir" "$work/electron"
    rm -rf "$work/electron/resources/app"
    cp -a "$bench/electron-app" "$work/electron/resources/app"
fi

# One display for both, without a GPU: the same software rendering. Wayland
# is hidden so that neither picks the session's compositor.
display=:$((90 + RANDOM % 9))
Xvfb "$display" -screen 0 1920x1080x24 > /dev/null 2>&1 &
xvfb=$!
trap 'kill $xvfb 2>/dev/null || true' EXIT
sleep 1
export DISPLAY="$display"
unset WAYLAND_DISPLAY

# The user agent both send: Chrome's for zebkit's Chromium, as zebkit sends it.
export BENCH_UA="Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/${CHROMIUM_VERSION%%.*}.0.0.0 Safari/537.36"
load="${BENCH_LOAD:-10}"
rest="${BENCH_REST:-20}"

pages=()
for page in static animation heavy; do pages+=("local:$page|file://$bench/local/$page.html"); done
if (( sites )); then
    while read -r url; do
        [[ -z "$url" || "$url" == \#* ]] && continue
        pages+=("${url#https://}|$url")
    done < "$bench/sites.txt"
fi

results="$work/results.jsonl"
: > "$results"
for entry in "${pages[@]}"; do
    name="${entry%%|*}"
    url="${entry#*|}"
    log "$name"
    rm -rf "$work/profile-zebkit" "$work/profile-electron"
    python3 -I "$bench/measure.py" --label "zebkit|$name" --port 9411 --load "$load" --rest "$rest" -- \
        "$work/zebkit-run" --no-sandbox --remote-debugging-port=9411 --user-data-dir="$work/profile-zebkit" \
        --window-size=1280,800 "$url" >> "$results"
    (( check )) && continue
    BENCH_URL="$url" python3 -I "$bench/measure.py" --label "electron|$name" --port 9412 --load "$load" --rest "$rest" -- \
        "$work/electron/electron" --no-sandbox --ozone-platform=x11 --remote-debugging-port=9412 \
        --user-data-dir="$work/profile-electron" >> "$results"
done

if (( check )); then
    python3 -I - "$results" "$bench/limits.json" <<'PY'
import json, sys
results = [json.loads(line) for line in open(sys.argv[1])]
limits = json.load(open(sys.argv[2]))
failed = 0
for r in results:
    page = r["label"].split("|", 1)[1]
    for key, limit in limits.get(page, {}).items():
        value = r["process_kinds"].get("renderer", 0) if key == "renderers" else r[key]
        ok = value <= limit
        failed += not ok
        print(f"{'ok  ' if ok else 'FAIL'} {page:16s} {key:13s} {value:>8} (limit {limit})")
sys.exit(1 if failed else 0)
PY
    exit $?
fi

python3 -I - "$results" "$ZEBKIT_ROOT/docs/reports" "$ELECTRON_VERSION" <<'PY'
import json, sys, datetime
results, reports, electron = sys.argv[1], sys.argv[2], sys.argv[3]
rows = {}
for line in open(results):
    r = json.loads(line)
    engine, page = r["label"].split("|", 1)
    rows.setdefault(page, {})[engine] = r
json.dump(rows, open(f"{reports}/web-bench.json", "w"), indent=1)
def cell(z, e, key, fmt):
    zv, ev = z[key], e[key]
    win = "**" if zv < ev else ""
    return f"{win}{fmt.format(zv)}{win} / {fmt.format(ev)}"
lines = [f"# Web pages: zebkit and Electron {electron}", "",
         f"scripts/web-bench.sh, {datetime.date.today()}. Each cell is zebkit / Electron; bold where zebkit costs less.",
         "Loading: CPU seconds in the first 10 s. At rest: share of one core in the 20 s after. Memory: PSS of every process.", "",
         "| Page | Loaded (title) | Loading, s | At rest, % | Memory, MB | Renderers |", "|---|---|---:|---:|---:|---:|"]
totals = {"zebkit": [0, 0, 0], "electron": [0, 0, 0]}
for page, r in rows.items():
    z, e = r.get("zebkit"), r.get("electron")
    if not (z and e):
        continue
    for engine, x in (("zebkit", z), ("electron", e)):
        totals[engine][0] += x["load_cpu_s"]; totals[engine][1] += x["rest_cpu_pct"]; totals[engine][2] += x["pss_mb"]
    same = z["title"][:30] == e["title"][:30]
    lines.append(f"| {page} | {z['title'][:40]}{'' if same else ' (Electron: ' + e['title'][:30] + ')'} | "
                 f"{cell(z, e, 'load_cpu_s', '{:.1f}')} | {cell(z, e, 'rest_cpu_pct', '{:.1f}')} | {cell(z, e, 'pss_mb', '{:d}')} | "
                 f"{z['process_kinds'].get('renderer', 0)} / {e['process_kinds'].get('renderer', 0)} |")
z, e = totals["zebkit"], totals["electron"]
lines.append(f"| **All** | | {z[0]:.1f} / {e[0]:.1f} | {z[1]:.1f} / {e[1]:.1f} | {z[2]:.0f} / {e[2]:.0f} | |")
open(f"{reports}/web-bench.md", "w").write("\n".join(lines) + "\n")
print("\n".join(lines))
PY
log "report: docs/reports/web-bench.md"
