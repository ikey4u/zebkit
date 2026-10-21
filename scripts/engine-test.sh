#!/usr/bin/env bash
# Test the engine library from programs built against the package, as an
# application is built against it.
#
#   scripts/engine-test.sh <profile>
#
# - The engine exports zebkit's names and nothing else.
# - tests/engine/vendor.c, vendor.cc: the libraries inside the engine
#   (<zebkit/vendor/*.h>), from C and from C++.
# - tests/engine/services.c: <zebkit/services.h> without the engine running.
# - tests/engine/pdf.c: <zebkit/pdf.h>, a document opened, read, rendered,
#   searched, changed and saved.
# - tests/engine/api.c: the C API (zebkit.h). Starts the engine, serves
#   CDP, loads a page, runs script in it, follows its events, posts tasks
#   from another thread, closes the view and quits.
# - tests/engine/bridge.c: zebkit.* in a page the application grants it
#   to, refused to one it does not; zebkit_fetch().
# - tests/engine/scheme.c: URL schemes of the application's own, a page
#   and its fetches answered by it, files in ranges.
# - tests/engine/script/app.mjs, run by tools/zebkit-js: an application in
#   JavaScript, outside any page.
# - tests/engine/ui/app.mjs: zebkit.ui, a window of Chromium's views from
#   that script, driven as a user would.
# - tools/zebkit-run with --remote-debugging-port=0: CDP as Chrome serves
#   it, read back over HTTP the way a client finds it.
#
# Without a window (--headless) and without the sandbox, which build
# machines and containers often cannot give the engine.

source "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

profile="${1:-}"
require_profile "$profile"
package="$OUT_DIR/$profile/package"
library="$(engine_library "$package")"
[[ -f "$library" ]] || die "no engine in $package; run scripts/package.sh $profile first"

work="$(mktemp -d)"
pids=()
trap 'for pid in "${pids[@]}"; do kill "$pid" 2>/dev/null || true; done; rm -rf "$work"' EXIT

log "building the test programs against $package"
tests="$ZEBKIT_ROOT/tests/engine"
build_against_package "$package" "$work/vendor" "$tests/vendor.c"
# shellcheck disable=SC2046
c++ -std=c++17 -Wall -Werror -I"$package/include" "$tests/vendor.cc" \
    $(engine_link_options "$package") -o "$work/vendor-cxx"
build_against_package "$package" "$work/services" "$tests/services.c"
build_against_package "$package" "$work/pdf" "$tests/pdf.c"
build_against_package "$package" "$work/api" "$tests/api.c" -- -pthread
build_against_package "$package" "$work/bridge" "$tests/bridge.c"
build_against_package "$package" "$work/scheme" "$tests/scheme.c"
build_against_package "$package" "$OUT_DIR/tools/zebkit-run" "$ZEBKIT_ROOT/tools/zebkit-run/zebkit-run.c"
build_against_package "$package" "$OUT_DIR/tools/zebkit-js" "$ZEBKIT_ROOT/tools/zebkit-js/zebkit-js.c"

engine_switches=(--no-sandbox --headless --user-data-dir="$work/profile")
failed=0

# run <name> <command...>: a test program; its output, and the engine's on failure.
run() {
    local name="$1"
    shift
    log "$name"
    if ! limited 180 "$@" > "$work/$name.log" 2> "$work/$name.err"; then
        failed=1
        cat "$work/$name.log"
        echo "it printed on stderr:"; tail -20 "$work/$name.err"
    else
        cat "$work/$name.log"
    fi
}

log "what the engine exports"
if [[ "$(uname)" == Darwin ]]; then
    # Mach-O puts an underscore before every C name.
    exported="$(nm -gU "$library" | awk '{ print substr($3, 2) }')"
else
    exported="$(nm -D --defined-only "$library" | awk '{ print $3 }')"
fi
foreign="$(grep -v '^zebkit_' <<< "$exported" | grep -v '^$' || true)"
if [[ -n "$foreign" ]]; then
    echo "FAIL exports names that are not zebkit's: $(head -5 <<< "$foreign" | paste -sd' ' -)"; failed=1
else
    echo "ok   $(wc -l <<< "$exported") names, all zebkit_*: $(grep -c '^zebkit_vendor_' <<< "$exported") of the libraries'"
fi

log "what the engine loads"
if [[ "$(uname)" == Darwin ]]; then
    # The system's frameworks and libraries, and the framework itself.
    loads="$(otool -L "$library" | tail -n +2 | awk '{ print $1 }')"
    stray="$(grep -v -e '^/System/Library/' -e '^/usr/lib/' -e '^@rpath/Zebkit.framework/' <<< "$loads" || true)"
else
    # Whatever the dynamic linker cannot find beside it or on the system.
    stray="$(ldd "$library" | grep 'not found' || true)"
fi
if [[ -n "$stray" ]]; then
    echo "FAIL loads what the package does not have: $(paste -sd' ' - <<< "$stray")"; failed=1
else
    echo "ok   only the system's libraries"
fi

run vendor "$work/vendor"
run vendor-cxx "$work/vendor-cxx"
run services "$work/services"
run pdf "$work/pdf"
run api "$work/api" "${engine_switches[@]}"

port="${ENGINE_TEST_PORT:-8796}"
python3 -u "$ZEBKIT_ROOT/tests/serve.py" "$tests/pages" "$port" > "$work/server.log" 2>&1 &
pids+=($!)
sleep 1
mkdir -p "$work/files"
ZEBKIT_TEST_BASE="http://127.0.0.1:$port" ZEBKIT_TEST_OTHER="http://localhost:$port" \
    ZEBKIT_TEST_FILES="$work/files" run bridge "$work/bridge" "${engine_switches[@]}"

mkdir -p "$work/scheme-files"
ZEBKIT_TEST_FILES="$work/scheme-files" run scheme "$work/scheme" "${engine_switches[@]}"

mkdir -p "$work/script-files"
run script "$OUT_DIR/tools/zebkit-js" "${engine_switches[@]}" "$tests/script/app.mjs" \
    "http://127.0.0.1:$port" "$work/script-files"
# It ends by itself once idle; ending before its last line is a failure.
grep -q '^\([0-9]*\) of \1 checks passed$' "$work/script.log" || { echo "FAIL the script did not finish"; failed=1; }

run ui "$OUT_DIR/tools/zebkit-js" "${engine_switches[@]}" "$tests/ui/app.mjs"
grep -q '^\([0-9]*\) of \1 checks passed$' "$work/ui.log" || { echo "FAIL zebkit.ui's script did not finish"; failed=1; }

ZEBKIT_DIR="$package" PYTHONPATH="$ZEBKIT_ROOT/bindings/python" \
    run python python3 "$tests/test_python.py" "${engine_switches[@]}"

log "CDP from the command line"
"$OUT_DIR/tools/zebkit-run" "${engine_switches[@]}" --remote-debugging-port=0 about:blank \
    > /dev/null 2> "$work/run.err" &
pids+=($!)
url=""
for (( waited = 0; waited < 300; waited++ )); do
    url="$(grep -o 'ws://[^ ]*' "$work/run.err" | head -1 || true)"
    [[ -n "$url" ]] && break
    sleep 0.1
done
if [[ -z "$url" ]]; then
    echo "FAIL no \"DevTools listening\" line; the engine printed:"; tail -20 "$work/run.err"
    failed=1
else
    port="${url#ws://*:}"; port="${port%%/*}"
    version="$(curl -fsS "http://127.0.0.1:$port/json/version")" || version=""
    if grep -q '"webSocketDebuggerUrl": *"ws://127.0.0.1:'"$port"'/devtools/browser/' <<< "$version"; then
        echo "ok   /json/version: $(grep -o '"Browser": *"[^"]*"' <<< "$version")"
    else
        echo "FAIL /json/version: $version"; failed=1
    fi
    if curl -fsS "http://127.0.0.1:$port/json/list" | grep -q '"type": *"page"'; then
        echo "ok   /json/list has the page"
    else
        echo "FAIL /json/list has no page"; failed=1
    fi
fi

(( failed )) && die "engine tests failed"
log "engine tests passed"
