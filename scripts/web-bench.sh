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
# The report goes to docs/reports/web-bench.md and .json (not with --check),
# with the engine's and Chromium's versions, the profile, the date (lib.sh's
# report_engine) and Electron's version.
# --check fails when zebkit is over a limit: a page costs more CPU or
# memory, or more processes, than it did when the limits were set. Raising
# a limit is a decision to write in the commit that does it. --check also
# holds zebkit to its privacy defaults, as browserleaks.com checks them,
# with tests/bench/web/local/privacy.html: no permission a site would have
# to ask for is granted, the user agent and client hints are Chromium's for
# this version, WebRTC shows no private address.
#
# Needs Xvfb, Python 3, out/blink/<profile>/package (scripts/package.sh) and
# Electron unpacked by scripts/electron-baseline.sh. Each page is shown for
# BENCH_LOAD (10) seconds of loading and BENCH_REST (20) seconds at rest,
# BENCH_RUNS (3) times in each engine; the report and the check take the
# median of the runs, since live sites differ from one load to the next.

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
# The first free display, from Xvfb itself.
start_xvfb 7 1920x1080x24
trap 'kill $xvfb_pid 2>/dev/null || true' EXIT
export DISPLAY="$xvfb_display"
unset WAYLAND_DISPLAY

# The user agent both send: Chrome's for zebkit's Chromium, as zebkit sends it.
export BENCH_UA="Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/${CHROMIUM_VERSION%%.*}.0.0.0 Safari/537.36"
load="${BENCH_LOAD:-10}"
rest="${BENCH_REST:-20}"
runs="${BENCH_RUNS:-3}"

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
  for run in $(seq "$runs"); do
    log "$name ($run of $runs)"
    rm -rf "$work/profile-zebkit" "$work/profile-electron"
    # Each engine's CDP on a port that was free.
    port="$(free_port)"
    python3 -I "$bench/measure.py" --label "zebkit|$name" --port "$port" --load "$load" --rest "$rest" -- \
        "$work/zebkit-run" --no-sandbox --remote-debugging-port="$port" --user-data-dir="$work/profile-zebkit" \
        --window-size=1280,800 "$url" >> "$results"
    (( check )) && continue
    port="$(free_port)"
    BENCH_URL="$url" python3 -I "$bench/measure.py" --label "electron|$name" --port "$port" --load "$load" --rest "$rest" -- \
        "$work/electron/electron" --no-sandbox --ozone-platform=x11 --remote-debugging-port="$port" \
        --user-data-dir="$work/profile-electron" >> "$results"
  done
done

# One row per engine and page: the median of each measure over the runs.
python3 -I - "$results" <<'PY'
import json, statistics, sys
rows = {}
for line in open(sys.argv[1]):
    r = json.loads(line)
    rows.setdefault(r["label"], []).append(r)
with open(sys.argv[1], "w") as out:
    for label, runs in rows.items():
        middle = sorted(runs, key=lambda r: r["rest_cpu_pct"])[len(runs) // 2]
        median = dict(middle)
        for key in ("load_cpu_s", "rest_cpu_pct", "pss_mb", "processes"):
            median[key] = statistics.median(r[key] for r in runs)
        median["pss_mb"] = round(median["pss_mb"])
        median["runs"] = len(runs)
        median["rest_cpu_pct_runs"] = [r["rest_cpu_pct"] for r in runs]
        out.write(json.dumps(median) + "\n")
PY

if (( check )); then
    build_against_package "$package" "$work/privacy" "$bench/privacy.c"
    log "privacy defaults"
    rm -rf "$work/profile-privacy"
    # A page that does not answer is reported below, as a failure of the check.
    { timeout 60 "$work/privacy" --no-sandbox --user-data-dir="$work/profile-privacy" \
        "file://$bench/local/privacy.html" 2>/dev/null || true; } | tail -1 > "$work/privacy.json"
    python3 -I - "$results" "$bench/limits.json" "$work/privacy.json" "${CHROMIUM_VERSION%%.*}" <<'PY'
import ipaddress, json, sys
results = [json.loads(line) for line in open(sys.argv[1])]
limits = json.load(open(sys.argv[2]))
failed = 0
def check(ok, what, value):
    global failed
    failed += not ok
    print(f"{'ok  ' if ok else 'FAIL'} privacy          {what:28s} {value}")
try:
    privacy = json.load(open(sys.argv[3]))
except ValueError:
    privacy = {"error": "no result"}
major = sys.argv[4]
if "error" in privacy:
    check(False, "privacy page", privacy["error"])
else:
    for name, state in privacy.items():
        if name.startswith("permission:") and name != "permission:screen-wake-lock":
            check(state in ("denied", "prompt", "unsupported"), name, state)
    check(f"Chrome/{major}.0.0.0" in privacy["userAgent"] and "content_shell" not in privacy["userAgent"],
          "user agent", privacy["userAgent"])
    check(f"Chromium/{major}" in privacy["brands"] and not any("content_shell" in b for b in privacy["brands"]),
          "client hints brands", privacy["brands"])
    check(privacy.get("webdriver") is False, "navigator.webdriver", privacy.get("webdriver"))
    private = []
    for address in privacy["iceAddresses"]:
        try:
            ip = ipaddress.ip_address(address)
            if ip.is_private or ip.is_loopback or ip.is_link_local:
                private.append(address)
        except ValueError:
            pass  # an mDNS name (*.local) hides the address, as Chrome does
    check(not private, "WebRTC private addresses", privacy["iceAddresses"])
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

python3 -I - "$results" "$ZEBKIT_ROOT/docs/reports" "$ELECTRON_VERSION" "$ZEBKIT_ROOT" "$load" "$rest" \
    "$(report_engine "$package" "$profile")" <<'PY'
import json, sys, datetime
results, reports, electron, root, load, rest, engine_line = sys.argv[1:8]
rows = {}
for line in open(results):
    r = json.loads(line)
    # The local pages by their place in the repository, not this machine's path.
    r["url"] = r["url"].replace(f"file://{root}/", "file://./")
    engine, page = r["label"].split("|", 1)
    rows.setdefault(page, {})[engine] = r
json.dump({"engine": engine_line, "electron": electron, "pages": rows}, open(f"{reports}/web-bench.json", "w"), indent=1)
def cell(z, e, key, fmt):
    zv, ev = z[key], e[key]
    win = "**" if zv < ev else ""
    return f"{win}{fmt.format(zv)}{win} / {fmt.format(ev)}"
lines = [f"# Web pages: zebkit and Electron {electron}", "", f"{engine_line}; Electron {electron}.", "",
         f"scripts/web-bench.sh, {datetime.date.today()}. Each cell is zebkit / Electron, the median of {next(iter(rows.values()))['zebkit'].get('runs', 1)} runs; bold where zebkit costs less.",
         f"Loading: CPU seconds in the first {load} s. At rest: share of one core in the {rest} s after. Memory: PSS of every process.", "",
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
