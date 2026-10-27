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
# - tests/engine/offscreen.c: off-screen views (<zebkit/offscreen.h>):
#   frames in shared surfaces, their damage, size and scale, input, a
#   <select>'s list as a popup layer, no frames while idle, frames held
#   back while the application holds its allowance, the frame rate limit.
#   ENGINE_TEST_SHOTS=<dir> keeps pictures of the frames there.
# - examples/offscreen-host: the same page drawn in one process and shown
#   in another, which links nothing of zebkit's: the frames' buffers passed
#   over a socket, the pixels the host gets checked, its input sent back,
#   what the frames cost measured.
# - tests/engine/page.c: what a page asks of its window (<zebkit/page.h>):
#   dialogs held and answered, beforeunload, window.print(); the page
#   printed to PDF and read back; drags in the page from the mouse, drags
#   the application takes, files and text dropped from outside, a slider.
# - tests/engine/media.c: what a page plays, saved as it plays
#   (<zebkit/media.h>): files the element loads, MP4 and WebM; DASH through
#   Media Source Extensions; a seek; audio alone; the element under the
#   player's controls; a codec change, in two files, without replacing a
#   file there; maxDuration; encrypted media refused. Each file read back
#   with the engine's FFmpeg (media_probe.h), every frame decoded. Again
#   with zebkit_media_keep_history(): a video played before anything
#   listed media, captured from its first frame.
# - tests/engine/cdp_page.py: the same over CDP, as Puppeteer would:
#   Page.javascriptDialogOpening and handleJavaScriptDialog, printToPDF
#   (and its stream), Input.dispatchDragEvent and setInterceptDrags.
# - tests/engine/popup.c: a site's sign-in through a popup: the popup as a
#   view of the application's (zebkit_view_on_window_open), off-screen, still
#   linked to its opener; a popup from a timer blocked.
# - tests/engine/cdp_browser.py: what a page sees of the browser and of a
#   CDP client: nothing of the client (errors described without the
#   page's code), window.chrome, popups blocked, a real headless screen.
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
offscreen_key_identity=
webauthn_key_identity=
cleanup() {
    for pid in "${pids[@]}"; do kill "$pid" 2>/dev/null || true; done
    if [[ "$ZEBKIT_HOST" == macos && -n "$offscreen_key_identity" ]]; then
        # Only the throwaway key created by this invocation's test executable.
        /usr/bin/security delete-generic-password \
            -s "Zebkit Cookies: $offscreen_key_identity" \
            -a "$offscreen_key_identity" >/dev/null 2>&1 || true
    fi
    if [[ "$ZEBKIT_HOST" == macos && -n "$webauthn_key_identity" ]]; then
        /usr/bin/security delete-generic-password \
            -s "Zebkit Cookies: $webauthn_key_identity" \
            -a "$webauthn_key_identity" >/dev/null 2>&1 || true
    fi
    rm -rf "$work"
}
trap cleanup EXIT

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
offscreen_options=()
[[ "$(uname)" == Darwin ]] && offscreen_options=(-framework IOSurface)
offscreen_test="$work/offscreen"
if [[ "$ZEBKIT_HOST" == macos ]]; then
    # Recompiled ad hoc executables cannot read an older binary's Keychain
    # item without a user prompt. Use a reserved, disposable test identity.
    offscreen_key_identity="zebkit-test-offscreen-$(basename "$work")"
    offscreen_test="$work/$offscreen_key_identity"
fi
build_against_package "$package" "$offscreen_test" "$tests/offscreen.c" -- "${offscreen_options[@]}"
build_against_package "$package" "$work/page" "$tests/page.c"
build_against_package "$package" "$work/popup" "$tests/popup.c"
build_against_package "$package" "$work/cookie-host" "$tests/cookie_host.c"
build_against_package "$package" "$work/cookie-file-runner" "$ZEBKIT_ROOT/tools/zebkit-run/zebkit-run.c"
build_against_package "$package" "$work/media" "$tests/media.c"
build_against_package "$package" "$work/media-probe" "$tests/media_probe.c"
build_against_package "$package" "$work/mode" "$tests/mode.c"
webauthn_key_identity="zebkit-test-webauthn-$(basename "$work")"
webauthn_host="$work/$webauthn_key_identity"
build_against_package "$package" "$webauthn_host" "$tests/webauthn_host.c"
render_options=(-pthread)
[[ "$(uname)" == Darwin ]] && render_options+=(-framework IOSurface)
build_against_package "$package" "$work/offscreen-render" "$ZEBKIT_ROOT/examples/offscreen-host/render.c" -- "${render_options[@]}"
host_options=()
[[ "$(uname)" == Darwin ]] && host_options=(-framework CoreVideo -framework IOSurface -framework CoreFoundation)
cc -std=c11 -O1 -Wall -Werror "$ZEBKIT_ROOT/examples/offscreen-host/show.c" "${host_options[@]}" -o "$work/offscreen-show"
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
run mode-headless "$work/mode" "${engine_switches[@]}"
run mode-automation "$work/mode" --no-sandbox --headless --enable-automation \
    --expect-automation --user-data-dir="$work/profile-automation"
run mode-driver-port "$work/mode" --no-sandbox --headless --remote-debugging-port=0 \
    --expect-automation --user-data-dir="$work/profile-driver-port"

port="${ENGINE_TEST_PORT:-8796}"
python3 -u "$ZEBKIT_ROOT/tests/serve.py" "$tests/pages" "$port" > "$work/server.log" 2>&1 &
pids+=($!)
sleep 1
mkdir -p "$work/files"
ZEBKIT_TEST_BASE="http://127.0.0.1:$port" ZEBKIT_TEST_OTHER="http://localhost:$port" \
    ZEBKIT_TEST_FILES="$work/files" run bridge "$work/bridge" "${engine_switches[@]}"

ZEBKIT_TEST_BASE="http://127.0.0.1:$port" ZEBKIT_TEST_SHOTS="${ENGINE_TEST_SHOTS:-}" \
    run offscreen "$offscreen_test" "${engine_switches[@]}"
grep -q '^\([0-9]*\) of \1 checks passed$' "$work/offscreen.log" || { echo "FAIL the off-screen test did not finish"; failed=1; }
run offscreen-host "$work/offscreen-show" --renderer="$work/offscreen-render" \
    --url="http://127.0.0.1:$port/offscreen.html" --size=400x300@1 ${ENGINE_TEST_SHOTS:+--shots="$ENGINE_TEST_SHOTS"} \
    --test -- --no-sandbox --user-data-dir="$work/profile-host"
grep -q '^\([0-9]*\) of \1 checks passed$' "$work/offscreen-host.log" || { echo "FAIL the off-screen host did not finish"; failed=1; }

mkdir -p "$work/page-files"
ZEBKIT_TEST_BASE="http://127.0.0.1:$port" ZEBKIT_TEST_FILES="$work/page-files" \
    run page "$work/page" "${engine_switches[@]}"
grep -q '^\([0-9]*\) of \1 checks passed$' "$work/page.log" || { echo "FAIL the page test did not finish"; failed=1; }
ZEBKIT_MEDIA_PROBE="$work/media-probe" run cdp-page python3 -I "$tests/cdp_page.py" "$OUT_DIR/tools/zebkit-run" "http://127.0.0.1:$port" \
    "$work/page-files" --no-sandbox --headless --user-data-dir="$work/profile-cdp"
ZEBKIT_TEST_BASE="http://127.0.0.1:$port" run popup "$work/popup" "${engine_switches[@]}"
grep -q '^\([0-9]*\) of \1 checks passed$' "$work/popup.log" || { echo "FAIL the popup test did not finish"; failed=1; }
mkdir -p "$work/media-files"
ZEBKIT_TEST_BASE="http://127.0.0.1:$port" ZEBKIT_TEST_FILES="$work/media-files" \
    run media "$work/media" "${engine_switches[@]}" --autoplay-policy=no-user-gesture-required
grep -q '^\([0-9]*\) of \1 checks passed$' "$work/media.log" || { echo "FAIL the media test did not finish"; failed=1; }
# zebkit_media_keep_history(), in an engine of its own.
mkdir -p "$work/media-history-files"
ZEBKIT_TEST_HISTORY=1 ZEBKIT_TEST_BASE="http://127.0.0.1:$port" ZEBKIT_TEST_FILES="$work/media-history-files" \
    run media-history "$work/media" "${engine_switches[@]/profile/profile-history}" \
    --autoplay-policy=no-user-gesture-required
grep -q '^\([0-9]*\) of \1 checks passed$' "$work/media-history.log" || { echo "FAIL the media history test did not finish"; failed=1; }
run cdp-browser python3 -I "$tests/cdp_browser.py" "$OUT_DIR/tools/zebkit-run" "http://127.0.0.1:$port" \
    --no-sandbox --headless --user-data-dir="$work/profile-cdp-browser"

run cookie-host python3 -I "$tests/cookie_host.py" "$work/cookie-host" --no-sandbox --headless
run cookie-file python3 -I "$tests/cookie_file.py" "$work/cookie-host" --no-sandbox --headless
run cookie-file-options python3 -I "$tests/cookie_file_options.py" "$work/cookie-file-runner" --no-sandbox --headless

run webauthn-plain-headless python3 -I "$tests/webauthn.py" "$webauthn_host" \
    --no-sandbox --headless --test-normal-view
run webauthn-host-ui python3 -I "$tests/webauthn_ui.py" "$webauthn_host" --no-sandbox
run webauthn python3 -I "$tests/webauthn.py" "$webauthn_host" --no-sandbox --headless
run webauthn-unfocused python3 -I "$tests/webauthn.py" "$webauthn_host" \
    --no-sandbox --headless --test-authentication-ui
run webauthn-no-activation python3 -I "$tests/webauthn.py" "$webauthn_host" \
    --no-sandbox --headless --test-authentication-ui --test-focused

# Cookie encryption uses the real OS key store. Headless Linux builders
# without a desktop keyring can opt into this integration test explicitly.
if [[ "$ZEBKIT_HOST" == macos || "${ENGINE_TEST_PERSISTENT_COOKIES:-0}" == 1 ]]; then
    run cookies python3 -I "$tests/cookies.py" "$OUT_DIR/tools/zebkit-run" --no-sandbox --headless
fi

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
    run python-services python3 "$tests/test_python.py" --services-only
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
    # The product as Chrome names it, which tools read to know what they drive.
    if grep -q '"webSocketDebuggerUrl": *"ws://127.0.0.1:'"$port"'/devtools/browser/' <<< "$version" \
        && grep -q '"Browser": *"Chrome/[0-9.]*"' <<< "$version" \
        && grep -q '"User-Agent": *"[^"]* Chrome/[0-9]*\.0\.0\.0 ' <<< "$version"; then
        # The user agent is Chrome's under --headless too, not HeadlessChrome's.
        echo "ok   /json/version: $(grep -o '"Browser": *"[^"]*"' <<< "$version"), $(grep -o 'Chrome/[0-9.]*' <<< "$version" | tail -1) in the user agent"
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
