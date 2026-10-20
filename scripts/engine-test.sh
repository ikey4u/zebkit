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
# - tests/engine/api.c: the C API (zebkit.h). Starts the engine, serves
#   CDP, loads a page, runs script in it, follows its events, posts tasks
#   from another thread, closes the view and quits.
# - tests/engine/bridge.c: zebkit.* in a page the application grants it
#   to, refused to one it does not; zebkit_fetch().
# - tools/zebkit-run with --remote-debugging-port=0: CDP as Chrome serves
#   it, read back over HTTP the way a client finds it.
#
# Without a window (--headless) and without the sandbox, which build
# machines and containers often cannot give the engine.

source "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

profile="${1:-}"
require_profile "$profile"
package="$OUT_DIR/$profile/package"
[[ -f "$package/libzebkit.so.1" ]] || die "no engine in $package; run scripts/package.sh $profile first"

work="$(mktemp -d)"
pids=()
trap 'for pid in "${pids[@]}"; do kill "$pid" 2>/dev/null || true; done; rm -rf "$work"' EXIT

log "building the test programs against $package"
tests="$ZEBKIT_ROOT/tests/engine"
build_against_package "$package" "$work/vendor" "$tests/vendor.c"
c++ -std=c++17 -Wall -Werror -I"$package/include" "$tests/vendor.cc" \
    -L"$package" -l:libzebkit.so.1 -Wl,-rpath,"$package" -o "$work/vendor-cxx"
build_against_package "$package" "$work/services" "$tests/services.c"
build_against_package "$package" "$work/api" "$tests/api.c" -- -pthread
build_against_package "$package" "$work/bridge" "$tests/bridge.c"
build_against_package "$package" "$OUT_DIR/tools/zebkit-run" "$ZEBKIT_ROOT/tools/zebkit-run/zebkit-run.c"

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
exported="$(nm -D --defined-only "$package/libzebkit.so.1" | awk '{ print $3 }')"
foreign="$(grep -v '^zebkit_' <<< "$exported" | grep -v '^$' || true)"
if [[ -n "$foreign" ]]; then
    echo "FAIL exports names that are not zebkit's: $(head -5 <<< "$foreign" | paste -sd' ' -)"; failed=1
else
    echo "ok   $(wc -l <<< "$exported") names, all zebkit_*: $(grep -c '^zebkit_vendor_' <<< "$exported") of the libraries'"
fi

run vendor "$work/vendor"
run vendor-cxx "$work/vendor-cxx"
run services "$work/services"
run api "$work/api" "${engine_switches[@]}"

port="${ENGINE_TEST_PORT:-8796}"
python3 -u "$ZEBKIT_ROOT/tests/serve.py" "$tests/pages" "$port" > "$work/server.log" 2>&1 &
pids+=($!)
sleep 1
mkdir -p "$work/files"
ZEBKIT_TEST_BASE="http://127.0.0.1:$port" ZEBKIT_TEST_OTHER="http://localhost:$port" \
    ZEBKIT_TEST_FILES="$work/files" run bridge "$work/bridge" "${engine_switches[@]}"

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
