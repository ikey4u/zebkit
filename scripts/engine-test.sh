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
# - tests/engine/canvas.c, pdf_annotations.c, pdf_hooks.c, pdf_bindings.py and
#   script/pdf_plugin.mjs: <zebkit/canvas.h>, <zebkit/text.h>, annotations and
#   hooks in pixels, with examples/pdf-plugin in C, Python and JavaScript.
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
# - tests/engine/automation.c: Playwright's commands, natively
#   (<zebkit/automation.h>, docs/AUTOMATION.md): navigation, reads that
#   wait, actions that wait until an element is enabled and nothing covers
#   it, fill, check, keys and mouse, strict mode and timeouts.
# - tests/engine/automation_python.py: the same through zebkit.automation,
#   Playwright's async API for Python.
# - tests/engine/playwright_connect.mjs: Playwright's own client (Node.js,
#   playwright-core $PLAYWRIGHT_CORE_VERSION) connected to zebkit's
#   Playwright endpoint (zebkit_automation_listen, no CDP); skipped when
#   Node.js or playwright-core is not there (scripts/automation-bench.sh
#   installs it).
# - tests/engine/mcp_server.mjs, mcp_conformance.mjs, mcp_agent_run.mjs:
#   tools/zebkit-mcp (docs/MCP.md) with the official MCP client over standard
#   input and HTTP, every tool and the bounds' escapes; the MCP project's
#   conformance suite; a scripted task through its tools. Skipped without cargo,
#   Node.js or the network for the first install.
# - tests/engine/cdp_page.py: the same over CDP, as Puppeteer would:
#   Page.javascriptDialogOpening and handleJavaScriptDialog, printToPDF
#   (and its stream), Input.dispatchDragEvent and setInterceptDrags.
# - tests/engine/popup.c: a site's sign-in through a popup: the popup as a
#   view of the application's (zebkit_view_on_window_open), off-screen, still
#   linked to its opener; a popup from a timer blocked.
# - tests/engine/cdp_browser.py: what a page sees of the browser and of a
#   CDP client: nothing of the client (errors described without the
#   page's code), window.chrome, popups blocked, a real headless screen.
# - tests/engine/hook_policies.py: the hooks of <zebkit/hooks.h> through C, Rust,
#   Python and host JavaScript, against local HTTP and TLS servers that say what
#   they received, with input a person would give over CDP.
# - tests/engine/native_parent.c, native_parent_python.py and Rust's
#   examples/native_parent.rs under a private Xvfb: a view's window inside a
#   window of the host's (zebkit_view_attach_native), its tree, pixels, input
#   through XTEST, resizing, each closed first. With --headless, the refusal.
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
package="${ZEBKIT_TEST_PACKAGE:-$OUT_DIR/$profile/package}"
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
build_against_package "$package" "$work/canvas" "$tests/canvas.c"
build_against_package "$package" "$work/pdf-annotations" "$tests/pdf_annotations.c"
build_against_package "$package" "$work/pdf-hooks" "$tests/pdf_hooks.c" "$ZEBKIT_ROOT/examples/pdf-plugin/plugin.c" -- -D_GNU_SOURCE -I"$ZEBKIT_ROOT/examples/pdf-plugin"
build_against_package "$package" "$work/api" "$tests/api.c" -- -pthread
if [[ "$ZEBKIT_HOST" == linux ]]; then
    cc -std=c11 -O1 -Wall -Werror -I"$package/include" "$tests/late_load.c" -ldl -pthread -o "$work/late-load"
fi
build_against_package "$package" "$work/lifecycle" "$tests/lifecycle.c" -- -pthread
build_against_package "$package" "$work/host-loop" "$tests/host_loop.c" -- -pthread
if [[ "$ZEBKIT_HOST" == linux ]] && pkg-config --exists glib-2.0; then
    read -r -a glib_flags <<< "$(pkg-config --cflags --libs glib-2.0)"
    build_against_package "$package" "$work/host-loop-glib" "$tests/host_loop_glib.c" -- "${glib_flags[@]}" -pthread
fi
build_against_package "$package" "$work/startup-settings" "$tests/startup_settings.c"
build_against_package "$package" "$work/edit-commands" "$tests/edit_commands.c"
build_against_package "$package" "$work/hooks" "$tests/hooks.c"
build_against_package "$package" "$work/certificate" "$tests/certificate.c"
build_against_package "$package" "$work/http_auth" "$tests/http_auth.c"
build_against_package "$package" "$work/hook-policies" "$tests/hook_policies.c"
build_against_package "$package" "$work/offscreen-scale" "$tests/offscreen_scale.c"
build_against_package "$package" "$work/bridge" "$tests/bridge.c"
if [[ "$ZEBKIT_HOST" == linux ]] && [[ -f /usr/include/X11/Xlib.h ]]; then
    build_against_package "$package" "$work/native-parent" "$tests/native_parent.c" -- -lX11 -ldl
    build_against_package "$package" "$work/native-host-x11" "$ZEBKIT_ROOT/examples/native-host/x11.c" -- -lX11
fi
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
build_against_package "$package" "$work/automation" "$tests/automation.c"
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
build_against_package "$package" "$work/tools/zebkit-run" "$ZEBKIT_ROOT/tools/zebkit-run/zebkit-run.c"
build_against_package "$package" "$work/tools/zebkit-js" "$ZEBKIT_ROOT/tools/zebkit-js/zebkit-js.c"

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

run vendor "$work/vendor" "$SRC_DIR/third_party/pdfium/testing/resources/fonts/ahem/Ahem.ttf"
run vendor-cxx "$work/vendor-cxx"
run services "$work/services"
run pdf "$work/pdf"
# Ahem: every glyph a box of one em, which makes text's pixels certain.
ahem="$SRC_DIR/third_party/pdfium/testing/resources/fonts/ahem/Ahem.ttf"
run canvas "$work/canvas" "$ahem"
run pdf-annotations "$work/pdf-annotations" "$ahem"
run pdf-hooks "$work/pdf-hooks" "$ahem"
run pdf-bindings env ZEBKIT_DIR="$package" PYTHONPATH="$ZEBKIT_ROOT/bindings/python" \
    python3 "$tests/pdf_bindings.py" "$ahem"
run api "$work/api" "${engine_switches[@]}"
run lifecycle "$work/lifecycle" --no-sandbox --headless --user-data-dir="$work/profile-lifecycle"
run host-loop "$work/host-loop" --no-sandbox --headless --user-data-dir="$work/profile-host-loop"
run edit-commands "$work/edit-commands" --no-sandbox --headless --user-data-dir="$work/profile-edit-commands"
if [[ -x "$work/host-loop-glib" ]]; then
    run host-loop-glib "$work/host-loop-glib" --no-sandbox --headless --user-data-dir="$work/profile-host-loop-glib"
fi
mkdir -p "$work/startup-settings-files"
ZEBKIT_TEST_SETTINGS_DIR="$work/startup-settings-files" run startup-settings "$work/startup-settings" --user-agent=argv-agent --user-data-dir="$work/unused-argv-profile"
for log_mode in none file; do
    run "startup-settings-python-$log_mode" env ZEBKIT_DIR="$package" PYTHONPATH="$ZEBKIT_ROOT/bindings/python" \
        ZEBKIT_TEST_LOG_MODE="$log_mode" python3 "$tests/startup_settings_python.py"
done
if [[ -x "$work/native-parent" ]]; then
    run native-parent-refused "$work/native-parent" --no-sandbox --headless --user-data-dir="$work/profile-native-parent-refused"
    if command -v Xvfb > /dev/null; then
        exec 9< <(Xvfb -displayfd 1 -screen 0 2200x1400x24 -nolisten tcp 2> /dev/null)
        xvfb=$!
        pids+=("$xvfb")
        read -r -t 15 -u 9 xvfb_display || xvfb_display=
        if [[ -n "$xvfb_display" ]]; then
            run native-parent env -u WAYLAND_DISPLAY DISPLAY=":$xvfb_display" "$work/native-parent" \
                --no-sandbox --user-data-dir="$work/profile-native-parent"
            run native-parent-python env -u WAYLAND_DISPLAY DISPLAY=":$xvfb_display" ZEBKIT_DIR="$package" \
                PYTHONPATH="$ZEBKIT_ROOT/bindings/python" python3 "$tests/native_parent_python.py" \
                --no-sandbox --user-data-dir="$work/profile-native-parent-python"
            if command -v cargo > /dev/null; then
                if (cd "$ZEBKIT_ROOT/rust" && ZEBKIT_DIR="$package" cargo build -q -p zebkit --example native_parent); then
                    run native-parent-rust env -u WAYLAND_DISPLAY DISPLAY=":$xvfb_display" LD_LIBRARY_PATH="$package" \
                        "$ZEBKIT_ROOT/rust/target/debug/examples/native_parent" \
                        --no-sandbox --user-data-dir="$work/profile-native-parent-rust"
                else
                    echo "FAIL the Rust native parent program did not build"; failed=1
                fi
            fi
        else
            echo "FAIL Xvfb did not start"; failed=1
        fi
        kill "$xvfb" 2>/dev/null || true
        exec 9<&-
    else
        log "native-parent: skipped (no Xvfb)"
    fi
fi
run hooks "$work/hooks" --no-sandbox --headless --user-data-dir="$work/profile-hooks"
run hooks-startup-policy "$work/hooks" --no-sandbox --headless --disable-popup-blocking --user-data-dir="$work/profile-hooks-startup"
run lifecycle-python env ZEBKIT_DIR="$package" PYTHONPATH="$ZEBKIT_ROOT/bindings/python" \
    python3 "$tests/lifecycle_python.py" --no-sandbox --headless --user-data-dir="$work/profile-lifecycle-python"
run media-inventory env ZEBKIT_DIR="$package" PYTHONPATH="$ZEBKIT_ROOT/bindings/python" \
    python3 "$tests/media_inventory.py" --no-sandbox --headless --js-flags=--expose-gc --user-data-dir="$work/profile-media-inventory"
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
ZEBKIT_TEST_BASE="http://127.0.0.1:$port" ZEBKIT_TEST_FILES="$work/page-files" run automation "$work/automation" "${engine_switches[@]}"
grep -q '^\([0-9]*\) of \1 checks passed$' "$work/automation.log" || { echo "FAIL the automation test did not finish"; failed=1; }
run user-agent-atomic python3 "$tests/user_agent_atomic.py" "$work/tools/zebkit-run" "${engine_switches[@]}"
run user-agent-clients env ZEBKIT_DIR="$package" PYTHONPATH="$ZEBKIT_ROOT/bindings/python" \
    python3 "$tests/user_agent_clients.py"
run user-agent-native env ZEBKIT_DIR="$package" PYTHONPATH="$ZEBKIT_ROOT/bindings/python" \
    python3 "$tests/user_agent_native.py"
run offscreen-scale "$work/offscreen-scale" "${engine_switches[@]}"
run automation-python env ZEBKIT_DIR="$package" python3 -I "$tests/automation_python.py" "http://127.0.0.1:$port" "${engine_switches[@]}"
pw_node=(node)
command -v mise > /dev/null && pw_node=(mise exec node@24 -- node)
engine_test_node_ready=false
if "${pw_node[@]}" --version > /dev/null 2>&1; then
    engine_test_node_ready=true
    run certificate-h2 env ZEBKIT_DIR="$package" PYTHONPATH="$ZEBKIT_ROOT/bindings/python" \
        python3 "$tests/certificate_h2.py" "${pw_node[@]}"
else
    log "certificate-h2: skipped (no Node.js)"
fi
run certificate-c python3 "$tests/certificate_c.py" "$work/certificate"
run http-auth-c python3 "$tests/http_auth_c.py" "$work/http_auth"
if [[ -d "$CACHE_DIR/playwright-tools/node_modules/playwright-core" ]] && "$engine_test_node_ready"; then
    run playwright-connect "${pw_node[@]}" "$tests/playwright_connect.mjs" "$CACHE_DIR/playwright-tools/node_modules/playwright-core" \
        "$work/tools/zebkit-run" --no-sandbox --headless --user-data-dir="$work/profile-connect"
else
    log "playwright-connect: skipped (no Node.js or playwright-core)"
fi
# tools/zebkit-mcp, built against this package, with the official client and the
# conformance suite as development dependencies (upstream.lock); not shipped.
if command -v cargo > /dev/null && "$engine_test_node_ready"; then
    mcp_tools="$CACHE_DIR/mcp-tools"
    if [[ ! -d "$mcp_tools/node_modules/@modelcontextprotocol/sdk" ]]; then
        log "installing the MCP client $MCP_SDK_VERSION and conformance suite $MCP_CONFORMANCE_VERSION"
        mkdir -p "$mcp_tools"
        (cd "$mcp_tools" && "${pw_node[@]/%node/npm}" install --silent --no-save \
            "@modelcontextprotocol/sdk@$MCP_SDK_VERSION" "@modelcontextprotocol/conformance@$MCP_CONFORMANCE_VERSION") || true
    fi
    log "building tools/zebkit-mcp"
    if (cd "$ZEBKIT_ROOT/tools/zebkit-mcp" && ZEBKIT_DIR="$package" cargo build -q --locked --target-dir "$work/mcp-target"); then
        zebkit_mcp="$work/mcp-target/debug/zebkit-mcp"
        if [[ -d "$mcp_tools/node_modules/@modelcontextprotocol/sdk" ]]; then
            # Over standard input and HTTP, every tool twice: longer than the rest.
            log mcp-server
            if ! limited 900 "${pw_node[@]}" "$tests/mcp_server.mjs" "$mcp_tools" "$zebkit_mcp" --no-sandbox > "$work/mcp-server.log" 2>&1; then
                failed=1
                grep -v '^ok' "$work/mcp-server.log" | tail -40
            fi
            tail -1 "$work/mcp-server.log"
            grep -q '^\([0-9]*\) of \1 checks passed$' "$work/mcp-server.log" || { echo "FAIL the MCP server's test did not finish"; failed=1; }
            run mcp-conformance "${pw_node[@]}" "$tests/mcp_conformance.mjs" "$mcp_tools" "$zebkit_mcp" --no-sandbox
            run mcp-agent-run "${pw_node[@]}" "$tests/mcp_agent_run.mjs" "$mcp_tools" "$zebkit_mcp" --no-sandbox
            grep -q '^\([0-9]*\) of \1 checks passed$' "$work/mcp-agent-run.log" || { echo "FAIL the scripted MCP run did not finish"; failed=1; }
        else
            log "mcp-server: skipped (the MCP client could not be installed)"
        fi
    else
        echo "FAIL tools/zebkit-mcp did not build"; failed=1
    fi
else
    log "mcp-server: skipped (no cargo or Node.js)"
fi
if [[ "$ZEBKIT_HOST" == linux ]]; then
    for owner in main worker; do
        run "late-load-c-$owner" "$work/late-load" "$library" "$owner" "${engine_switches[@]}"
        run "late-load-$owner" env ZEBKIT_DIR="$package" PYTHONPATH="$ZEBKIT_ROOT/bindings/python" \
            python3 "$tests/late_load.py" "$owner" "${engine_switches[@]}"
    done
fi
run view-settings env ZEBKIT_DIR="$package" PYTHONPATH="$ZEBKIT_ROOT/bindings/python" \
    python3 "$tests/view_settings.py" "http://127.0.0.1:$port" "${engine_switches[@]}"
run navigation-hooks env ZEBKIT_DIR="$package" PYTHONPATH="$ZEBKIT_ROOT/bindings/python" \
    python3 "$tests/navigation_hooks.py" "${engine_switches[@]}"
run navigation-activation env ZEBKIT_DIR="$package" PYTHONPATH="$ZEBKIT_ROOT/bindings/python" \
    python3 "$tests/navigation_activation.py"
run navigation-activation-state env ZEBKIT_DIR="$package" PYTHONPATH="$ZEBKIT_ROOT/bindings/python" \
    ZEBKIT_TEST_PRERENDER_USER_AGENT=1 ZEBKIT_TEST_PRERENDER_INVALID_USER_AGENT=1 \
    ZEBKIT_TEST_PRERENDER_EVALUATE=1 \
    ZEBKIT_TEST_PRERENDER_INIT_SCRIPT=1 ZEBKIT_TEST_PRERENDER_DISPOSE_SCRIPT=1 \
    python3 "$tests/navigation_activation.py"
run navigation-activation-user-agent env ZEBKIT_DIR="$package" PYTHONPATH="$ZEBKIT_ROOT/bindings/python" \
    ZEBKIT_TEST_PRERENDER_USER_AGENT=1 python3 "$tests/navigation_activation.py"
run navigation-activation-user-agent-metadata env ZEBKIT_DIR="$package" PYTHONPATH="$ZEBKIT_ROOT/bindings/python" \
    ZEBKIT_TEST_PRERENDER_USER_AGENT=1 ZEBKIT_TEST_PRERENDER_USER_AGENT_METADATA=1 \
    ZEBKIT_TEST_PRERENDER_INIT_SCRIPT=1 ZEBKIT_TEST_PRERENDER_DISPOSE_SCRIPT=1 \
    python3 "$tests/navigation_activation.py"
for hook_test in popup_metadata popup_bindings console_hooks console_bindings certificate_hooks http_auth http_auth_python; do
    run "$hook_test" env ZEBKIT_DIR="$package" PYTHONPATH="$ZEBKIT_ROOT/bindings/python" \
        python3 "$tests/$hook_test.py" "${engine_switches[@]}"
done
run http-auth-proxy env ZEBKIT_DIR="$package" PYTHONPATH="$ZEBKIT_ROOT/bindings/python" \
    python3 "$tests/http_auth.py" proxy
run permission-hooks env ZEBKIT_DIR="$package" PYTHONPATH="$ZEBKIT_ROOT/bindings/python" \
    python3 "$tests/permission_hooks.py" "http://127.0.0.1:$port" --use-fake-device-for-media-stream "${engine_switches[@]}"
run pdf-outline env ZEBKIT_DIR="$package" PYTHONPATH="$ZEBKIT_ROOT/bindings/python" \
    python3 "$tests/pdf_outline.py"
for quality in 3G Slow-2G; do
    run "network-quality-$quality" env ZEBKIT_DIR="$package" PYTHONPATH="$ZEBKIT_ROOT/bindings/python" \
        python3 "$tests/network_quality.py" "$quality" "${engine_switches[@]}"
done
run media-tools env ZEBKIT_DIR="$package" PYTHONPATH="$ZEBKIT_ROOT/bindings/python" \
    ZEBKIT_MEDIA_PROBE="$work/media-probe" python3 "$tests/media_tools.py"
run media-tap env ZEBKIT_DIR="$package" PYTHONPATH="$ZEBKIT_ROOT/bindings/python" \
    ZEBKIT_MEDIA_PROBE="$work/media-probe" \
    python3 "$tests/media_tap.py" "http://127.0.0.1:$port" "${engine_switches[@]}" \
    --autoplay-policy=no-user-gesture-required

for adaptive_mode in down up audio mkv clock; do
    run "media-resolution-$adaptive_mode" env ZEBKIT_DIR="$package" PYTHONPATH="$ZEBKIT_ROOT/bindings/python" \
        ZEBKIT_MEDIA_PROBE="$work/media-probe" ZEBKIT_ADAPTIVE_MODE="$adaptive_mode" \
        python3 "$tests/media_resolution.py" "http://127.0.0.1:$port" "${engine_switches[@]}"
done
for clock_mode in hls hls-discontinuity webm off unknown; do
    run "media-clock-$clock_mode" env ZEBKIT_DIR="$package" PYTHONPATH="$ZEBKIT_ROOT/bindings/python" \
        ZEBKIT_MEDIA_PROBE="$work/media-probe" \
        python3 "$tests/media_clock.py" "http://127.0.0.1:$port" "$clock_mode" "${engine_switches[@]}"
done
ZEBKIT_MEDIA_PROBE="$work/media-probe" run cdp-page python3 -I "$tests/cdp_page.py" "$work/tools/zebkit-run" "http://127.0.0.1:$port" \
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
run cdp-browser python3 -I "$tests/cdp_browser.py" "$work/tools/zebkit-run" "http://127.0.0.1:$port" \
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
    run cookies python3 -I "$tests/cookies.py" "$work/tools/zebkit-run" --no-sandbox --headless
fi

mkdir -p "$work/scheme-files"
ZEBKIT_TEST_FILES="$work/scheme-files" run scheme "$work/scheme" "${engine_switches[@]}"

mkdir -p "$work/script-files"
# <zebkit/hooks.h> in every language, one engine per mode (tests/engine/hook_policies.py):
# links to other programs, downloads, client certificates, isolated worlds, file
# choosers, media, keys, context menus, fullscreen and the network.
hook_groups=(external download certificate world page network)
for hook_group in "${hook_groups[@]}"; do
    run "hook-c-$hook_group" env PYTHONPATH="$tests" python3 "$tests/hook_policies.py" "c:$hook_group" "$work/hook-policies"
    run "hook-javascript-$hook_group" env PYTHONPATH="$tests" python3 "$tests/hook_policies.py" "javascript:$hook_group" \
        "$work/tools/zebkit-js" "$tests/script/hook_policies.mjs"
    run "hook-python-$hook_group" env ZEBKIT_DIR="$package" PYTHONPATH="$ZEBKIT_ROOT/bindings/python:$tests" \
        python3 "$tests/hook_policies.py" "python:$hook_group" python3 "$tests/hook_policies_python.py"
done
run hook-python-popup env ZEBKIT_DIR="$package" PYTHONPATH="$ZEBKIT_ROOT/bindings/python:$tests" \
    python3 "$tests/hook_policies.py" "python:popup" python3 "$tests/hook_policies_python.py"
if command -v cargo > /dev/null; then
    log "building the Rust hook program"
    if (cd "$ZEBKIT_ROOT/rust" && ZEBKIT_DIR="$package" cargo build -q -p zebkit --example hook_policies); then
        for hook_group in "${hook_groups[@]}"; do
            run "hook-rust-$hook_group" env LD_LIBRARY_PATH="$package" PYTHONPATH="$tests" python3 "$tests/hook_policies.py" \
                "rust:$hook_group" "$ZEBKIT_ROOT/rust/target/debug/examples/hook_policies"
        done
    else
        echo "FAIL the Rust hook program did not build"; failed=1
    fi
else
    log "hook-rust: skipped (no cargo)"
fi

run hooks-javascript "$work/tools/zebkit-js" --no-sandbox --headless --user-data-dir="$work/profile-hooks-javascript" "$tests/script/hooks.mjs" "http://127.0.0.1:$port"
run certificate-bindings python3 "$tests/certificate_bindings.py" "$work/tools/zebkit-js"
run http-auth-bindings python3 "$tests/http_auth_bindings.py" "$work/tools/zebkit-js"
run user-agent-bindings python3 "$tests/user_agent_bindings.py" "$work/tools/zebkit-js"
run window-open-javascript "$work/tools/zebkit-js" --no-sandbox --headless --user-data-dir="$work/profile-window-open-javascript" "$tests/script/window_open.mjs" "http://127.0.0.1:$port"
grep -q '^ok JavaScript original popup adoption' "$work/window-open-javascript.log" || { echo "FAIL window-open script did not finish"; failed=1; }
run script "$work/tools/zebkit-js" "${engine_switches[@]}" "$tests/script/app.mjs" \
    "http://127.0.0.1:$port" "$work/script-files"
# It ends by itself once idle; ending before its last line is a failure.
grep -q '^\([0-9]*\) of \1 checks passed$' "$work/script.log" || { echo "FAIL the script did not finish"; failed=1; }

run pdf-plugin "$work/tools/zebkit-js" "${engine_switches[@]}" "$tests/script/pdf_plugin.mjs" "$ahem"
grep -q '^\([0-9]*\) of \1 checks passed$' "$work/pdf-plugin.log" || { echo "FAIL the PDF plugin's script did not finish"; failed=1; }

run ui "$work/tools/zebkit-js" "${engine_switches[@]}" "$tests/ui/app.mjs"
grep -q '^\([0-9]*\) of \1 checks passed$' "$work/ui.log" || { echo "FAIL zebkit.ui's script did not finish"; failed=1; }

ZEBKIT_DIR="$package" PYTHONPATH="$ZEBKIT_ROOT/bindings/python" \
    run python-services python3 "$tests/test_python.py" --services-only
ZEBKIT_DIR="$package" PYTHONPATH="$ZEBKIT_ROOT/bindings/python" \
    run python python3 "$tests/test_python.py" "${engine_switches[@]}"

log "CDP from the command line"
"$work/tools/zebkit-run" "${engine_switches[@]}" --remote-debugging-port=0 about:blank \
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
