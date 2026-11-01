#!/usr/bin/env bash
# Playwright's own test suite (tests/page and tests/library at the tag of the
# protocol zebkit speaks) against zebkit's endpoint (zebkit_automation_listen,
# chromium.connect()), or the same tests on Chrome to tell the engine's
# differences from what fails everywhere. docs/AUTOMATION.md, section 7.
#
#   scripts/playwright-suite.sh [--profile PROFILE] [--target zebkit|chrome]
#       [--project page|library] [--workers N] [--grep REGEX] [--timeout MS]
#       [--global-minutes N] [--out DIR] [--reference DIR] [--summarize-only] [files...]
#
# ZEBKIT_TEST_PACKAGE names another package; CHROME names another Chrome. The
# results go to DIR (default out/playwright-suite/<target>): results.jsonl, one
# line a test, and summary.txt, counts by file with each failure against
# tests/playwright-suite/expected-failures.txt. --reference DIR takes Chrome's
# results to mark what fails on Chrome too. Exits 1 on a failure that the list
# does not explain. The first run clones the tag into .cache and builds it.

source "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

profile=slim
target=zebkit
project=
workers=4
grep_for=
timeout_ms=30000
global_minutes=120
out=
reference=
summarize_only=0
files=()
while (( $# )); do
    case "$1" in
        --profile) profile="$2"; shift ;;
        --target) target="$2"; shift ;;
        --project) project="$2"; shift ;;
        --workers) workers="$2"; shift ;;
        --grep) grep_for="$2"; shift ;;
        --timeout) timeout_ms="$2"; shift ;;
        --global-minutes) global_minutes="$2"; shift ;;
        --out) out="$2"; shift ;;
        --reference) reference="$2"; shift ;;
        --summarize-only) summarize_only=1 ;;
        -*) die "unknown option $1" ;;
        *) files+=("$1") ;;
    esac
    shift
done
out="${out:-$OUT_DIR/playwright-suite/$target}"
suite="$ZEBKIT_ROOT/tests/playwright-suite"
repo="$CACHE_DIR/playwright-repo"
mkdir -p "$out"
out="$(cd "$out" && pwd -P)"
summarize() {
    python3 -I "$suite/summarize.py" "$out/results.jsonl" "$suite/expected-failures.txt" "$out/summary.txt" \
        ${reference:+--reference "$reference/results.jsonl"} ${engine_exits:+--exits "$engine_exits"}
}
engine_exits=
[[ "$target" == zebkit ]] && engine_exits="$out/engine-exits.log"
if (( summarize_only )); then
    summarize
    exit
fi

node=(node)
command -v mise > /dev/null && node=(mise exec node@24 -- node)
npm=(npm)
command -v mise > /dev/null && npm=(mise exec node@24 -- npm)
if [[ "$(git -C "$repo" describe --tags --exact-match 2> /dev/null)" != "$PLAYWRIGHT_SUITE_TAG" ]]; then
    rm -rf "$repo"
    log "cloning Playwright $PLAYWRIGHT_SUITE_TAG"
    git -c advice.detachedHead=false clone --quiet --depth 1 --branch "$PLAYWRIGHT_SUITE_TAG" "$PLAYWRIGHT_URL" "$repo"
fi
if [[ ! -e "$repo/packages/playwright-core/lib/coreBundle.js" ]]; then
    log "building Playwright's tree"
    (cd "$repo" && PLAYWRIGHT_SKIP_BROWSER_DOWNLOAD=1 nice -n 19 ionice -c3 "${npm[@]}" ci --no-audit --no-fund \
        && nice -n 19 ionice -c3 "${npm[@]}" run build)
fi

: > "$out/engine-exits.log"
rm -rf "$out/test-results" "$out/profiles"
# As the server of Playwright under its own tests: window.builtins in pages.
export PWTEST_UNDER_TEST=1
export PW_SUITE_REPO="$repo" PW_SUITE_OUT="$out" PW_SUITE_DIR="$suite" PW_SUITE_WORKERS="$workers" \
    PW_SUITE_TIMEOUT="$timeout_ms" PW_SUITE_GLOBAL_TIMEOUT="$(( global_minutes * 60000 ))" \
    PLAYWRIGHT_SKIP_VALIDATE_HOST_REQUIREMENTS=1
engines=
runner=
stop_engines() { [[ -n "$engines" ]] && kill "$engines" 2> /dev/null; engines=; }
# timeout(1) puts what it runs in a process group of its own, which a signal
# to this script's group does not reach: it is stopped by its own group.
stop_runner() { [[ -n "$runner" ]] && kill -- "-$runner" 2> /dev/null; runner=; }
trap 'stop_runner; stop_engines' EXIT
trap 'exit 143' TERM INT
if [[ "$target" == zebkit ]]; then
    package="${ZEBKIT_TEST_PACKAGE:-$OUT_DIR/$profile/package}"
    [[ -d "$package" ]] || die "no package in $package: scripts/package.sh $profile"
    build_against_package "$package" "$out/zebkit-run" "$ZEBKIT_ROOT/tools/zebkit-run/zebkit-run.c"
    export PW_SUITE_BASE_PORT=$(( 21000 + RANDOM % 20000 ))
    # A test that launches a browser of its own must not find Chrome here and
    # pass without touching the engine: it fails, and the list says why.
    mkdir -p "$out/no-browsers"
    export PLAYWRIGHT_BROWSERS_PATH="$out/no-browsers"
    nice -n 19 ionice -c3 "${node[@]}" "$suite/engines.mjs" "$out/zebkit-run" "$PW_SUITE_BASE_PORT" "$workers" "$out" &
    engines=$!
    up=0
    for _ in $(seq 300); do
        up=1
        for (( i = 0; i < workers; ++i )); do
            (exec 3<> "/dev/tcp/127.0.0.1/$(( PW_SUITE_BASE_PORT + i ))") 2> /dev/null || up=0
        done
        (( up )) && break
        kill -0 "$engines" 2> /dev/null || die "the engines stopped: see $out"
        sleep 0.2
    done
    (( up )) || die "the engines serve nothing: see $out/engine-0.log"
else
    # Unless named, the Chromium Playwright installed for itself, as it finds it:
    # in the user's cache, which lib.sh's XDG_CACHE_HOME would hide.
    unset XDG_CACHE_HOME
    chrome="${CHROME:-$(cd "$repo" && "${node[@]}" -e "console.log(require('playwright-core').chromium.executablePath())")}"
    [[ -x "$chrome" ]] || die "no Chrome: set CHROME"
    export PW_SUITE_CHROME="$chrome"
fi

arguments=(--config="$suite/playwright.config.ts")
[[ -n "$project" ]] && arguments+=(--project="$project")
[[ -n "$grep_for" ]] && arguments+=(--grep="$grep_for")
# The runner's own limit is the global timeout; this one is for a runner that hangs.
cd "$repo"
nice -n 19 ionice -c3 timeout -k 30 "$(( global_minutes * 60 + 120 ))" \
    "${node[@]}" node_modules/.bin/playwright test "${arguments[@]}" "${files[@]}" &
runner=$!
wait "$runner" || true
runner=
cd "$ZEBKIT_ROOT"
stop_engines
summarize
