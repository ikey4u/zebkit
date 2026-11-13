#!/usr/bin/env bash
# What Playwright's commands cost natively (<zebkit/automation.h>) and over
# CDP, on one page (tests/bench/automation/index.html), headless:
#
#   - zebkit native: tests/engine/automation.c's benchmark;
#   - zebkit endpoint: Playwright's own client connected (connect(), no
#     CDP) to zebkit_automation_listen() in tools/zebkit-run, the same
#     build: what a Playwright test in any language gets;
#   - zebkit over CDP: Playwright connected to tools/zebkit-run, the same
#     build;
#   - Chrome over CDP: Playwright with its own Chromium, when it is
#     installed (~/.cache/ms-playwright) or CHROME names one.
#
#   scripts/automation-bench.sh [--profile PROFILE] [--runs N] [--check]
#
# The report goes to docs/reports/automation-bench.md and .json, with the
# engine's and Chromium's versions, the profile and the date (lib.sh's
# report_engine). --check
# fails when a native command is slower than Playwright over CDP on the
# same build (docs/AUTOMATION.md, "How it is checked"), and writes no
# report. Needs Python 3, Node.js (at mise.toml's pin) and the package
# (scripts/package.sh); playwright-core is installed into .cache once.

source "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

profile=slim
runs=50
check=0
while (( $# )); do
    case "$1" in
        --profile) profile="$2"; shift ;;
        --runs) runs="$2"; shift ;;
        --check) check=1 ;;
        *) die "usage: $(basename "$0") [--profile PROFILE] [--runs N] [--check]" ;;
    esac
    shift
done

package="$OUT_DIR/$profile/package"
work="$OUT_DIR/automation-bench"
[[ -d "$package" ]] || die "no package in $package: scripts/package.sh $profile"
mkdir -p "$work"
build_against_package "$package" "$work/automation" "$ZEBKIT_ROOT/tests/engine/automation.c"
build_against_package "$package" "$work/zebkit-run" "$ZEBKIT_ROOT/tools/zebkit-run/zebkit-run.c"

node=("${node_cmd[@]}")
tools="$CACHE_DIR/playwright-tools"
ensure_npm "$tools" "${playwright_tools[@]}"

# The page server on a port the system gives it (lib.sh).
start_page_server "$ZEBKIT_ROOT/tests/bench/automation" "$work/server.log"
port="$page_port"
trap 'kill $page_server 2> /dev/null || true' EXIT
url="http://127.0.0.1:$port/"
switches=(--no-sandbox --headless)

log "native"
ZEBKIT_TEST_BASE="http://127.0.0.1:$port" ZEBKIT_AUTOMATION_BENCH="$runs" \
    "$work/automation" "${switches[@]}" --user-data-dir="$work/profile-native" 2> "$work/native.log" \
    | tail -n 1 > "$work/native.json"
log "Playwright over CDP, zebkit"
"${node[@]}" "$ZEBKIT_ROOT/tests/bench/automation/playwright.mjs" "$tools/node_modules/playwright-core" "$url" "$runs" \
    zebkit "$work/zebkit-run" "${switches[@]}" --user-data-dir="$work/profile-cdp" > "$work/cdp.json"
log "Playwright's client to zebkit's endpoint"
"${node[@]}" "$ZEBKIT_ROOT/tests/bench/automation/playwright.mjs" "$tools/node_modules/playwright-core" "$url" "$runs" \
    zebkit-endpoint "$work/zebkit-run" "${switches[@]}" --user-data-dir="$work/profile-endpoint" > "$work/endpoint.json" \
    || : > "$work/endpoint.json"
chrome="${CHROME:-$(ls -d "$HOME"/.cache/ms-playwright/chromium-*/chrome-linux64/chrome 2> /dev/null | sort -V | tail -n 1)}"
: > "$work/chrome.json"
if (( ! check )) && [[ -x "$chrome" ]]; then
    log "Playwright over CDP, Chrome ($chrome)"
    "${node[@]}" "$ZEBKIT_ROOT/tests/bench/automation/playwright.mjs" "$tools/node_modules/playwright-core" "$url" "$runs" \
        chrome "$chrome" > "$work/chrome.json"
fi

python3 -I - "$work" "$check" "$ZEBKIT_ROOT/docs/reports" "$runs" "$PLAYWRIGHT_CORE_VERSION" \
    "$(report_engine "$package" "$profile")" <<'EOF'
import json, os, sys
work, check, reports, runs, core = sys.argv[1], sys.argv[2] == "1", sys.argv[3], sys.argv[4], sys.argv[5]
engine = sys.argv[6]
def load(name):
    path = os.path.join(work, name)
    text = open(path).read().strip() if os.path.exists(path) else ""
    return json.loads(text) if text else None
native, cdp, chrome = load("native.json"), load("cdp.json"), load("chrome.json")
endpoint = load("endpoint.json")
if not native or not cdp:
    sys.exit("no results: see " + work)
slower = [name for name, r in native["results"].items()
          if name in cdp["results"] and r["median"] > cdp["results"][name]["median"]]
if check:
    for name in slower:
        print(f"FAIL {name}: native {native['results'][name]['median']} ms, over CDP {cdp['results'][name]['median']} ms")
    sys.exit(1 if slower else 0)
columns = [native] + ([endpoint] if endpoint else []) + [cdp] + ([chrome] if chrome else [])
lines = ["# Playwright's commands: native, zebkit's endpoint and over CDP", "", f"{engine}.", "",
         f"`scripts/automation-bench.sh`, {runs} runs of each (20 of `goto` and of each screenshot) after three to warm up,",
         f"headless, tests/bench/automation/index.html served locally. Playwright is playwright-core {core} in Node.js.",
         "Medians in milliseconds; p90 in parentheses.", "",
         "| Operation | " + " | ".join(c["engine"] + (f" ({c['version']})" if c.get("version") else "") for c in columns) + " | native vs over CDP |",
         "|---|" + "---|" * (len(columns) + 1)]
for name, r in native["results"].items():
    cells = []
    for c in columns:
        v = c["results"].get(name)
        cells.append(f"{v['median']:.2f} ({v['p90']:.2f})" if v else "")
    over = cdp["results"].get(name)
    ratio = f"{over['median'] / r['median']:.1f}x faster" if over and r["median"] > 0 and r["median"] <= over["median"] else ("slower" if over else "")
    lines.append(f"| {name} | " + " | ".join(cells) + f" | {ratio} |")
open(os.path.join(reports, "automation-bench.md"), "w").write("\n".join(lines) + "\n")
json.dump({"engine": engine, "native": native, "endpoint": endpoint, "cdp": cdp, "chrome": chrome}, open(os.path.join(reports, "automation-bench.json"), "w"), indent=1)
print("\n".join(lines))
EOF
