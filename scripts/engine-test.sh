#!/usr/bin/env bash
# Test the engine library from programs built against the package, as an
# application is built against it.
#
#   scripts/engine-test.sh <profile> [--only REGEX] [--strict]
#   scripts/engine-test.sh <profile> --fetch
#   scripts/engine-test.sh <profile> --build-only DIR
#   DIR/scripts/engine-test.sh <profile> --prebuilt DIR/programs [--only REGEX] [--strict]
#
# --build-only makes DIR, which holds all the suite needs: the programs (for a
# macOS profile on Linux, cross-compiled and signed ad hoc), the package, the
# tests' scripts and pages. Copy it to a machine with no compiler or source and
# run the suite there with --prebuilt; docs/MACOS.md has the steps. --only runs the
# tests whose names match, and builds only the programs they run; one that
# matches no test fails.
#
# The run itself does not use the network. --fetch gets what some tests need
# from it, into .cache: the real long PDF, and the npm packages of the CDP
# clients and of the MCP client (upstream.lock's versions); without them those
# tests are skipped. Every test skipped for want of a tool or a download is
# listed at the end, with every part of a test its log says was not run (its
# "skip <part>: <why>" and "narrowed: ..." lines). --strict (or
# ENGINE_TEST_STRICT=1) makes a skip, and a test that exits 0 after printing
# FAIL lines, a failure; ENGINE_TEST_EXPECTED_SKIPS, extended regular
# expressions one a line on "<test>: <why>" or "<test>: <part>: <why>", names
# the skips it accepts (scripts/check.sh's list). Tests that are only run when
# asked (ENGINE_TEST_WINDOWS=1, ENGINE_TEST_PERSISTENT_COOKIES=1) are not
# skips. Each test has a profile of its own, profile-<name> in the work
# directory, and the page server is on the port the system gives it.
#
# On macOS the suite keeps out of the person's files: HOME and the temporary
# directory are its own, and the Keychain calls are answered from files
# (tests/engine/macos_private_keychain.c) unless ENGINE_TEST_REAL_KEYCHAIN=1.
# ENGINE_TEST_KEEP_WORK=1 keeps the logs and profiles.
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
# - tests/engine/frames_input.c: input to a page's frames as a prototype
#   viewer has them (same origin, srcdoc, blob:, sandboxed, another site's,
#   scaled, in scrolled boxes): clicks, hover, selection drags, keys, an
#   input method, the wheel, <select>, drops; on Linux also in a window.
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
# - tests/engine/find_in_page.py: find in page (zebkit_view_find) in frames
#   and shadow trees, next and previous, highlights, stopping; Zebkit.find*.
# - tests/engine/download_outcomes.py: how downloads end: complete with the
#   saved path, cancelled, failed on the network or the file system, each
#   with its error; Zebkit.downloadProgress.
# - tests/engine/automation_dialogs.py: dialogs native Playwright clicks
#   open while a CDP client or the application holds the page's dialogs.
# - tests/engine/notifications.py, tray.py: web notifications and the tray icon, on Linux
#   against fake desktop services on a private bus (fake_notifications.py, fake_tray_watcher.py).
# - tests/engine/appearance.py: pages following the system's colour scheme (appearance.follow),
#   on Linux through a fake XDG portal (fake_portal.py).
# - tests/engine/window_frame.py: <zebkit/window.h>: options before the window, frames,
#   the title bar area the page sees, drag regions against real clicks, state and events.
# - tests/engine/system_holes.py: File System Access through the file chooser hook
#   and the permission hook, protocol handlers and pointer/keyboard lock as permissions.
# - tests/engine/html_in_canvas.py: HTML drawn into a canvas (drawElementImage(),
#   Chromium 157) in a view's pixels where it is drawn, and its input there.
# - tests/engine/system_locale.py: the page's locale the system's (LANG, on
#   Linux), and en-US where it has none.
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
# - tests/engine/cdp_clients.mjs: puppeteer-core and playwright-core
#   (connectOverCDP) against zebkit and against Chrome, in the same run, the
#   differences in a table; those expected are named with their reasons in
#   tests/engine/cdp_clients_expected.txt. Skipped without Node.js or Chrome.
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
# - tests/engine/http_auth.py, certificate.py, user_agent.py, console.py and
#   popup.py: HTTP authentication, a document's TLS errors, the user agent, the
#   console and new windows, each one driver and one list of modes for every
#   language (language_modes.py); worker_websocket.py: workers' WebSockets.
# - tests/engine/native_parent.c, native_parent_python.py and Rust's
#   examples/native_parent.rs under a private Xvfb: a view's window inside a
#   window of the host's (zebkit_view_attach_native), its tree, pixels, input
#   through XTEST, resizing, each closed first. With --headless, the refusal.
#   On macOS tests/engine/native_parent_mac.m, on an NSView, and examples/native-host's
#   AppKit host, with ENGINE_TEST_WINDOWS=1.
# - tests/engine/gpu_status.py, on macOS: hardware compositing and WebGL, as chrome://gpu has them.
# - tests/engine/pdf_in_page.py: a page that opens a PDF, as Chrome's: what the page
#   sees, scrolling, zoom, find, copy, links, printing, a long real document's memory
#   ($PDF_REAL_URL, fetched into the cache if the network allows).
# - tests/engine/native_host_pdf_scroll.py: the native hosts' PDF pane through the
#   whole document with the wheel: on Linux with XTest, if the hosts of
#   examples/native-host are built; on a Mac, AppKit's by itself.
# - tests/engine/scheme.c: URL schemes of the application's own, a page
#   and its fetches answered by it, files in ranges.
# - tests/engine/script/app.mjs, run by tools/zebkit-js: an application in
#   JavaScript, outside any page.
# - tests/engine/ui/app.mjs: zebkit.ui, a window of Chromium's views from
#   that script, driven as a user would.
# - tools/zebkit-run with --remote-debugging-port=0: CDP as Chrome serves
#   it, read back over HTTP the way a client finds it.
#
# Without a window (--headless) and, as each program's --no-sandbox says, without
# the sandbox, which build machines and containers often cannot give the engine
# (docs/SANDBOX.md); tests/engine/sandbox_startup.py checks what happens without
# the switch. The engine says so on standard error each time.

source "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

profile="${1:-}"
require_profile "$profile"
shift
build_only=
prebuilt=
only=
fetch=
strict="${ENGINE_TEST_STRICT:-}"
[[ "$strict" == 0 ]] && strict=
while (( $# )); do
    case "$1" in
        --build-only) build_only="$(caller_path "${2:?a directory}")"; shift 2 ;;
        --prebuilt) prebuilt="$(caller_path "${2:?a directory}")"; shift 2 ;;
        --only) only="${2:?a regular expression}"; shift 2 ;;
        --strict) strict=1; shift ;;
        --fetch) fetch=1; shift ;;
        *) die "unknown option $1" ;;
    esac
done
# The tests read it too: a part of one not run fails it, unless
# ENGINE_TEST_EXPECTED_SKIPS names it (tests/engine/harness.py).
[[ -n "$strict" ]] && export ENGINE_TEST_STRICT=1 || unset ENGINE_TEST_STRICT
# Nothing of the tests' Python is compiled into the source tree: this for the
# runs without -I, -B for the isolated ones (lib.sh's python_run).
export PYTHONDONTWRITEBYTECODE=1
# Cargo's builds within the machine's limit (lib.sh's ZEBKIT_JOBS).
export CARGO_BUILD_JOBS="${CARGO_BUILD_JOBS:-$ZEBKIT_JOBS}"

# What the run takes from the network, fetched apart: --fetch.
real_pdf="${ZEBKIT_TEST_REAL_PDF:-$CACHE_DIR/pdf/PDF32000_2008.pdf}"
cdp_clients="$CACHE_DIR/cdp-clients"
cdp_clients_packages=("puppeteer-core@$PUPPETEER_CORE_VERSION" "playwright-core@$PLAYWRIGHT_CORE_VERSION")
mcp_tools="$CACHE_DIR/mcp-tools"
mcp_tools_packages=("@modelcontextprotocol/sdk@$MCP_SDK_VERSION" "@modelcontextprotocol/conformance@$MCP_CONFORMANCE_VERSION")
if [[ -n "$fetch" ]]; then
    if [[ ! -f "$real_pdf" ]]; then
        log "fetching $PDF_REAL_URL"
        mkdir -p "$(dirname "$real_pdf")"
        curl -fsSL --max-time 300 -o "$real_pdf.part" "$PDF_REAL_URL" || die "the real PDF could not be fetched"
        [[ "$(sha256 "$real_pdf.part" | cut -d' ' -f1)" == "$PDF_REAL_SHA256" ]] || { rm -f "$real_pdf.part"; die "the real PDF is not the pinned one"; }
        mv "$real_pdf.part" "$real_pdf"
    fi
    if "${node_cmd[@]}" --version > /dev/null 2>&1; then
        ensure_npm --ignore-scripts "$cdp_clients" "${cdp_clients_packages[@]}" || die "the CDP clients could not be installed"
        ensure_npm "$mcp_tools" "${mcp_tools_packages[@]}" || die "the MCP client could not be installed"
        ensure_npm "$CACHE_DIR/playwright-tools" "${playwright_tools[@]}" || die "playwright-core could not be installed"
    else
        log "no Node.js: the npm packages are not fetched"
    fi
    # The crates of the Rust hosts of examples/ (example-gpui-browser,
    # example-winit-host), which the run checks offline.
    if command -v cargo > /dev/null; then
        for rust_host_dir in examples/gpui-browser examples/native-host/winit; do
            log "fetching the crates of $rust_host_dir"
            (cd "$ZEBKIT_ROOT/$rust_host_dir" && cargo fetch -q --locked) || die "the crates of $rust_host_dir could not be fetched"
        done
    else
        log "no cargo: the examples' crates are not fetched"
    fi
    log "fetched what the tests need"
    exit 0
fi
set_target_for_profile "$profile"
[[ "$ZEBKIT_TARGET" == "$ZEBKIT_HOST" || -n "$build_only" ]] \
    || die "$profile's programs do not run on $ZEBKIT_HOST: use --build-only and run them there"
package="${ZEBKIT_TEST_PACKAGE:-$OUT_DIR/$profile/package}"
library="$(engine_library "$package")"
[[ -f "$library" ]] || die "no engine in $package; run scripts/package.sh $profile first"

work="$(mktemp -d "$TMPDIR/engine-test.XXXXXX")"
pids=()
# Programs copied into the package for a run (on Windows, beside zebkit.dll):
# removed again, the package stays what package.sh made.
package_copies=()
offscreen_key_identity=
webauthn_key_identity=
cleanup() {
    for pid in "${pids[@]}"; do kill "$pid" 2>/dev/null || true; done
    for copy in "${package_copies[@]}"; do rm -f "$copy"; done
    if [[ "$ZEBKIT_HOST" == macos && -n "${ENGINE_TEST_REAL_KEYCHAIN:-}" ]]; then
        # Only the throwaway keys of this invocation's test executables.
        for identity in "$offscreen_key_identity" "$webauthn_key_identity"; do
            /usr/bin/security delete-generic-password -s "Zebkit Cookies: $identity" \
                -a "$identity" >/dev/null 2>&1 || true
        done
    fi
    if [[ -n "${ENGINE_TEST_KEEP_WORK:-}" ]]; then
        echo "kept: $work"
    else
        # On Windows the last test's engine children can hold its profile's
        # files for a moment after it ends: its result is the run's, not this.
        local tries=0
        until rm -rf "$work" 2>/dev/null || (( ++tries >= 30 )); do sleep 1; done
        [[ -e "$work" ]] && echo "note: $work is still in use; not removed" >&2
    fi
    return 0
}
trap cleanup EXIT

# The programs: built for this run, or built before. In a directory that moves,
# they find the engine from where they are.
bin="$work"
if [[ -n "$prebuilt" ]]; then
    bin="$(cd "$prebuilt" && pwd -P)" || die "no directory $prebuilt"
elif [[ -n "$build_only" ]]; then
    mkdir -p "$build_only/programs"
    build_only="$(cd "$build_only" && pwd -P)" || die "no directory $build_only"
    bin="$build_only/programs"
    origin='$ORIGIN'
    [[ "$ZEBKIT_TARGET" == macos ]] && origin='@executable_path'
    # programs/x and programs/tools/x.
    ZEBKIT_TARGET_RPATHS=("$origin/../out/blink/$profile/package" "$origin/../../out/blink/$profile/package")
fi
tests="$ZEBKIT_ROOT/tests/engine"
# Reserved names: a recompiled ad hoc executable cannot read an older binary's
# Keychain item without a prompt, so each gets disposable keys of its own.
offscreen_test="$bin/offscreen"
if [[ "$ZEBKIT_TARGET" == macos ]]; then
    offscreen_key_identity="zebkit-test-offscreen-$(basename "$bin" | tr -cd 'A-Za-z0-9.')"
    offscreen_test="$bin/$offscreen_key_identity"
fi
webauthn_key_identity="zebkit-test-webauthn-$(basename "$bin" | tr -cd 'A-Za-z0-9.')"
webauthn_host="$bin/$webauthn_key_identity"

# program <command...>: the build of a test program, whose output is after -o
# or build_against_package's second argument. For the whole suite and for
# --build-only it is built at once; with --only, when a selected test names it
# (need, from run).
program_outputs=()
program_commands=()
program() {
    local output= i
    if [[ "$1" == build_against_package ]]; then
        output="$3"
    else
        for (( i = 2; i <= $#; i++ )); do [[ "${!i}" == -o ]] && { i=$(( i + 1 )); output="${!i}"; }; done
    fi
    if [[ -z "$only" || -n "$build_only" || -z "$output" ]]; then
        "$@"
        return
    fi
    program_outputs+=("$output")
    program_commands+=("$(printf '%q ' "$@")")
}
# need <word...>: builds the programs not built yet that a word names: the
# program itself, or VAR=program, --option=program.
need() {
    (( ${#program_outputs[@]} )) || return 0
    local word i output
    for word in "$@"; do
        for i in "${!program_outputs[@]}"; do
            output="${program_outputs[i]}"
            [[ -n "$output" && ( "$word" == "$output" || "$word" == *"=$output" ) ]] || continue
            log "building ${output#"$bin"/}"
            eval "${program_commands[i]}" || die "${output#"$bin"/} did not build"
            program_outputs[i]=
        done
    done
}
# have_program <path>: it is built, or would be when a test needs it.
have_program() {
    local output
    [[ -x "$1" ]] && return 0
    for output in "${program_outputs[@]}"; do [[ "$output" == "$1" ]] && return 0; done
    return 1
}

if [[ -z "$prebuilt" ]]; then
log "building the test programs against $package"
program build_against_package "$package" "$bin/vendor" "$tests/vendor.c"
engine_link_options "$package"
program target_cxx -std=c++17 -Wall -Werror -I"$package/include" "$tests/vendor.cc" \
    "${engine_link[@]}" -o "$bin/vendor-cxx"
program build_against_package "$package" "$bin/services" "$tests/services.c"
program build_against_package "$package" "$bin/pdf" "$tests/pdf.c"
program build_against_package "$package" "$bin/canvas" "$tests/canvas.c"
program build_against_package "$package" "$bin/pdf-annotations" "$tests/pdf_annotations.c"
program build_against_package "$package" "$bin/pdf-hooks" "$tests/pdf_hooks.c" "$ZEBKIT_ROOT/examples/pdf-plugin/plugin.c" -- -D_GNU_SOURCE -I"$ZEBKIT_ROOT/examples/pdf-plugin"
program build_against_package "$package" "$bin/api" "$tests/api.c" -- -pthread
program target_cc -std=c11 -O1 -Wall -Werror -I"$package/include" "$tests/late_load.c" -ldl -pthread -o "$bin/late-load"
program build_against_package "$package" "$bin/lifecycle" "$tests/lifecycle.c" -- -pthread
program build_against_package "$package" "$bin/host-loop" "$tests/host_loop.c" -- -pthread
if [[ "$ZEBKIT_TARGET" == linux ]] && pkg-config --exists glib-2.0; then
    read -r -a glib_flags <<< "$(pkg-config --cflags --libs glib-2.0)"
    program build_against_package "$package" "$bin/host-loop-glib" "$tests/host_loop_glib.c" -- "${glib_flags[@]}" -pthread
fi
program build_against_package "$package" "$bin/startup-settings" "$tests/startup_settings.c"
program build_against_package "$package" "$bin/edit-commands" "$tests/edit_commands.c"
program build_against_package "$package" "$bin/hooks" "$tests/hooks.c"
program build_against_package "$package" "$bin/certificate" "$tests/certificate.c"
program build_against_package "$package" "$bin/http_auth" "$tests/http_auth.c"
program build_against_package "$package" "$bin/user_agent" "$tests/user_agent.c"
program build_against_package "$package" "$bin/hook-policies" "$tests/hook_policies.c"
program build_against_package "$package" "$bin/runtime-settings" "$tests/runtime_settings.c"
program build_against_package "$package" "$bin/offscreen-scale" "$tests/offscreen_scale.c"
program build_against_package "$package" "$bin/bridge" "$tests/bridge.c"
if [[ "$ZEBKIT_TARGET" == windows ]]; then
    # An application's own window and loop, and the sandbox with its executable
    # as the child (docs/WINDOWS.md); frames through file mappings and textures.
    program build_against_package "$package" "$bin/win-smoke" "$tests/win_smoke.c"
    program build_against_package "$package" "$bin/win-host" "$tests/win_host.c"
    program build_against_package "$package" "$bin/win-offscreen" "$tests/win_offscreen.c"
    program build_against_package "$package" "$bin/win-smoke-sandbox" "$tests/win_smoke.c" -- -DZEBKIT_TEST_SANDBOX "$package/zebkit_sandbox.lib" -Wl,/STACK:0x800000
    program build_against_package "$package" "$bin/win-host-sandbox" "$tests/win_host.c" -- -DZEBKIT_TEST_SANDBOX "$package/zebkit_sandbox.lib" -Wl,/STACK:0x800000
    program build_against_package "$package" "$bin/win-native-parent" "$tests/win_native_parent.c"
    program build_against_package "$package" "$bin/win-native-parent-sandbox" "$tests/win_native_parent.c" -- -DZEBKIT_TEST_SANDBOX "$package/zebkit_sandbox.lib" -Wl,/STACK:0x800000
    program target_cxx -std=c++20 -O1 -I"$package/include" "$tests/win_sandbox_stubs.cc" "$package/zebkit_sandbox.lib" \
        "${engine_link[@]}" -Wl,/STACK:0x800000 -o "$bin/win-sandbox-stubs"
fi
if [[ "$ZEBKIT_TARGET" == linux ]] && [[ -f /usr/include/X11/Xlib.h ]]; then
    program build_against_package "$package" "$bin/native-parent" "$tests/native_parent.c" -- -lX11 -ldl
    program build_against_package "$package" "$bin/native-window" "$tests/native_window.c" -- -pthread
    program build_against_package "$package" "$bin/mixed-views" "$tests/mixed_views.c" -- -DMIXED_X11 -lX11 -ldl
    program build_against_package "$package" "$bin/native-host-x11" "$ZEBKIT_ROOT/examples/native-host/x11.c" -- -lX11
fi
if [[ "$ZEBKIT_TARGET" == macos ]]; then
    program build_against_package "$package" "$bin/native-parent-mac" "$tests/native_parent_mac.m" -- -fobjc-arc -framework Cocoa
    program build_against_package "$package" "$bin/mixed-views" "$tests/mixed_views.c" -- -framework IOSurface
    program target_cxx -std=c++17 -fobjc-arc -O1 -Wall -Wextra -Wno-deprecated-declarations -I"$package/include" "$ZEBKIT_ROOT/examples/native-host/appkit.mm" \
        -framework Cocoa -framework IOSurface "${engine_link[@]}" -o "$bin/native-host-appkit"
fi
program build_against_package "$package" "$bin/scheme" "$tests/scheme.c"
offscreen_options=()
[[ "$ZEBKIT_TARGET" == macos ]] && offscreen_options=(-framework IOSurface)
program build_against_package "$package" "$offscreen_test" "$tests/offscreen.c" -- "${offscreen_options[@]}"
frames_options=()
[[ "$ZEBKIT_TARGET" == linux && -f /usr/include/X11/Xlib.h ]] && frames_options=(-DFRAMES_X11 -lX11 -ldl)
program build_against_package "$package" "$bin/frames-input" "$tests/frames_input.c" -- "${frames_options[@]}"
program build_against_package "$package" "$bin/page" "$tests/page.c"
program build_against_package "$package" "$bin/popup" "$tests/popup.c"
program build_against_package "$package" "$bin/cookie-host" "$tests/cookie_host.c"
program build_against_package "$package" "$bin/cookie-file-runner" "$ZEBKIT_ROOT/tools/zebkit-run/zebkit-run.c"
program build_against_package "$package" "$bin/automation" "$tests/automation.c"
program build_against_package "$package" "$bin/automation-reentry" "$tests/automation_reentry.c"
program build_against_package "$package" "$bin/media" "$tests/media.c"
program build_against_package "$package" "$bin/media-probe" "$tests/media_probe.c"
program build_against_package "$package" "$bin/mode" "$tests/mode.c"
program build_against_package "$package" "$webauthn_host" "$tests/webauthn_host.c"
render_options=(-pthread)
[[ "$ZEBKIT_TARGET" == macos ]] && render_options+=(-framework IOSurface)
# The example of another process's frames: Unix sockets and mach ports, not on Windows.
if [[ "$ZEBKIT_TARGET" != windows ]]; then
program build_against_package "$package" "$bin/offscreen-render" "$ZEBKIT_ROOT/examples/offscreen-host/render.c" -- "${render_options[@]}"
host_options=()
[[ "$ZEBKIT_TARGET" == macos ]] && host_options=(-framework CoreVideo -framework IOSurface -framework CoreFoundation)
program target_cc -std=c11 -O1 -Wall -Werror "$ZEBKIT_ROOT/examples/offscreen-host/show.c" "${host_options[@]}" -o "$bin/offscreen-show"
fi
program build_against_package "$package" "$bin/tools/zebkit-run" "$ZEBKIT_ROOT/tools/zebkit-run/zebkit-run.c"
# Built only, as an application builds it: its window is a person's to drag
# (examples/custom-titlebar; window_frame.py tests <zebkit/window.h> itself).
program build_against_package "$package" "$bin/custom-titlebar" "$ZEBKIT_ROOT/examples/custom-titlebar/main.c"
program build_against_package "$package" "$bin/tools/zebkit-js" "$ZEBKIT_ROOT/tools/zebkit-js/zebkit-js.c"
if [[ "$ZEBKIT_TARGET" == macos ]]; then
    keychain_source="$tests/macos_private_keychain.c"
    keychain_frameworks=(-framework Security -framework CoreFoundation)
    target_cc -std=c11 -O1 -Wall -Werror -dynamiclib "$keychain_source" "${keychain_frameworks[@]}" -o "$bin/macos-private-keychain.dylib"
    target_cc -std=c11 -O1 -Wall -Werror -DMACOS_KEYCHAIN_LAUNCH "$keychain_source" -o "$bin/macos-keychain-launch"
    target_cc -std=c11 -O1 -Wall -Werror -DMACOS_KEYCHAIN_CHECK "$keychain_source" "${keychain_frameworks[@]}" -o "$bin/macos-keychain-check"
fi
fi

# The engine's switches for a test; run gives each test a profile of its own.
engine_switches=(--no-sandbox --headless --user-data-dir="$work/profile")
failed=0
ran=0
# The names of the tests run, between spaces (bash 3.2 has no associative arrays).
ran_tests=" "
# What was not run: "<test>: <why>", or "<test>: <part>: <why>" for a part of
# one (its "skip <part>: <why>" and "narrowed: ..." lines, collected by run).
skipped=()
unsure=()
# ENGINE_TEST_EXPECTED_SKIPS: extended regular expressions, one a line, of
# skips that --strict accepts (scripts/check.sh's list); every other skip fails it.
expected_skips="${ENGINE_TEST_EXPECTED_SKIPS:-}"
export ENGINE_TEST_EXPECTED_SKIPS="$expected_skips"

selected() { [[ -z "$only" || "$1" =~ $only ]]; }

# expected_skip <entry>: an entry of skipped that the expected skips name.
expected_skip() {
    [[ -n "$expected_skips" ]] && grep -Eq -e "$expected_skips" <<< "$1"
}

# skip <reason> <name...>: those tests, the selected of them, were not run.
skip() {
    local reason="$1" name
    shift
    for name in "$@"; do
        selected "$name" && skipped+=("$name: $reason")
    done
    return 0
}

# mark_ran <name>: a test that has run, by run() or by hand.
mark_ran() {
    ran=$(( ran + 1 ))
    ran_tests+="$1 "
}
was_run() { [[ "$ran_tests" == *" $1 "* ]]; }

# finished <name> <what>: its log ends with every check passed, and at least one.
# Nothing for a test that was not selected or not run.
finished() {
    was_run "$1" || return 0
    result_checked+="$1 "
    grep -q '^\([1-9][0-9]*\) of \1 checks passed$' "$work/$1.log" || { echo "FAIL $2 did not finish"; failed=1; }
}

# finished_line <name> <regex> <what>: it printed its last line, which a
# script that ended early (zebkit-js ends once nothing is pending) has not.
# These calls are the registry of tests whose result is a line of their own
# rather than "N of N checks passed".
finished_line() {
    was_run "$1" || return 0
    result_checked+="$1 "
    grep -q "$2" "$work/$1.log" || { echo "FAIL $3 did not finish"; failed=1; }
}

# One result format (G19): a test run by run() that exits 0 has passed only if
# its log has "N of N checks passed" (N > 0) or finished_line checked its own
# last line. The tests below have neither yet: each prints its own "ok ..."
# lines, and exiting 0 is all that is checked of them: tests/engine/python_declarations.py,
# gpu_status.py's runs, the AppKit host of examples/native-host and Rust's
# examples/native_parent.rs. They are listed at the end of a run; a test not in
# this list that has neither fails. Extended regular expressions on the test's
# name; remove a name once its test prints the summary.
no_result_line_yet='^(gpu-status-(headless|window)|native-host-appkit|native-parent-rust|python-declarations)$'
run_tests=" "
failed_tests=" "
result_checked=" "

# collect_skips <name>: the parts of a test its log says were not run.
collect_skips() {
    local line
    while IFS= read -r line; do
        line="${line#skip }"
        skipped+=("$1: $line")
    done < <(grep -E '^(skip |narrowed:)' "$work/$1.log" || true)
}

# run <name> <command...>: a test program; its output, and the engine's on failure.
run() {
    local name="$1" arg args=() line words=()
    shift
    selected "$name" || return 0
    mark_ran "$name"
    run_tests+="$name "
    # A profile of its own: what one test leaves there is not the next one's.
    for arg in "$@"; do
        [[ "$arg" == "--user-data-dir=$work/profile" ]] && arg="--user-data-dir=$work/profile-$name"
        args+=("$arg")
    done
    set -- "${args[@]}"
    # The programs it names, and the environment it is given, built if they are not.
    if (( ${#program_outputs[@]} )); then
        while IFS= read -r line; do words+=("$line"); done < <(env)
        need "${launch[@]}" "$@" "${words[@]}"
    fi
    log "$name"
    if ! (
        # The name the expected skips are matched with (tests/engine/harness.py).
        export ENGINE_TEST_NAME="$name"
        # Protected programs clear DYLD_*, so env is done here, before the launcher.
        if (( ${#launch[@]} )) && [[ "$1" == env ]]; then
            shift
            while [[ "$1" == *=* ]]; do export "${1%%=*}=${1#*=}"; shift; done
        fi
        limited "${run_limit:-180}" "${launch[@]}" "$@"
    ) > "$work/$name.log" 2> "$work/$name.err"; then
        failed=1
        failed_tests+="$name "
        cat "$work/$name.log"
        echo "FAIL $name: it did not exit 0"
        echo "it printed on stderr:"; tail -20 "$work/$name.err"
        collect_skips "$name"
    else
        cat "$work/$name.log"
        collect_skips "$name"
        # A count of checks that is not all of them, or none, is not a pass.
        if grep -E '^[0-9]+ of [0-9]+ checks passed$' "$work/$name.log" | awk '$1 != $3 || $3 == 0 { bad = 1 } END { exit !bad }'; then
            echo "FAIL $name: not every check passed"; failed=1; failed_tests+="$name "
        elif grep -q '^FAIL' "$work/$name.log"; then
            unsure+=("$name")
            if [[ -n "$strict" ]]; then
                echo "FAIL $name: it exited 0 but printed FAIL lines"; failed=1
            else
                echo "note: $name exited 0 but printed FAIL lines"
            fi
        fi
    fi
}

# What the engine exports and loads, with the target's tools.
check_engine_library() {
    local nm_tool=(nm) otool_tool=(otool)
    if [[ "$ZEBKIT_TARGET" == macos && "$ZEBKIT_HOST" != macos ]]; then
        nm_tool=("$llvm_bin/llvm-nm"); otool_tool=("$llvm_bin/llvm-otool")
    fi
    log "what the engine exports"
    if [[ "$ZEBKIT_TARGET" == windows ]]; then
        # Chromium's own dllexports are there too: base's handle verifier and
        # crash hooks, and SQLite's recovery extension.
        exported="$("$llvm_bin/llvm-readobj" --coff-exports "$library" | awk '$1 == "Name:" { print $2 }' \
            | grep -vxE 'CrashForExceptionInNonABICompliantCodeRange|GetHandleVerifier|IsSandboxedProcess|sqlite3_dbdata_init')" \
            || die "the engine's exports could not be read"
    elif [[ "$ZEBKIT_TARGET" == macos ]]; then
        # Mach-O puts an underscore before every C name.
        exported="$("${nm_tool[@]}" -gU "$library" | awk '{ print substr($3, 2) }')" || die "the engine's exports could not be read"
    else
        exported="$("${nm_tool[@]}" -D --defined-only "$library" | awk '{ print $3 }')" || die "the engine's exports could not be read"
    fi
    foreign="$(grep -v '^zebkit_' <<< "$exported" | grep -v '^$' || true)"
    if [[ -n "$foreign" ]]; then
        echo "FAIL exports names that are not zebkit's: $(head -5 <<< "$foreign" | paste -sd' ' -)"; failed=1
    else
        echo "ok   $(wc -l <<< "$exported") names, all zebkit_*: $(grep -c '^zebkit_vendor_' <<< "$exported") of the libraries'"
    fi

    log "what the engine loads"
    if [[ "$ZEBKIT_TARGET" == windows ]]; then
        # Not Microsoft's redistributable runtime: the C runtime is linked in.
        stray="$("$llvm_bin/llvm-readobj" --coff-imports "$library" | awk '$1 == "Name:" && $2 ~ /\.[dD][lL][lL]$/ { print $2 }' \
            | grep -iE '^(vcruntime|msvcp|ucrtbase|concrt|api-ms-win-crt|d3dcompiler)' || true)"
    elif [[ "$ZEBKIT_TARGET" == macos ]]; then
        # The system's frameworks and libraries, and the framework itself.
        loads="$("${otool_tool[@]}" -L "$library" | tail -n +2 | awk '{ print $1 }')" || die "what the engine loads could not be read"
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
}
# Cargo's builds for the suite, kept from run to run: this profile's package
# is what the crates link.
cargo_dir="$OUT_DIR/engine-test/$profile/cargo"
# The Rust examples the suite runs: the hook and settings programs, and the Rust
# halves of the feature drivers (http_auth.py, certificate.py, user_agent.py).
rust_examples=(hook_policies runtime_settings certificate_policy http_auth user_agent console popup)
# Go, from the PATH or mise's pin (mise.toml), for the Go binding's checks.
go_cmd=()
if (cd "$ZEBKIT_ROOT" && command -v go > /dev/null && go version > /dev/null 2>&1); then go_cmd=(go)
elif command -v mise > /dev/null && (cd "$ZEBKIT_ROOT" && mise which go > /dev/null 2>&1); then go_cmd=(mise exec -C "$ZEBKIT_ROOT" -- go); fi

if [[ -n "$build_only" ]]; then
    check_engine_library
    # What the PDF checks read (python_parity.py, Go's check_pdf), from PDFium's
    # tests: the bundle has no Chromium tree.
    pdfium_resources="$SRC_DIR/third_party/pdfium/testing/resources"
    mkdir -p "$bin/pdf-resources/fonts/ahem"
    cp "$pdfium_resources/hello_world.pdf" "$pdfium_resources/bookmarks.pdf" "$pdfium_resources/text_form.pdf" "$bin/pdf-resources/"
    cp "$pdfium_resources/fonts/ahem/Ahem.ttf" "$bin/pdf-resources/fonts/ahem/"
    # The Go binding's checks for the bundle's system: Windows with zebkit-cc
    # (clang for MSVC, the sandbox library linked), macOS with the SDK. They run
    # from the package, beside the engine, as an application does.
    if [[ ${#go_cmd[@]} -gt 0 && "$ZEBKIT_TARGET" != linux ]]; then
        log "building the Go binding's checks for $ZEBKIT_TARGET"
        go_env=(CGO_ENABLED=1 CGO_CFLAGS="-I$package/include")
        if [[ "$ZEBKIT_TARGET" == windows ]]; then
            (cd "$ZEBKIT_ROOT/bindings/golang" && "${go_cmd[@]}" build -o "$work/zebkit-cc" ./cmd/zebkit-cc) \
                || die "zebkit-cc did not build"
            go_env+=(GOOS=windows GOARCH=amd64 CC="$work/zebkit-cc" ZEBKIT_CLANG="$llvm_bin/clang"
                ZEBKIT_CC_FLAGS="-Xmicrosoft-windows-sys-root $windows_sysroot"
                CGO_LDFLAGS="$package/zebkit.dll.lib $package/zebkit_sandbox.lib -Wl,/STACK:0x800000")
        else
            mkdir -p "$work/go-bin"
            ln -sf "$llvm_bin/llvm-strip" "$work/go-bin/strip"
            go_env+=(GOOS=darwin GOARCH=arm64 PATH="$work/go-bin:$PATH" CC="$llvm_bin/clang --target=arm64-apple-macos13 -isysroot $macos_sdk"
                CGO_LDFLAGS="-fuse-ld=lld -F$package")
        fi
        go_exe= go_ldflags="-s -w -extldflags=-Wl,-rpath,@executable_path"
        [[ "$ZEBKIT_TARGET" == windows ]] && go_exe=.exe go_ldflags="-s -w"
        for go_check in check check_hooks check_pdf check_automation; do
            (cd "$ZEBKIT_ROOT/bindings/golang" && env "${go_env[@]}" "${go_cmd[@]}" build -ldflags="$go_ldflags" \
                -o "$bin/go-$go_check$go_exe" "./internal/$go_check" 2>&1 | grep -v 'longer than 8 characters' >&2
                [[ -f "$bin/go-$go_check$go_exe" ]]) || die "the Go binding's $go_check did not build for $ZEBKIT_TARGET"
        done
    fi
    # The Rust hook program, if this machine's toolchain has the target.
    rust_target=
    [[ "$ZEBKIT_TARGET" == macos && "$ZEBKIT_HOST" != macos ]] && rust_target=aarch64-apple-darwin
    [[ "$ZEBKIT_TARGET" == windows ]] && rust_target=x86_64-pc-windows-msvc
    installed_targets=
    [[ -n "$rust_target" ]] && installed_targets="$(rustup target list --installed 2> /dev/null || true)"
    if command -v cargo > /dev/null && { [[ -z "$rust_target" ]] || grep -qx "$rust_target" <<< "$installed_targets"; }; then
        log "building the Rust hook program"
        rust_options=()
        rust_env=(ZEBKIT_DIR="$package" CARGO_TARGET_DIR="$cargo_dir/bundle-${rust_target:-host}")
        rust_flags="-C link-arg=-Wl,-rpath,$origin/../../out/blink/$profile/package"
        if [[ "$ZEBKIT_TARGET" == windows ]]; then
            rust_options=(--target "$rust_target")
            rust_env+=(CARGO_TARGET_X86_64_PC_WINDOWS_MSVC_LINKER="$llvm_bin/lld-link")
            rust_flags="-C link-arg=/winsysroot:$windows_sysroot"
        elif [[ -n "$rust_target" ]]; then
            printf '#!/bin/sh\nexec "%s" --target=arm64-apple-macos13 -isysroot "%s" -fuse-ld=lld "$@" -Wno-unused-command-line-argument\n' \
                "$llvm_bin/clang" "$macos_sdk" > "$work/rust-linker"
            chmod +x "$work/rust-linker"
            rust_options=(--target "$rust_target")
            rust_env+=(CARGO_TARGET_AARCH64_APPLE_DARWIN_LINKER="$work/rust-linker")
        fi
        mkdir -p "$bin/rust"
        (cd "$ZEBKIT_ROOT/bindings/rust" && env "${rust_env[@]}" RUSTFLAGS="$rust_flags" \
            cargo build -q -p zebkit $(printf -- '--example %s ' "${rust_examples[@]}") "${rust_options[@]}") \
            || die "the Rust hook program did not build"
        rust_exe=
        [[ "$ZEBKIT_TARGET" == windows ]] && rust_exe=.exe
        for rust_example in "${rust_examples[@]}"; do
            cp "$cargo_dir/bundle-${rust_target:-host}/${rust_target:+$rust_target/}debug/examples/$rust_example$rust_exe" "$bin/rust/$rust_example$rust_exe"
        done
    else
        log "hook-rust: not built (no cargo or no ${rust_target:-host} target)"
    fi
    (( failed )) && die "the engine's checks failed"
    log "copying what the suite needs into $build_only"
    mkdir -p "$build_only/out/blink/$profile"
    rm -rf "$build_only/out/blink/$profile/package"
    cp -a "$package" "$build_only/out/blink/$profile/package"
    ahem_relative="${SRC_DIR#"$ZEBKIT_ROOT"/}/third_party/pdfium/testing/resources/fonts/ahem/Ahem.ttf"
    # On Linux the hooks' certstore group builds tests/engine/nss_import.c where it
    # runs, with the sysroot's NSS headers when the system has none (hook_policies.py).
    nss_headers=()
    nss_include="${SRC_DIR#"$ZEBKIT_ROOT"/}/build/linux/debian_bullseye_amd64-sysroot/usr/include"
    [[ "$ZEBKIT_TARGET" == linux && -d "$ZEBKIT_ROOT/$nss_include/nss" ]] && nss_headers=("$nss_include/nss" "$nss_include/nspr")
    # The set-user-ID helper's test (the sandbox-helper entry): its part as any
    # user, and as root on a test machine (it builds zebkit-run there).
    sandbox_helper=()
    [[ "$ZEBKIT_TARGET" == linux ]] && sandbox_helper=(scripts/sandbox-helper-test.sh tools/zebkit-run/zebkit-run.c)
    tar -C "$ZEBKIT_ROOT" --exclude=__pycache__ -cf - scripts/engine-test.sh scripts/lib.sh upstream.lock profiles tests/serve.py \
        tests/engine bindings/python examples/pdf-plugin examples/native-host/sample.pdf examples/ui/todo.mjs "$ahem_relative" \
        ${nss_headers[@]+"${nss_headers[@]}"} ${sandbox_helper[@]+"${sandbox_helper[@]}"} | tar -C "$build_only" -xf -
    if [[ -f "$real_pdf" ]]; then
        mkdir -p "$build_only/.cache/pdf"
        cp "$real_pdf" "$build_only/.cache/pdf/"
    fi
    log "built: run $build_only/scripts/engine-test.sh $profile --prebuilt $build_only/programs where they run"
    exit 0
fi
if [[ -z "$prebuilt" ]]; then
    check_engine_library
    # The registry of surfaces against Chromium's delegates and src/engine
    # (docs/SYSTEM_INTEGRATION.md, 6.6).
    if selected surfaces; then
        log "surfaces"
        mark_ran surfaces
        "${python_run[@]}" "$ZEBKIT_ROOT/scripts/check-surfaces.py" --chromium "$SRC_DIR" || { echo "FAIL the surfaces' registry"; failed=1; }
    fi
    # Every function of the C API in every binding (scripts/check-bindings.py).
    if selected check-bindings; then
        log "check-bindings"
        mark_ran check-bindings
        "${python_run[@]}" "$ZEBKIT_ROOT/scripts/check-bindings.py" || { echo "FAIL a binding misses functions of the C API"; failed=1; }
    fi
else
    log "what the engine exports and loads: checked by --build-only"
fi

# On Windows a DLL is found on PATH; python3 there may be the Store's stub.
if [[ "$ZEBKIT_HOST" == windows ]]; then
    # A copy of python.exe under that name, with the real one's library and a
    # pyvenv.cfg to find its standard library by: a script of that name is not a
    # program on a shared folder.
    mkdir -p "$work/path"
    python_home="$(python -c 'import sys; print(sys.base_prefix)' | tr -d '\r')" || die "no Python (python) on PATH"
    python_dll="$(python -c 'import sys; print("python%d%d" % sys.version_info[:2])' | tr -d '\r')" || die "no Python (python) on PATH"
    python_home="$(cygpath -u "$python_home")" || die "cygpath did not convert $python_home"
    cp "$python_home/python.exe" "$work/path/python3.exe"
    cp "$python_home/$python_dll.dll" "$python_home/python3.dll" "$python_home/vcruntime140.dll" "$work/path/"
    [[ -f "$python_home/vcruntime140_1.dll" ]] && cp "$python_home/vcruntime140_1.dll" "$work/path/"
    echo "home = $(cygpath -w "$python_home")" > "$work/path/pyvenv.cfg"
    # For every test, -I or not: output in UTF-8 whatever the system's code page, and
    # a temporary profile left behind if a child still has a file open as the browser
    # ends, where Unix would remove it.
    # (The prefix of a Python with pyvenv.cfg beside it is the directory above.)
    mkdir -p "$work/Lib/site-packages"
    cat > "$work/Lib/site-packages/sitecustomize.py" <<'PY'
import functools, sys, tempfile
for stream in (sys.stdout, sys.stderr):
    stream.reconfigure(encoding="utf-8", errors="backslashreplace")
tempfile.TemporaryDirectory = functools.partial(tempfile.TemporaryDirectory, ignore_cleanup_errors=True)
PY
    # Temporary files stay in the test's directory.
    mkdir -p "$work/tmp"
    export TEMP="$(cygpath -w "$work/tmp")" TMP="$(cygpath -w "$work/tmp")" TMPDIR="$work/tmp"
    # A default profile too, of a test that names none; checked at the end.
    real_local_app_data="$LOCALAPPDATA"
    mkdir -p "$work/local"
    export LOCALAPPDATA="$(cygpath -w "$work/local")"
    zebkit_before="$( (ls -A "$real_local_app_data/zebkit" 2> /dev/null || true) | wc -l)"
    export PATH="$work/path:$package:$PATH"
fi

# A person's Mac: their files stay theirs, and their Keychain is not asked.
launch=()
if [[ "$ZEBKIT_HOST" == macos ]]; then
    mkdir -p "$work/path" "$work/home" "$work/keychain" "$work/tmp"
    export TMPDIR="$work/tmp" MAC_CHROMIUM_TMPDIR="$work/tmp"
    ln -s "$("${python_run[@]}" -c 'import os, sys; print(os.path.realpath(sys.executable))')" "$work/path/python3"
    export PATH="$work/path:$PATH"
    export HOME="$work/home" CFFIXED_USER_HOME="$work/home"
    if [[ -z "${ENGINE_TEST_REAL_KEYCHAIN:-}" ]]; then
        export ZEBKIT_TEST_KEYCHAIN_LIBRARY="$bin/macos-private-keychain.dylib" ZEBKIT_TEST_KEYCHAIN_DIR="$work/keychain"
        launch=("$bin/macos-keychain-launch")
        # Nothing is run until the calls are seen going to the files, from a
        # program and from Python, which the Python tests start the engine in.
        printf 'check' > "$work/keychain/$(printf 'zebkit-test-check' | od -An -tx1 | tr -d ' \n')-$(printf 'zebkit-test-check' | od -An -tx1 | tr -d ' \n')"
        limited 30 "${launch[@]}" "$bin/macos-keychain-check" > /dev/null \
            || die "the private Keychain is not in effect in a program; stopping before the login Keychain is touched"
        [[ "$(limited 30 "${launch[@]}" "${python_run[@]}" -c 'import os; print(os.environ.get("DYLD_INSERT_LIBRARIES", ""))')" == "$ZEBKIT_TEST_KEYCHAIN_LIBRARY" ]] \
            || die "$(command -v python3) drops DYLD_INSERT_LIBRARIES (a protected Python?); another Python on PATH, or ENGINE_TEST_REAL_KEYCHAIN=1"
    fi
fi

run vendor "$bin/vendor" "$SRC_DIR/third_party/pdfium/testing/resources/fonts/ahem/Ahem.ttf"
run vendor-cxx "$bin/vendor-cxx"
run services "$bin/services"
run pdf "$bin/pdf"
# Ahem: every glyph a box of one em, which makes text's pixels certain.
ahem="$SRC_DIR/third_party/pdfium/testing/resources/fonts/ahem/Ahem.ttf"
run canvas "$bin/canvas" "$ahem"
run pdf-annotations "$bin/pdf-annotations" "$ahem"
run pdf-hooks "$bin/pdf-hooks" "$ahem"
run pdf-bindings env ZEBKIT_DIR="$package" PYTHONPATH="$ZEBKIT_ROOT/bindings/python" \
    python3 "$tests/pdf_bindings.py" "$ahem"
run api "$bin/api" "${engine_switches[@]}"
run lifecycle "$bin/lifecycle" --no-sandbox --headless --user-data-dir="$work/profile-lifecycle"
run host-loop "$bin/host-loop" --no-sandbox --headless --user-data-dir="$work/profile-host-loop"
run edit-commands "$bin/edit-commands" --no-sandbox --headless --user-data-dir="$work/profile-edit-commands"
if have_program "$bin/host-loop-glib"; then
    run host-loop-glib "$bin/host-loop-glib" --no-sandbox --headless --user-data-dir="$work/profile-host-loop-glib"
elif [[ "$ZEBKIT_TARGET" == linux ]]; then
    skip "no glib-2.0 to build it with" host-loop-glib
fi
mkdir -p "$work/startup-settings-files"
ZEBKIT_TEST_SETTINGS_DIR="$work/startup-settings-files" run startup-settings "$bin/startup-settings" --user-agent=argv-agent --user-data-dir="$work/unused-argv-profile"
for log_mode in none file; do
    run "startup-settings-python-$log_mode" env ZEBKIT_DIR="$package" PYTHONPATH="$ZEBKIT_ROOT/bindings/python" \
        ZEBKIT_TEST_LOG_MODE="$log_mode" python3 "$tests/startup_settings_python.py"
done
# The tests in a window of a private X server.
x11_tests=(responsiveness-x11 native-parent mixed-views mixed-views-scaled native-window-x11 native-window-javascript sign-in-x11 input-method-x11 js-dialogs-x11 context-menu-x11 menu-state-x11 window-frame-x11 system-holes-window native-parent-python native-parent-rust)
if have_program "$bin/native-window"; then
    run native-window "$bin/native-window" --no-sandbox --headless --user-data-dir="$work/profile-native-window"
fi
if [[ "$ZEBKIT_TARGET" == linux ]] && ! have_program "$bin/native-parent"; then
    skip "no X11 headers to build it with" native-window native-parent-refused "${x11_tests[@]}"
    skip "no X11 headers" native-window-wayland native-window-javascript-wayland window-frame-wayland
fi
x11_selected=
for x11_test in "${x11_tests[@]}"; do selected "$x11_test" && x11_selected=1; done
if have_program "$bin/native-parent"; then
    run native-parent-refused "$bin/native-parent" --no-sandbox --headless --user-data-dir="$work/profile-native-parent-refused"
    if [[ -n "$x11_selected" ]] && command -v Xvfb > /dev/null; then
        exec 9< <(Xvfb -displayfd 1 -screen 0 2200x1400x24 -nolisten tcp 2> /dev/null)
        xvfb=$!
        pids+=("$xvfb")
        read -r -t 15 -u 9 xvfb_display || xvfb_display=
        if [[ -n "$xvfb_display" ]]; then
            # The same of a window and of a view attached to one.
            run responsiveness-x11 env -u WAYLAND_DISPLAY DISPLAY=":$xvfb_display" ZEBKIT_DIR="$package" "${python_run[@]}" \
                "$tests/responsiveness.py" --no-sandbox --user-data-dir="$work/profile-responsiveness-x11"
            run native-parent env -u WAYLAND_DISPLAY DISPLAY=":$xvfb_display" "$bin/native-parent" \
                --no-sandbox --user-data-dir="$work/profile-native-parent"
            run mixed-views env -u WAYLAND_DISPLAY DISPLAY=":$xvfb_display" "$bin/mixed-views" \
                --no-sandbox --user-data-dir="$work/profile-mixed-views"
            run mixed-views-scaled env -u WAYLAND_DISPLAY DISPLAY=":$xvfb_display" MIXED_SCALED=2 "$bin/mixed-views" \
                --force-device-scale-factor=2 --no-sandbox --user-data-dir="$work/profile-mixed-scaled"
            run native-window-x11 env -u WAYLAND_DISPLAY DISPLAY=":$xvfb_display" ZEBKIT_TEST_NATIVE_WINDOW_X11=1 \
                "$bin/native-window" --no-sandbox --user-data-dir="$work/profile-native-window-x11"
            run native-window-javascript env -u WAYLAND_DISPLAY DISPLAY=":$xvfb_display" "$bin/tools/zebkit-js" \
                --no-sandbox --user-data-dir="$work/profile-native-window-js" "$tests/script/native_window.mjs" x11
            finished native-window-javascript "the native window script (X11)"
            run sign-in-x11 env -u WAYLAND_DISPLAY DISPLAY=":$xvfb_display" ZEBKIT_DIR="$package" \
                "${python_run[@]}" "$tests/sign_in.py" --no-sandbox --user-data-dir="$work/profile-sign-in-x11"
            # A real input method in an X11 window: IBus, a fake one on its own bus.
            if command -v dbus-run-session > /dev/null && python3 -c "import dbus.mainloop.glib" 2> /dev/null; then
                run input-method-x11 env -u WAYLAND_DISPLAY -u IBUS_ADDRESS DISPLAY=":$xvfb_display" ZEBKIT_DIR="$package" \
                    dbus-run-session -- "${python_run[@]}" "$tests/input_method.py" --no-sandbox --user-data-dir="$work/profile-input-method"
            else
                skip "no dbus-run-session or dbus-python" input-method-x11
            fi
            run js-dialogs-x11 env -u WAYLAND_DISPLAY DISPLAY=":$xvfb_display" ZEBKIT_DIR="$package" \
                "${python_run[@]}" "$tests/js_dialogs.py" --no-sandbox --user-data-dir="$work/profile-js-dialogs-x11"
            run context-menu-x11 env -u WAYLAND_DISPLAY DISPLAY=":$xvfb_display" ZEBKIT_DIR="$package" \
                "${python_run[@]}" "$tests/context_menu.py" --no-sandbox --user-data-dir="$work/profile-context-menu-x11"
            run menu-state-x11 env -u WAYLAND_DISPLAY DISPLAY=":$xvfb_display" ZEBKIT_DIR="$package" \
                "${python_run[@]}" "$tests/menu_state.py" --no-sandbox --user-data-dir="$work/profile-menu-x11"
            run window-frame-x11 env -u WAYLAND_DISPLAY DISPLAY=":$xvfb_display" ZEBKIT_DIR="$package" \
                "${python_run[@]}" "$tests/window_frame.py" --no-sandbox --user-data-dir="$work/profile-window-frame-x11"
            run system-holes-window env -u WAYLAND_DISPLAY DISPLAY=":$xvfb_display" ZEBKIT_DIR="$package" \
                "${python_run[@]}" "$tests/system_holes.py" --no-sandbox --user-data-dir="$work/profile-holes-window"
            run native-parent-python env -u WAYLAND_DISPLAY DISPLAY=":$xvfb_display" ZEBKIT_DIR="$package" \
                PYTHONPATH="$ZEBKIT_ROOT/bindings/python" python3 "$tests/native_parent_python.py" \
                --no-sandbox --user-data-dir="$work/profile-native-parent-python"
            if ! selected native-parent-rust; then
                :
            elif [[ -n "$prebuilt" ]]; then
                skip "not in the bundle (--prebuilt): run from a checkout" native-parent-rust
            elif command -v cargo > /dev/null; then
                if (cd "$ZEBKIT_ROOT/bindings/rust" && ZEBKIT_DIR="$package" CARGO_TARGET_DIR="$cargo_dir/host" cargo build -q -p zebkit --example native_parent); then
                    run native-parent-rust env -u WAYLAND_DISPLAY DISPLAY=":$xvfb_display" LD_LIBRARY_PATH="$package" \
                        "$cargo_dir/host/debug/examples/native_parent" \
                        --no-sandbox --user-data-dir="$work/profile-native-parent-rust"
                else
                    echo "FAIL the Rust native parent program did not build"; failed=1
                fi
            else
                skip "no cargo" native-parent-rust
            fi
        else
            echo "FAIL Xvfb did not start"; failed=1
        fi
        kill "$xvfb" 2>/dev/null || true
        exec 9<&-
    elif [[ -n "$x11_selected" ]]; then
        skip "no Xvfb" "${x11_tests[@]}"
    fi
fi
# Wayland: a headless wlroots compositor of its own (labwc, no GPU), with an
# empty configuration so that nothing of the user's starts in it.
if have_program "$bin/native-window" && [[ "$ZEBKIT_HOST" == linux ]] && command -v labwc > /dev/null &&
    { selected native-window-wayland || selected native-window-javascript-wayland || selected window-frame-wayland; }; then
    wayland_run="$work/wayland-run"
    mkdir -p "$wayland_run" "$work/wayland-config" && chmod 700 "$wayland_run"
    env -i HOME="$work" PATH=/usr/bin:/bin XDG_RUNTIME_DIR="$wayland_run" XDG_CONFIG_HOME="$work/wayland-config" \
        WLR_BACKENDS=headless WLR_RENDERER=pixman WLR_HEADLESS_OUTPUTS=1 WLR_LIBINPUT_NO_DEVICES=1 \
        labwc -C "$work/wayland-config" > "$work/labwc.log" 2>&1 &
    labwc=$!
    pids+=("$labwc")
    # Slower to start on a loaded machine: up to 20 s.
    for _ in $(seq 200); do [[ -S "$wayland_run/wayland-0" ]] && break; sleep 0.1; done
    if [[ -S "$wayland_run/wayland-0" ]]; then
        # A keyboard on the compositor's seat: without one labwc gives no window
        # the focus, which window_frame.py's window actions need. wtype's virtual
        # keyboard, held (it would press Shift an hour later) until the tests end.
        wayland_keyboard=
        if command -v wtype > /dev/null; then
            env -u DISPLAY XDG_RUNTIME_DIR="$wayland_run" WAYLAND_DISPLAY=wayland-0 wtype -s 3600000 -k Shift_L \
                > /dev/null 2>&1 &
            wayland_keyboard=$!
            pids+=("$wayland_keyboard")
            sleep 0.5
        fi
        run native-window-wayland env -u DISPLAY XDG_RUNTIME_DIR="$wayland_run" WAYLAND_DISPLAY=wayland-0 \
            ZEBKIT_TEST_NATIVE_WINDOW_WAYLAND=1 "$bin/native-window" --no-sandbox --ozone-platform=wayland \
            --user-data-dir="$work/profile-native-window-wayland"
        run native-window-javascript-wayland env -u DISPLAY XDG_RUNTIME_DIR="$wayland_run" WAYLAND_DISPLAY=wayland-0 \
            "$bin/tools/zebkit-js" --no-sandbox --ozone-platform=wayland \
            --user-data-dir="$work/profile-native-window-js-wayland" "$tests/script/native_window.mjs" wayland
        finished native-window-javascript-wayland "the native window script (Wayland)"
        run window-frame-wayland env -u DISPLAY XDG_RUNTIME_DIR="$wayland_run" WAYLAND_DISPLAY=wayland-0 \
            ZEBKIT_DIR="$package" "${python_run[@]}" "$tests/window_frame.py" --no-sandbox --ozone-platform=wayland \
            --user-data-dir="$work/profile-window-frame-wayland"
        [[ -z "$wayland_keyboard" ]] || kill "$wayland_keyboard" 2> /dev/null || true
    else
        echo "FAIL labwc did not start"; tail -5 "$work/labwc.log"; failed=1
    fi
    kill "$labwc" 2>/dev/null || true
elif [[ "$ZEBKIT_HOST" == linux ]] && ! command -v labwc > /dev/null; then
    skip "no labwc" native-window-wayland native-window-javascript-wayland window-frame-wayland
fi
# Windows on the screen, and the keyboard for a moment: only when asked, on a person's Mac.
if [[ "$ZEBKIT_HOST" == macos ]]; then
    run gpu-status-headless python3 "$tests/gpu_status.py" "$bin/tools/zebkit-run" "${engine_switches[@]}"
    run native-parent-refused "$bin/native-parent-mac" --no-sandbox --headless --user-data-dir="$work/profile-native-parent-refused"
    if [[ -n "${ENGINE_TEST_WINDOWS:-}" ]]; then
        # Since Chromium 157 a window that others cover is hidden on macOS 26
        # and later too, as in Chrome; on a Mac whose screen is locked, or
        # under a full-screen terminal, all of the test's are. The test is of
        # what zebkit hides, not of what covers the screen: Chromium's own
        # switch turns that off for it.
        run mixed-views "$bin/mixed-views" --no-sandbox --user-data-dir="$work/profile-mixed-views" \
            --enable-features=MacOcclusionForceShutoff
        finished mixed-views "the mixed views' test"
        # The same for these three: under a locked or covered screen macOS calls
        # their windows occluded, and Chromium then hides the page and aborts its pickers.
        run native-parent "$bin/native-parent-mac" --no-sandbox --user-data-dir="$work/profile-native-parent" \
            --enable-features=MacOcclusionForceShutoff
        finished native-parent "the attached view's test"
        run gpu-status-window python3 "$tests/gpu_status.py" "$bin/tools/zebkit-run" --no-sandbox --user-data-dir="$work/profile-gpu-window"
        # <zebkit/window.h> in AppKit windows, the system's actions included.
        run window-frame-mac env ZEBKIT_DIR="$package" "${python_run[@]}" "$tests/window_frame.py" \
            --no-sandbox --user-data-dir="$work/profile-window-frame-mac" \
            --enable-features=MacOcclusionForceShutoff
        run system-holes-mac env ZEBKIT_DIR="$package" "${python_run[@]}" "$tests/system_holes.py" \
            --no-sandbox --user-data-dir="$work/profile-holes-mac" \
            --enable-features=MacOcclusionForceShutoff
        # The AppKit host, its application and run loop its own: the panes' picture.
        run native-host-appkit "$bin/native-host-appkit" --no-sandbox --user-data-dir="$work/profile-appkit" \
            --url="data:text/html,<body style='background:%2312a456'>" --pdf="$ZEBKIT_ROOT/examples/native-host/sample.pdf" \
            --smoke-test="$work/appkit.png"
        # The page's green, through the frames' IOSurfaces into the host's own drawing;
        # in the display's colours, so green above the others rather than an exact value.
        if selected native-host-appkit && ! awk '/centre:/ { found = $5 > $4 + 40 && $5 > $6 + 30 } END { exit !found }' "$work/native-host-appkit.log"; then
            echo "FAIL the AppKit host's web pane is not the page's colour"; failed=1
        fi
    else
        log "native-parent: skipped (it needs windows: ENGINE_TEST_WINDOWS=1)"
    fi
fi
if [[ "$ZEBKIT_HOST" == windows ]]; then
    # A sandboxed child has the environment's PATH no more: zebkit.dll beside the
    # executable, as an application has it.
    need "$bin/win-smoke-sandbox" "$bin/win-host-sandbox" "$bin/win-native-parent-sandbox"
    cp "$bin/win-smoke-sandbox.exe" "$bin/win-host-sandbox.exe" "$bin/win-native-parent-sandbox.exe" "$package/"
    package_copies+=("$package/win-smoke-sandbox.exe" "$package/win-host-sandbox.exe" "$package/win-native-parent-sandbox.exe")
    run win-smoke "$bin/win-smoke" --no-sandbox --user-data-dir="$work/profile-win-smoke" "$work/win-smoke.png"
    run win-smoke-sandbox "$package/win-smoke-sandbox" --user-data-dir="$work/profile-win-smoke-sandbox" "$work/win-smoke-sandbox.png"
    run win-host "$bin/win-host" --no-sandbox --user-data-dir="$work/profile-win-host"
    run win-host-sandbox "$package/win-host-sandbox" --user-data-dir="$work/profile-win-host-sandbox"
    run win-offscreen "$bin/win-offscreen" --no-sandbox --headless --user-data-dir="$work/profile-win-offscreen"
    if selected win-sandbox-stubs; then
        log win-sandbox-stubs
        mark_ran win-sandbox-stubs
        stub_code=0
        need "$bin/win-sandbox-stubs"
        cp "$bin/win-sandbox-stubs.exe" "$package/"
        package_copies+=("$package/win-sandbox-stubs.exe")
        "$package/win-sandbox-stubs" > "$work/win-sandbox-stubs.log" 2>&1 || stub_code=$?
        if grep -q '^returned' "$work/win-sandbox-stubs.log" || ! grep -q 'reached' "$work/win-sandbox-stubs.log"; then
            echo "FAIL a stub in zebkit_sandbox.lib did not end the process: code $stub_code"; failed=1
        else
            echo "ok   a stub in zebkit_sandbox.lib ends the process"
        fi
    fi
    # Takes the mouse and the keyboard: only when asked.
    if [[ -n "${ENGINE_TEST_WINDOWS:-}" ]]; then
        run win-webauthn python3 "$tests/win_webauthn.py" "$webauthn_host" --no-sandbox
        run win-native-parent "$bin/win-native-parent" --no-sandbox --user-data-dir="$work/profile-win-native-parent"
        # <zebkit/window.h> in Win32 windows, the system's actions included.
        run window-frame-win env ZEBKIT_DIR="$package" "${python_run[@]}" "$tests/window_frame.py" \
            --no-sandbox --user-data-dir="$work/profile-window-frame-win"
        run system-holes-win env ZEBKIT_DIR="$package" "${python_run[@]}" "$tests/system_holes.py" \
            --no-sandbox --user-data-dir="$work/profile-holes-win"
        finished win-native-parent "the attached view's test"
        run win-native-parent-sandbox "$package/win-native-parent-sandbox" --user-data-dir="$work/profile-win-native-parent-sandbox"
        finished win-native-parent-sandbox "the attached view's test, in the sandbox"
    fi
fi
run hooks "$bin/hooks" --no-sandbox --headless --user-data-dir="$work/profile-hooks"
run hooks-startup-policy "$bin/hooks" --no-sandbox --headless --disable-popup-blocking --user-data-dir="$work/profile-hooks-startup"
run lifecycle-python env ZEBKIT_DIR="$package" PYTHONPATH="$ZEBKIT_ROOT/bindings/python" \
    python3 "$tests/lifecycle_python.py" --no-sandbox --headless --user-data-dir="$work/profile-lifecycle-python"
run media-inventory env ZEBKIT_DIR="$package" PYTHONPATH="$ZEBKIT_ROOT/bindings/python" \
    python3 "$tests/media_inventory.py" --no-sandbox --headless --js-flags=--expose-gc --user-data-dir="$work/profile-media-inventory"
run mode-headless "$bin/mode" "${engine_switches[@]}"
run mode-automation "$bin/mode" --no-sandbox --headless --enable-automation \
    --expect-automation --user-data-dir="$work/profile-mode-automation"
run mode-driver-port "$bin/mode" --no-sandbox --headless --remote-debugging-port=0 \
    --expect-automation --user-data-dir="$work/profile-driver-port"

# The pages, on a port that was free: another run's server is not this one.
start_page_server "$tests/pages" "$work/server.log"
pids+=("$page_server")
port="$page_port"
mkdir -p "$work/files"
ZEBKIT_TEST_BASE="http://127.0.0.1:$port" ZEBKIT_TEST_OTHER="http://localhost:$port" \
    ZEBKIT_TEST_FILES="$work/files" run bridge "$bin/bridge" "${engine_switches[@]}"

ZEBKIT_TEST_BASE="http://127.0.0.1:$port" ZEBKIT_TEST_SHOTS="${ENGINE_TEST_SHOTS:-}" \
    run offscreen "$offscreen_test" "${engine_switches[@]}"
finished offscreen "the off-screen test"
# Again with the frames in GPU buffers (offscreen.c's note: says which it had);
# on macOS the frames are IOSurfaces either way, shm there is the shared-memory copy.
offscreen_buffers=gpu
[[ "$ZEBKIT_TARGET" == macos ]] && offscreen_buffers=shm
ZEBKIT_TEST_BASE="http://127.0.0.1:$port" ZEBKIT_TEST_BUFFERS="$offscreen_buffers" \
    run "offscreen-$offscreen_buffers-buffers" "$offscreen_test" "${engine_switches[@]}"
finished "offscreen-$offscreen_buffers-buffers" "the off-screen test with $offscreen_buffers buffers"
# Frames as a prototype viewer has them, input from the application; on Linux
# again in the engine's own window, input from the X server.
ZEBKIT_TEST_BASE="http://127.0.0.1:$port" ZEBKIT_TEST_OTHER="http://localhost:$port" \
    run frames-input "$bin/frames-input" "${engine_switches[@]}"
finished frames-input "the frames' input test"
ZEBKIT_TEST_SCALE=2 ZEBKIT_TEST_BASE="http://127.0.0.1:$port" ZEBKIT_TEST_OTHER="http://localhost:$port" \
    run frames-input-scaled "$bin/frames-input" "${engine_switches[@]}"
finished frames-input-scaled "the frames' input test at twice the pixels"
if [[ "$ZEBKIT_HOST" == linux ]] && selected frames-input-window && command -v Xvfb > /dev/null; then
    exec 8< <(Xvfb -displayfd 1 -screen 0 1600x1200x24 -nolisten tcp 2> /dev/null)
    frames_xvfb=$!
    pids+=("$frames_xvfb")
    read -r -t 15 -u 8 frames_display || frames_display=
    if [[ -n "$frames_display" ]]; then
        ZEBKIT_TEST_WINDOW=1 ZEBKIT_TEST_BASE="http://127.0.0.1:$port" ZEBKIT_TEST_OTHER="http://localhost:$port" \
            run frames-input-window env -u WAYLAND_DISPLAY DISPLAY=":$frames_display" "$bin/frames-input" \
            --no-sandbox --user-data-dir="$work/profile-frames-window"
        finished frames-input-window "the frames' input test in a window"
    else
        echo "FAIL Xvfb did not start"; failed=1
    fi
    kill "$frames_xvfb" 2>/dev/null || true
    exec 8<&-
elif [[ "$ZEBKIT_HOST" == linux ]]; then
    command -v Xvfb > /dev/null || skip "no Xvfb" frames-input-window
fi
if [[ "$ZEBKIT_HOST" != windows ]]; then
run offscreen-host "$bin/offscreen-show" --renderer="$bin/offscreen-render" \
    --url="http://127.0.0.1:$port/offscreen.html" --size=400x300@1 ${ENGINE_TEST_SHOTS:+--shots="$ENGINE_TEST_SHOTS"} \
    --test -- --no-sandbox --user-data-dir="$work/profile-host"
finished offscreen-host "the off-screen host"
fi

mkdir -p "$work/page-files"
ZEBKIT_TEST_BASE="http://127.0.0.1:$port" ZEBKIT_TEST_FILES="$work/page-files" \
    run page "$bin/page" "${engine_switches[@]}"
finished page "the page test"
ZEBKIT_TEST_BASE="http://127.0.0.1:$port" ZEBKIT_TEST_FILES="$work/page-files" run automation "$bin/automation" "${engine_switches[@]}"
finished automation "the automation test"
run automation-reentry "$bin/automation-reentry" --no-sandbox --headless --user-data-dir="$work/profile-reentry"
finished automation-reentry "the reentry test"
run automation-input env ZEBKIT_DIR="$package" PYTHONPATH="$ZEBKIT_ROOT/bindings/python" \
    python3 "$tests/automation_input.py" --no-sandbox --headless --user-data-dir="$work/profile-input"
finished automation-input "the input test"
run automation-input-held env ZEBKIT_TEST_NO_INPUT_WAIT=1 ZEBKIT_DIR="$package" PYTHONPATH="$ZEBKIT_ROOT/bindings/python" \
    python3 "$tests/automation_input.py" --zebkit-test-no-input-wait --no-sandbox --headless --user-data-dir="$work/profile-input-held"
run offscreen-scale "$bin/offscreen-scale" "${engine_switches[@]}"
run automation-python env ZEBKIT_DIR="$package" "${python_run[@]}" "$tests/automation_python.py" "http://127.0.0.1:$port" "${engine_switches[@]}"
run html-in-canvas env ZEBKIT_DIR="$package" "${python_run[@]}" "$tests/html_in_canvas.py" "http://127.0.0.1:$port" "${engine_switches[@]}"
# What reached the system without the application's decision: File System Access,
# protocol handlers, pointer and keyboard lock (docs/SYSTEM_INTEGRATION.md, phase 2).
run system-holes env ZEBKIT_DIR="$package" "${python_run[@]}" "$tests/system_holes.py" "${engine_switches[@]}"
# HTTP authentication without a function: the engine's sign-in dialog with a window (9).
run sign-in env ZEBKIT_DIR="$package" "${python_run[@]}" "$tests/sign_in.py" "${engine_switches[@]}"
# A page's prompt() and confirm(): Chromium's views dialog with a window (9).
run js-dialogs env ZEBKIT_DIR="$package" "${python_run[@]}" "$tests/js_dialogs.py" "${engine_switches[@]}"
# Find in page: frames, shadow trees, next and previous, highlights, stopping; over CDP too.
run find-in-page env ZEBKIT_DIR="$package" "${python_run[@]}" "$tests/find_in_page.py" "${engine_switches[@]}"
# How downloads end: complete with the saved path, cancelled, failed on the network and the file system.
run download-outcomes env ZEBKIT_DIR="$package" "${python_run[@]}" "$tests/download_outcomes.py" "${engine_switches[@]}"
# The file name the automation's Page.download gives, from Content-Disposition, as Chrome's.
run automation-download-name env ZEBKIT_DIR="$package" "${python_run[@]}" "$tests/automation_download_name.py" "${engine_switches[@]}"
# Dialogs a native Playwright click opens, held for a CDP client or the application.
run automation-dialogs env ZEBKIT_DIR="$package" "${python_run[@]}" "$tests/automation_dialogs.py" "${engine_switches[@]}"
# The clipboard's permissions: a write after a click granted, a read the application's to decide.
run clipboard-permission env ZEBKIT_DIR="$package" "${python_run[@]}" "$tests/clipboard_permission.py" "${engine_switches[@]}"
# A page's context menu: default items, commands on the captured context (8).
run context-menu env ZEBKIT_DIR="$package" "${python_run[@]}" "$tests/context_menu.py" "${engine_switches[@]}"
# The menu's items' state by id (docs/SYSTEM_INTEGRATION.md, 8).
run menu-state env ZEBKIT_DIR="$package" "${python_run[@]}" "$tests/menu_state.py" "${engine_switches[@]}"
# Screens, keeping awake, opening URLs with the system's handler (13, 14).
run system-services env ZEBKIT_DIR="$package" "${python_run[@]}" "$tests/system_services.py" "${engine_switches[@]}"
# Accessibility trees when an assistive technology asks (macOS: AXManualAccessibility).
run accessibility env ZEBKIT_DIR="$package" "${python_run[@]}" "$tests/accessibility.py" "${engine_switches[@]}"
# Pages following the system's appearance: a fake XDG portal on a private bus (Linux).
if [[ "$ZEBKIT_HOST" == linux ]] && command -v dbus-run-session > /dev/null && python3 -c "import dbus.mainloop.glib" 2> /dev/null; then
    run appearance-follow env ZEBKIT_DIR="$package" dbus-run-session -- "${python_run[@]}" "$tests/appearance.py" follow \
        "${engine_switches[@]}"
    run appearance-fixed env ZEBKIT_DIR="$package" dbus-run-session -- "${python_run[@]}" "$tests/appearance.py" fixed \
        "${engine_switches[@]}"
    # Web notifications to the desktop's service, and the tray's StatusNotifierItem (12).
    run notifications env ZEBKIT_DIR="$package" dbus-run-session -- "${python_run[@]}" "$tests/notifications.py" \
        "${engine_switches[@]}"
    run tray env ZEBKIT_DIR="$package" dbus-run-session -- "${python_run[@]}" "$tests/tray.py" \
        "${engine_switches[@]}"
    # The session's lock, from the desktop's screen saver (14).
    run session-lock env ZEBKIT_DIR="$package" dbus-run-session -- "${python_run[@]}" "$tests/session_lock.py" \
        "${engine_switches[@]}"
elif [[ "$ZEBKIT_HOST" == linux ]]; then
    skip "no dbus-run-session or dbus-python" appearance-follow appearance-fixed notifications tray session-lock
fi
# getDisplayMedia() through the media access hook: the view itself as the source (10).
run display-capture env ZEBKIT_DIR="$package" "${python_run[@]}" "$tests/display_capture.py" "${engine_switches[@]}"
# The Python binding's later families: contexts, scripts, off-screen views,
# page dialogs, print and drag, media capture, the rest of PDF.
# PDFium's test files: the Chromium tree's here, the bundle's copy (--build-only) elsewhere.
pdf_resources="$SRC_DIR/third_party/pdfium/testing/resources"
[[ -n "$prebuilt" ]] && pdf_resources="$bin/pdf-resources"
run view-lifetime env ZEBKIT_DIR="$package" "${python_run[@]}" "$tests/view_lifetime.py" "${engine_switches[@]}"
# A page that stops answering input, and one that answers again: off-screen views.
run responsiveness env ZEBKIT_DIR="$package" "${python_run[@]}" "$tests/responsiveness.py" "${engine_switches[@]}"
# What a host script's uncaught error does to zebkit-js: printed, and exit 1 at the end.
run script-uncaught "${python_run[@]}" "$tests/script_uncaught.py" "$bin/tools/zebkit-js" --no-sandbox --headless \
    --user-data-dir="$work/profile-script-uncaught"
# A service worker's clients.openWindow() from a notification click, and the popup policy.
run sw-open-window env ZEBKIT_DIR="$package" "${python_run[@]}" "$tests/sw_open_window.py" "${engine_switches[@]}"
run python-declarations "${python_run[@]}" "$tests/python_declarations.py"
run python-parity env ZEBKIT_DIR="$package" ZEBKIT_PDF_RESOURCES="$pdf_resources" "${python_run[@]}" "$tests/python_parity.py" \
    "${engine_switches[@]}"
# WebUSB, WebHID, Web Serial choosers through zebkit_view_on_device_chooser (10).
run device-chooser env ZEBKIT_DIR="$package" "${python_run[@]}" "$tests/device_chooser.py" "${engine_switches[@]}"
# The application's icon in the Dock or on the taskbar (12); the launcher's signal on Linux's private bus.
dock_bus=()
[[ "$ZEBKIT_HOST" == linux ]] && command -v dbus-run-session > /dev/null && dock_bus=(dbus-run-session --)
run dock env ZEBKIT_DIR="$package" "${dock_bus[@]}" "${python_run[@]}" "$tests/dock.py" "${engine_switches[@]}"
# A view's window made with options (<zebkit/window.h>): without a window here,
# with one under Xvfb and labwc below.
run window-frame env ZEBKIT_DIR="$package" "${python_run[@]}" "$tests/window_frame.py" "${engine_switches[@]}"
if [[ "$ZEBKIT_HOST" == linux ]]; then
    # LANG is the system's locale on Linux only; macOS and Windows have the user's settings.
    run system-locale env ZEBKIT_DIR="$package" LANG=ja_JP.UTF-8 LC_ALL=ja_JP.UTF-8 \
        "${python_run[@]}" "$tests/system_locale.py" ja-JP "${engine_switches[@]}"
    run system-locale-none env ZEBKIT_DIR="$package" LANG=C LC_ALL=C \
        "${python_run[@]}" "$tests/system_locale.py" en-US "${engine_switches[@]}"
fi
pw_node=("${node_cmd[@]}")
engine_test_node_ready=false
if "${pw_node[@]}" --version > /dev/null 2>&1; then
    engine_test_node_ready=true
    run certificate-h2 env ZEBKIT_DIR="$package" PYTHONPATH="$ZEBKIT_ROOT/bindings/python" \
        python3 "$tests/certificate_h2.py" "${pw_node[@]}"
else
    skip "no Node.js" certificate-h2
fi
# A feature's cases in every language, one driver with one list of modes each
# (tests/engine/language_modes.py): HTTP authentication, a document's TLS errors,
# the user agent, the console, new windows. Rust's are with the Rust examples below.
# A driver starts an engine for each mode, each with 90 s, inside a budget of
# its own (language_modes.py) that ends before this limit, so that a stall is
# reported by mode.
run_limit=600
for family in http_auth certificate user_agent; do
    run "${family//_/-}-c" "${python_run[@]}" "$tests/$family.py" c "$bin/$family"
done
for family in http_auth certificate user_agent console popup; do
    run "${family//_/-}-python" env ZEBKIT_DIR="$package" "${python_run[@]}" "$tests/$family.py" python \
        "${python_run[@]}" "$tests/${family}_python.py"
done
for family in http_auth certificate user_agent console; do
    run "${family//_/-}-javascript" "${python_run[@]}" "$tests/$family.py" javascript "$bin/tools/zebkit-js" \
        "$tests/script/$family.mjs"
done
run user-agent-cdp "${python_run[@]}" "$tests/user_agent.py" cdp "$bin/tools/zebkit-run"
run_limit=
if npm_has "$CACHE_DIR/playwright-tools" "playwright-core@$PLAYWRIGHT_CORE_VERSION" && "$engine_test_node_ready"; then
    run playwright-connect "${pw_node[@]}" "$tests/playwright_connect.mjs" "$CACHE_DIR/playwright-tools/node_modules/playwright-core" \
        "$bin/tools/zebkit-run" --no-sandbox --headless --user-data-dir="$work/profile-connect"
else
    skip "no Node.js or playwright-core $PLAYWRIGHT_CORE_VERSION: engine-test.sh --fetch" playwright-connect
fi
# Real CDP clients, Chrome's results as the reference (tests/engine/cdp_clients.mjs).
if selected cdp-clients && "$engine_test_node_ready" && [[ "$ZEBKIT_HOST" == linux ]]; then
    # Unless named, the Chromium Playwright installed for itself, as it finds it:
    # in the user's cache, which lib.sh's XDG_CACHE_HOME would hide (its ffmpeg too).
    cdp_chrome="${CHROME:-}"
    if npm_has "$cdp_clients" "${cdp_clients_packages[@]}" && [[ -z "$cdp_chrome" ]]; then
        cdp_chrome="$(cd "$cdp_clients" && env -u XDG_CACHE_HOME "${pw_node[@]}" -e "console.log(require('playwright-core').chromium.executablePath())" 2> /dev/null | tr -d '\r' || true)"
    fi
    if ! npm_has "$cdp_clients" "${cdp_clients_packages[@]}"; then
        skip "no ${cdp_clients_packages[*]}: engine-test.sh --fetch" cdp-clients
    elif [[ ! -x "$cdp_chrome" ]]; then
        skip "no Chrome" cdp-clients
    else
        # Not run(): two browsers, one after the other, take longer than its limit.
        log cdp-clients
        mark_ran cdp-clients
        need "$bin/tools/zebkit-run"
        if ! nice -n 19 ionice -c3 timeout -k 10 2400 env -u XDG_CACHE_HOME "${pw_node[@]}" "$tests/cdp_clients.mjs" "$cdp_clients" "$tests/cdp_clients_expected.txt" \
            --zebkit "$bin/tools/zebkit-run" --chrome "$cdp_chrome" --work "$work/cdp-clients" \
            --cache "$CACHE_DIR/cdp-clients-results" > "$work/cdp-clients.log" 2> "$work/cdp-clients.err"; then
            failed=1
            cat "$work/cdp-clients.log"
            echo "FAIL cdp-clients: it did not exit 0"
            echo "it printed on stderr:"; tail -20 "$work/cdp-clients.err"
        else
            cat "$work/cdp-clients.log"
        fi
        collect_skips cdp-clients
        finished cdp-clients "the CDP clients' comparison"
    fi
elif [[ "$ZEBKIT_HOST" == linux ]]; then
    skip "no Node.js" cdp-clients
fi
# On macOS the engine starts on the main thread only (<zebkit/zebkit.h>).
late_load_owners=(main worker)
[[ "$ZEBKIT_HOST" == macos ]] && late_load_owners=(main)
for owner in "${late_load_owners[@]}"; do
    run "late-load-c-$owner" "$bin/late-load" "$library" "$owner" "${engine_switches[@]}"
    run "late-load-$owner" env ZEBKIT_DIR="$package" PYTHONPATH="$ZEBKIT_ROOT/bindings/python" \
        python3 "$tests/late_load.py" "$owner" "${engine_switches[@]}"
done
# tools/zebkit-mcp, built against this package, with the official client and the
# conformance suite as development dependencies (upstream.lock); not shipped.
mcp_tests=(mcp-server mcp-conformance mcp-agent-run)
mcp_selected=
for mcp_test in "${mcp_tests[@]}"; do selected "$mcp_test" && mcp_selected=1; done
if [[ -z "$mcp_selected" ]]; then
    :
elif [[ -n "$prebuilt" ]]; then
    # Built with cargo from tools/, which the bundle does not have; on Linux the
    # checkout's run has them.
    [[ "$ZEBKIT_HOST" == linux ]] && skip "not in the bundle (--prebuilt): run from a checkout" "${mcp_tests[@]}"
elif ! command -v cargo > /dev/null; then
    skip "no cargo" "${mcp_tests[@]}"
elif ! "$engine_test_node_ready"; then
    skip "no Node.js" "${mcp_tests[@]}"
elif ! npm_has "$mcp_tools" "${mcp_tools_packages[@]}"; then
    skip "no ${mcp_tools_packages[*]}: engine-test.sh --fetch" "${mcp_tests[@]}"
else
    log "building tools/zebkit-mcp"
    if (cd "$ZEBKIT_ROOT/tools/zebkit-mcp" && ZEBKIT_DIR="$package" cargo build -q --locked --target-dir "$cargo_dir/mcp"); then
        zebkit_mcp="$cargo_dir/mcp/debug/zebkit-mcp"
        if selected mcp-server; then
            # Over standard input and HTTP, every tool twice: longer than the rest.
            log mcp-server
            mark_ran mcp-server
            if ! limited 900 "${pw_node[@]}" "$tests/mcp_server.mjs" "$mcp_tools" "$zebkit_mcp" --no-sandbox > "$work/mcp-server.log" 2>&1; then
                failed=1
                grep -v '^ok' "$work/mcp-server.log" | tail -40
                echo "FAIL mcp-server: it did not exit 0"
            fi
            tail -1 "$work/mcp-server.log"
            collect_skips mcp-server
            finished mcp-server "the MCP server's test"
        fi
        run mcp-conformance "${pw_node[@]}" "$tests/mcp_conformance.mjs" "$mcp_tools" "$zebkit_mcp" --no-sandbox
        run mcp-agent-run "${pw_node[@]}" "$tests/mcp_agent_run.mjs" "$mcp_tools" "$zebkit_mcp" --no-sandbox
        finished mcp-agent-run "the scripted MCP run"
    else
        echo "FAIL tools/zebkit-mcp did not build"; failed=1
    fi
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
run permission-hooks env ZEBKIT_DIR="$package" PYTHONPATH="$ZEBKIT_ROOT/bindings/python" \
    python3 "$tests/permission_hooks.py" "http://127.0.0.1:$port" --use-fake-device-for-media-stream "${engine_switches[@]}"
run pdf-outline env ZEBKIT_DIR="$package" PYTHONPATH="$ZEBKIT_ROOT/bindings/python" \
    python3 "$tests/pdf_outline.py"
for quality in 3G Slow-2G; do
    run "network-quality-$quality" env ZEBKIT_DIR="$package" PYTHONPATH="$ZEBKIT_ROOT/bindings/python" \
        python3 "$tests/network_quality.py" "$quality" "${engine_switches[@]}"
done
run media-tools env ZEBKIT_DIR="$package" PYTHONPATH="$ZEBKIT_ROOT/bindings/python" \
    ZEBKIT_MEDIA_PROBE="$bin/media-probe" python3 "$tests/media_tools.py"
# WebRTC's mDNS binds UDP 5353, and on Windows that asks a person about the firewall
# (docs/WINDOWS.md). This test only needs the two ends to find each other.
media_tap_switches=()
[[ "$ZEBKIT_HOST" == windows ]] && media_tap_switches=(--disable-features=WebRtcHideLocalIpsWithMdns)
run media-tap env ZEBKIT_DIR="$package" PYTHONPATH="$ZEBKIT_ROOT/bindings/python" \
    ZEBKIT_MEDIA_PROBE="$bin/media-probe" \
    python3 "$tests/media_tap.py" "http://127.0.0.1:$port" "${engine_switches[@]}" \
    --autoplay-policy=no-user-gesture-required "${media_tap_switches[@]}"

for adaptive_mode in down up audio mkv clock; do
    run "media-resolution-$adaptive_mode" env ZEBKIT_DIR="$package" PYTHONPATH="$ZEBKIT_ROOT/bindings/python" \
        ZEBKIT_MEDIA_PROBE="$bin/media-probe" ZEBKIT_ADAPTIVE_MODE="$adaptive_mode" \
        python3 "$tests/media_resolution.py" "http://127.0.0.1:$port" "${engine_switches[@]}"
done
for clock_mode in hls hls-discontinuity webm off unknown; do
    run "media-clock-$clock_mode" env ZEBKIT_DIR="$package" PYTHONPATH="$ZEBKIT_ROOT/bindings/python" \
        ZEBKIT_MEDIA_PROBE="$bin/media-probe" \
        python3 "$tests/media_clock.py" "http://127.0.0.1:$port" "$clock_mode" "${engine_switches[@]}"
done
ZEBKIT_MEDIA_PROBE="$bin/media-probe" run cdp-page "${python_run[@]}" "$tests/cdp_page.py" "$bin/tools/zebkit-run" "http://127.0.0.1:$port" \
    "$work/page-files" --no-sandbox --headless --user-data-dir="$work/profile-cdp"
ZEBKIT_TEST_BASE="http://127.0.0.1:$port" run popup "$bin/popup" "${engine_switches[@]}"
finished popup "the popup test"
mkdir -p "$work/media-files"
ZEBKIT_TEST_BASE="http://127.0.0.1:$port" ZEBKIT_TEST_FILES="$work/media-files" \
    run media "$bin/media" "${engine_switches[@]}" --autoplay-policy=no-user-gesture-required
finished media "the media test"
# zebkit_media_keep_history(), in an engine of its own.
mkdir -p "$work/media-history-files"
ZEBKIT_TEST_HISTORY=1 ZEBKIT_TEST_BASE="http://127.0.0.1:$port" ZEBKIT_TEST_FILES="$work/media-history-files" \
    run media-history "$bin/media" "${engine_switches[@]}" \
    --autoplay-policy=no-user-gesture-required
finished media-history "the media history test"
run cdp-browser "${python_run[@]}" "$tests/cdp_browser.py" "$bin/tools/zebkit-run" "http://127.0.0.1:$port" \
    --no-sandbox --headless --user-data-dir="$work/profile-cdp-browser"
# What CDP changes at runtime, by what the page and a server saw (docs/ROADMAP.md).
run cdp-overrides "${python_run[@]}" "$tests/cdp_overrides.py" "$bin/tools/zebkit-run" \
    --no-sandbox --headless --user-data-dir="$work/profile-cdp-overrides"
finished cdp-overrides "the CDP overrides test"
run pdf-in-page env ZEBKIT_TEST_REAL_PDF="$real_pdf" "${python_run[@]}" "$tests/pdf_in_page.py" "$bin/tools/zebkit-run" \
    --no-sandbox --headless --user-data-dir="$work/profile-pdf-in-page"
finished pdf-in-page "the PDF in pages test"
[[ -f "$real_pdf" ]] || skip "its long real document: engine-test.sh --fetch" pdf-in-page
# The PDF pane of the native hosts: built apart (examples/native-host/README.md) on Linux.
if [[ "$ZEBKIT_HOST" == linux ]] && command -v Xvfb > /dev/null; then
    for pdf_host in "$OUT_DIR/examples/gtk-host" "$OUT_DIR/examples/qt-host" \
        "$ZEBKIT_ROOT/examples/native-host/winit/target/debug/zebkit-winit-host"; do
        [[ -x "$pdf_host" ]] || { skip "no ${pdf_host#"$ZEBKIT_ROOT"/}: examples/native-host/README.md builds it" \
            "native-host-pdf-$(basename "$pdf_host")" "native-host-input-$(basename "$pdf_host")"; continue; }
        # winit drops XTest's wheel: that host wheels itself.
        self_driven=()
        pdf_document="$([[ -f "$real_pdf" ]] && echo "$real_pdf" || echo generated)"
        # It is a debug build, and wheels by itself: the generated document is long enough.
        run_limit=180
        [[ "$pdf_host" == *winit* ]] && { self_driven=(--self); pdf_document=generated; run_limit=450; }
        run "native-host-pdf-$(basename "$pdf_host")" env LD_LIBRARY_PATH="$CACHE_DIR/native-toolkits/usr/lib/x86_64-linux-gnu" "${python_run[@]}" "$tests/native_host_pdf_scroll.py" "${self_driven[@]}" "$pdf_host" \
            "$pdf_document" --no-sandbox --disable-gpu
        # The toolkit's window's input through XTest: focus, keys, a button, a <select>.
        run "native-host-input-$(basename "$pdf_host")" env LD_LIBRARY_PATH="$CACHE_DIR/native-toolkits/usr/lib/x86_64-linux-gnu" \
            "${python_run[@]}" "$tests/native_host_input.py" "$pdf_host" --no-sandbox --disable-gpu
    done
    run_limit=180
elif [[ "$ZEBKIT_HOST" == macos && -n "${ENGINE_TEST_WINDOWS:-}" ]]; then
    run_limit=450
    run native-host-appkit-pdf "${python_run[@]}" "$tests/native_host_pdf_scroll.py" --self "$bin/native-host-appkit" \
        generated:40 --no-sandbox
    run_limit=180
fi

if [[ "$ZEBKIT_HOST" == linux ]]; then
    run sandbox-startup "${python_run[@]}" "$tests/sandbox_startup.py" "$bin/tools/zebkit-run"
    # The set-user-ID helper as docs/SANDBOX.md installs it: the package's side
    # here; the install and the engine using it need root, which the suite never
    # asks for, so that part is a skip ("setuid helper: needs root"), run as root
    # on a test machine at each release (scripts/sandbox-helper-test.sh's header).
    if [[ -x "$ZEBKIT_ROOT/scripts/sandbox-helper-test.sh" ]]; then
        run sandbox-helper "$ZEBKIT_ROOT/scripts/sandbox-helper-test.sh" "$package"
    else
        skip "no scripts/sandbox-helper-test.sh in the bundle" sandbox-helper
    fi
fi
run cookie-host "${python_run[@]}" "$tests/cookie_host.py" "$bin/cookie-host" --no-sandbox --headless
run cookie-file "${python_run[@]}" "$tests/cookie_file.py" "$bin/cookie-host" --no-sandbox --headless
run cookie-file-options "${python_run[@]}" "$tests/cookie_file_options.py" "$bin/cookie-file-runner" --no-sandbox --headless

run webauthn-plain-headless "${python_run[@]}" "$tests/webauthn.py" "$webauthn_host" \
    --no-sandbox --headless --test-normal-view
run webauthn-host-ui "${python_run[@]}" "$tests/webauthn_ui.py" "$webauthn_host" --no-sandbox
run webauthn "${python_run[@]}" "$tests/webauthn.py" "$webauthn_host" --no-sandbox --headless
run webauthn-unfocused "${python_run[@]}" "$tests/webauthn.py" "$webauthn_host" \
    --no-sandbox --headless --test-authentication-ui
run webauthn-no-activation "${python_run[@]}" "$tests/webauthn.py" "$webauthn_host" \
    --no-sandbox --headless --test-authentication-ui --test-focused

# Cookie encryption uses the real OS key store. Headless Linux builders
# without a desktop keyring can opt into this integration test explicitly.
if [[ "$ZEBKIT_HOST" == macos || "$ZEBKIT_HOST" == windows || "${ENGINE_TEST_PERSISTENT_COOKIES:-0}" == 1 ]]; then
    run cookies "${python_run[@]}" "$tests/cookies.py" "$bin/tools/zebkit-run" --no-sandbox --headless
fi

mkdir -p "$work/scheme-files"
ZEBKIT_TEST_FILES="$work/scheme-files" run scheme "$bin/scheme" "${engine_switches[@]}"

mkdir -p "$work/script-files"
# The headers the network adds, against the server, CDP and real Chrome (tests/engine/network_stages.py).
run network-stages env ZEBKIT_DIR="$package" PYTHONPATH="$ZEBKIT_ROOT/bindings/python:$tests" \
    python3 "$tests/network_stages.py" --no-sandbox --headless
finished network-stages "the network stages test"
# A shared or service worker's WebSocket, decided by its own context's policy.
run worker-websocket env ZEBKIT_DIR="$package" "${python_run[@]}" "$tests/worker_websocket.py" "${engine_switches[@]}"
# <zebkit/hooks.h> in every language, one engine per group (Rust: per mode)
# (tests/engine/hook_policies.py): links to other programs, downloads, client
# certificates and the system's stores of them, isolated worlds, file choosers,
# media, keys, context menus, fullscreen, the network and popups.
hook_groups=(external download certificate world page network popup certstore)
rust_hook_groups=(external download certificate world page network popup certstore)
# Each group runs its modes one after another, a mode stalled for 60 s ended
# by the driver and reported (hook_policies.py): more than run's 180 s.
run_limit=600
# certstore_without_cc <group> <test...>: on Linux the certstore group builds
# tests/engine/nss_import.c where it runs (hook_policies.py); with no C
# compiler there (a bundle's machine) its tests are a skip, said, not a failure.
certstore_without_cc() {
    [[ "$1" == certstore && "$ZEBKIT_HOST" == linux ]] && ! command -v cc > /dev/null || return 1
    shift
    skip "no C compiler (cc) to build tests/engine/nss_import.c with" "$@"
}
for hook_group in "${hook_groups[@]}"; do
    certstore_without_cc "$hook_group" hook-{c,javascript,python}-certstore && continue
    run "hook-c-$hook_group" env PYTHONPATH="$tests" "${python_run[@]}" "$tests/hook_policies.py" "c:$hook_group" "$bin/hook-policies"
    run "hook-javascript-$hook_group" env PYTHONPATH="$tests" "${python_run[@]}" "$tests/hook_policies.py" "javascript:$hook_group" \
        "$bin/tools/zebkit-js" "$tests/script/hook_policies.mjs"
    run "hook-python-$hook_group" env ZEBKIT_DIR="$package" PYTHONPATH="$ZEBKIT_ROOT/bindings/python:$tests" \
        "${python_run[@]}" "$tests/hook_policies.py" "python:$hook_group" python3 "$tests/hook_policies_python.py"
done
run_limit=
# What zebkit decides, changed while it runs, in every language (tests/engine/runtime_settings.py).
run runtime-settings-c "${python_run[@]}" "$tests/runtime_settings.py" c "$bin/runtime-settings" @switches@
finished runtime-settings-c "the C runtime settings test"
run runtime-settings-python env ZEBKIT_DIR="$package" PYTHONPATH="$ZEBKIT_ROOT/bindings/python" \
    "${python_run[@]}" "$tests/runtime_settings.py" python python3 "$tests/runtime_settings_python.py" @switches@
finished runtime-settings-python "the Python runtime settings test"
run runtime-settings-javascript "${python_run[@]}" "$tests/runtime_settings.py" javascript \
    "$bin/tools/zebkit-js" @switches@ "$tests/script/runtime_settings.mjs"
finished runtime-settings-javascript "the JavaScript runtime settings test"
# The Rust examples (rust_examples): built here into the suite's cargo
# directory, or from the bundle (--build-only).
rust_tests=(runtime-settings-rust http-auth-rust certificate-rust user-agent-rust console-rust popup-rust rust-engine)
for hook_group in "${rust_hook_groups[@]}"; do rust_tests+=("hook-rust-$hook_group"); done
rust_selected=
for rust_test in "${rust_tests[@]}"; do selected "$rust_test" && rust_selected=1; done
rust_dir=
if [[ -n "$prebuilt" ]]; then
    [[ -x "$bin/rust/hook_policies" || -x "$bin/rust/hook_policies.exe" ]] && rust_dir="$bin/rust"
elif [[ -n "$rust_selected" ]] && command -v cargo > /dev/null; then
    log "building the Rust examples"
    if (cd "$ZEBKIT_ROOT/bindings/rust" && ZEBKIT_DIR="$package" CARGO_TARGET_DIR="$cargo_dir/host" \
        cargo build -q -p zebkit $(printf -- '--example %s ' "${rust_examples[@]}")); then
        rust_dir="$cargo_dir/host/debug/examples"
    else
        echo "FAIL the Rust examples did not build"; failed=1
    fi
fi
# rust_example <name>: its path, if it is there.
rust_example() {
    [[ -n "$rust_dir" ]] && [[ -x "$rust_dir/$1" || -x "$rust_dir/$1.exe" ]] && echo "$rust_dir/$1"
    return 0
}
if [[ -n "$(rust_example hook_policies)" ]]; then
    run_limit=600
    for hook_group in "${rust_hook_groups[@]}"; do
        certstore_without_cc "$hook_group" hook-rust-certstore && continue
        run "hook-rust-$hook_group" env LD_LIBRARY_PATH="$package" PYTHONPATH="$tests" "${python_run[@]}" "$tests/hook_policies.py" \
            "rust:$hook_group" "$(rust_example hook_policies)"
    done
    run_limit=
else
    for hook_group in "${rust_hook_groups[@]}"; do skip "no Rust hook program (cargo)" "hook-rust-$hook_group"; done
fi
if [[ -n "$(rust_example runtime_settings)" ]]; then
    run runtime-settings-rust env LD_LIBRARY_PATH="$package" "${python_run[@]}" "$tests/runtime_settings.py" rust "$(rust_example runtime_settings)" @switches@
    finished runtime-settings-rust "the Rust runtime settings test"
else
    skip "no Rust settings program (cargo)" runtime-settings-rust
fi
# The Rust halves of the feature drivers above (example: http_auth.rs,
# certificate_policy.rs, user_agent.rs, console.rs, popup.rs).
for pair in http_auth:http_auth certificate:certificate_policy user_agent:user_agent console:console popup:popup; do
    family="${pair%%:*}" example="$(rust_example "${pair#*:}")"
    if [[ -n "$example" ]]; then
        run_limit=600 run "${family//_/-}-rust" env LD_LIBRARY_PATH="$package" "${python_run[@]}" "$tests/$family.py" rust "$example"
    else
        skip "no Rust example (cargo)" "${family//_/-}-rust"
    fi
done
# The Rust binding's own test of a real engine (bindings/rust/zebkit/tests/engine.rs),
# its profile the suite's. It reads the checkout's pages: not in the bundle.
if ! selected rust-engine; then
    :
elif [[ -n "$prebuilt" ]]; then
    skip "not in the bundle (--prebuilt): run from a checkout" rust-engine
elif ! command -v cargo > /dev/null; then
    skip "no cargo" rust-engine
else
    log "building the Rust binding's engine test"
    rust_engine="$(cd "$ZEBKIT_ROOT/bindings/rust" && ZEBKIT_DIR="$package" CARGO_TARGET_DIR="$cargo_dir/host" \
        cargo test -q -p zebkit --test engine --no-run --message-format=json 2> "$work/rust-engine-build.err" \
        | python3 -I -c 'import json, sys
for line in sys.stdin:
    message = json.loads(line)
    if message.get("reason") == "compiler-artifact" and message.get("executable") and message["target"]["name"] == "engine":
        print(message["executable"])')" || rust_engine=
    if [[ -x "$rust_engine" ]]; then
        run rust-engine env LD_LIBRARY_PATH="$package" "$rust_engine" "--user-data-dir=$work/profile"
        # Its summary is its own: "N passed, 0 failed".
        finished_line rust-engine '^[1-9][0-9]* passed, 0 failed$' "the Rust binding's engine test"
    else
        tail -20 "$work/rust-engine-build.err"
        echo "FAIL the Rust binding's engine test did not build"; failed=1
    fi
fi
# The Rust hosts of examples/, checked (not built) against this package where
# their crates are in cargo's cache already (--fetch gets them): the run
# fetches nothing.
for rust_host in gpui-browser:examples/gpui-browser winit-host:examples/native-host/winit; do
    rust_host_name="example-${rust_host%%:*}" rust_host_dir="$ZEBKIT_ROOT/${rust_host#*:}"
    selected "$rust_host_name" || continue
    if [[ -n "$prebuilt" ]]; then
        skip "not in the bundle (--prebuilt): run from a checkout" "$rust_host_name"
    elif ! command -v cargo > /dev/null; then
        skip "no cargo" "$rust_host_name"
    elif ! (cd "$rust_host_dir" && cargo fetch -q --offline --locked) > "$work/$rust_host_name-fetch.log" 2>&1; then
        # Crates missing from the cache are a skip (engine-test.sh --fetch gets
        # them); any other failure of cargo (a lock file out of date, a broken
        # manifest) is the example's.
        if grep -qiE 'offline|not available|failed to download|no matching package|failed to get' "$work/$rust_host_name-fetch.log" \
            && ! grep -qiE 'lock file .* needs to be updated|cannot update the lock file|failed to parse manifest' "$work/$rust_host_name-fetch.log"; then
            skip "its crates are not in cargo's cache (engine-test.sh --fetch)" "$rust_host_name"
        else
            mark_ran "$rust_host_name"
            tail -20 "$work/$rust_host_name-fetch.log"
            echo "FAIL $rust_host_name: cargo fetch --offline --locked failed, not for crates missing from the cache"; failed=1
            failed_tests+="$rust_host_name "
        fi
    else
        log "$rust_host_name"
        mark_ran "$rust_host_name"
        if (cd "$rust_host_dir" && ZEBKIT_DIR="$package" CARGO_TARGET_DIR="$cargo_dir/$rust_host_name" \
            nice -n 19 cargo check -q --offline --locked) > "$work/$rust_host_name.log" 2>&1; then
            echo "ok   ${rust_host#*:} checks against the package"
        else
            tail -30 "$work/$rust_host_name.log"
            echo "FAIL ${rust_host#*:} does not build against the package"; failed=1
        fi
    fi
done

run hooks-javascript "$bin/tools/zebkit-js" --no-sandbox --headless --user-data-dir="$work/profile-hooks-javascript" "$tests/script/hooks.mjs" "http://127.0.0.1:$port"
finished hooks-javascript "the hooks script"
# The Go binding (bindings/golang): its four check programs against this
# package. Built here on Linux; for macOS and Windows built into the bundle
# (--build-only) and run from the package there, as an application runs beside
# the engine, on Windows sandboxed.
go_checks=(check check_hooks check_pdf check_automation)
go_dir=
go_selected=
for go_check in "${go_checks[@]}"; do selected "go-$go_check" && go_selected=1; done
if [[ -z "$go_selected" ]]; then
    :
elif [[ -n "$prebuilt" && ( -f "$bin/go-check" || -f "$bin/go-check.exe" ) ]]; then
    go_dir="$package"
    for go_check in "${go_checks[@]}"; do
        for go_file in "$bin/go-$go_check"*; do
            cp "$go_file" "$package/"
            package_copies+=("$package/$(basename "$go_file")")
        done
    done
    go_resources="$bin/pdf-resources"
elif [[ -z "$prebuilt" && ${#go_cmd[@]} -gt 0 && "$ZEBKIT_HOST" == linux ]]; then
    log "building the Go binding's checks"
    go_dir="$work/go"
    mkdir -p "$go_dir"
    (cd "$ZEBKIT_ROOT/bindings/golang" && export CGO_CFLAGS="-I$package/include" CGO_LDFLAGS="-L$package -Wl,-rpath,$package" \
        && "${go_cmd[@]}" vet ./... && for go_check in "${go_checks[@]}"; do
            "${go_cmd[@]}" build -o "$go_dir/go-$go_check" "./internal/$go_check" || exit 1; done) \
        || { echo "FAIL the Go binding did not build"; failed=1; go_dir=; }
    go_resources="$SRC_DIR/third_party/pdfium/testing/resources"
else
    for go_check in "${go_checks[@]}"; do skip "no Go, or no checks in the bundle" "go-$go_check"; done
fi
if [[ -n "$go_dir" ]]; then
    for go_check in "${go_checks[@]}"; do
        go_args=()
        [[ "$go_check" == check_pdf ]] && go_args=(--resources="$go_resources")
        if [[ "$ZEBKIT_HOST" == windows ]]; then
            run "go-$go_check" env ZEBKIT_MEDIA_DIR="$tests/pages/media" "$go_dir/go-$go_check" "${go_args[@]}" \
                --headless --user-data-dir="$work/profile-go-$go_check"
        else
            run "go-$go_check" env ZEBKIT_MEDIA_DIR="$tests/pages/media" "$go_dir/go-$go_check" "${go_args[@]}" \
                "${engine_switches[@]}"
        fi
    done
fi
# zebkit-js beyond the basics: contexts, view basics, schemes, CDP, AES-GCM,
# the canvas over pixels; page.h and off-screen views; automation and media.
js_font=
[[ -f "$SRC_DIR/third_party/pdfium/testing/resources/fonts/ahem/Ahem.ttf" ]] && js_font="$SRC_DIR/third_party/pdfium/testing/resources/fonts/ahem/Ahem.ttf"
run lifetimes-javascript "$bin/tools/zebkit-js" --no-sandbox --headless --zebkit-scheme=jsfail,standard,secure,fetch \
    --user-data-dir="$work/profile-lifetimes-javascript" "$tests/script/lifetimes.mjs" "http://127.0.0.1:$port"
finished lifetimes-javascript "the lifetimes script"
run views-more-javascript "$bin/tools/zebkit-js" --no-sandbox --headless --zebkit-scheme=jsapp,standard,secure,fetch \
    --user-data-dir="$work/profile-views-more-javascript" "$tests/script/views_more.mjs" "http://127.0.0.1:$port" $js_font
finished views-more-javascript "the views script"
run page-offscreen-javascript "$bin/tools/zebkit-js" --no-sandbox --headless \
    --user-data-dir="$work/profile-page-offscreen-javascript" "$tests/script/page_offscreen.mjs" "http://127.0.0.1:$port"
finished page-offscreen-javascript "the page and off-screen script"
run automation-media-javascript "$bin/tools/zebkit-js" --no-sandbox --headless --autoplay-policy=no-user-gesture-required \
    --user-data-dir="$work/profile-automation-media-javascript" "$tests/script/automation_media.mjs" "http://127.0.0.1:$port" \
    "$tests/pages/media/clip.mp4" "$work"
finished automation-media-javascript "the automation and media script"
run system-javascript "$bin/tools/zebkit-js" --no-sandbox --headless --user-data-dir="$work/profile-system-javascript" "$tests/script/system.mjs" "http://127.0.0.1:$port"
finished system-javascript "the system services script"
run window-open-javascript "$bin/tools/zebkit-js" --no-sandbox --headless --user-data-dir="$work/profile-window-open-javascript" "$tests/script/window_open.mjs" "http://127.0.0.1:$port"
finished window-open-javascript "the window-open script"
# The forms that do not wait on the engine's thread (cdp.listenAsync, openExternalAsync,
# showInFolderAsync), and why a hook's reply was refused (request.error).
run async-forms-javascript "$bin/tools/zebkit-js" --no-sandbox --headless --user-data-dir="$work/profile-async-forms-javascript" "$tests/script/async_forms.mjs" "http://127.0.0.1:$port" "$work"
finished async-forms-javascript "the asynchronous forms script"
# A page's process going, a page that stops answering input and answers again, an off-screen
# view not made, a bound call dropped unanswered (tests/engine/script/process_events.mjs).
run process-events-javascript "$bin/tools/zebkit-js" --no-sandbox --headless --enable-features=RendererHangWatcher:delay/2s --js-flags=--expose-gc \
    --user-data-dir="$work/profile-process-events-javascript" "$tests/script/process_events.mjs" "http://127.0.0.1:$port"
finished process-events-javascript "the process events script"
run page-call-gc-javascript "$bin/tools/zebkit-js" --no-sandbox --headless --js-flags=--expose-gc \
    --user-data-dir="$work/profile-page-call-gc-javascript" "$tests/script/page_call_gc.mjs" "http://127.0.0.1:$port"
finished page-call-gc-javascript "a bound call rejected with a collection before its reaction"
run native-window-javascript-headless "$bin/tools/zebkit-js" --no-sandbox --headless \
    --user-data-dir="$work/profile-native-window-js-headless" "$tests/script/native_window.mjs" none
finished native-window-javascript-headless "the native window script (headless)"
run script "$bin/tools/zebkit-js" "${engine_switches[@]}" "$tests/script/app.mjs" \
    "http://127.0.0.1:$port" "$work/script-files"
# It ends by itself once idle; ending before its last line is a failure.
finished script "the script"

run pdf-plugin "$bin/tools/zebkit-js" "${engine_switches[@]}" "$tests/script/pdf_plugin.mjs" "$ahem"
finished pdf-plugin "the PDF plugin's script"

run ui "$bin/tools/zebkit-js" "${engine_switches[@]}" "$tests/ui/app.mjs"
finished ui "zebkit.ui's script"
# examples/ui/todo.mjs's scripted session: it prints nothing, its picture is its result.
run example-todo "$bin/tools/zebkit-js" "${engine_switches[@]}" "$ZEBKIT_ROOT/examples/ui/todo.mjs" --capture "$work/todo.png"
if was_run example-todo; then
    result_checked+="example-todo "
    todo_png="$(head -c 24 "$work/todo.png" 2> /dev/null | od -An -tx1 | tr -d ' \n' || true)"
    if [[ ${#todo_png} == 48 && "${todo_png:0:16}" == 89504e470d0a1a0a ]] && (( 16#${todo_png:32:8} >= 400 && 16#${todo_png:40:8} >= 600 )); then
        echo "ok   examples/ui/todo.mjs --capture: a PNG of $(( 16#${todo_png:32:8} ))x$(( 16#${todo_png:40:8} ))"
    else
        echo "FAIL examples/ui/todo.mjs --capture wrote no picture of its window"; failed=1
    fi
fi

ZEBKIT_DIR="$package" PYTHONPATH="$ZEBKIT_ROOT/bindings/python" \
    run python-services python3 "$tests/test_python.py" --services-only
ZEBKIT_DIR="$package" PYTHONPATH="$ZEBKIT_ROOT/bindings/python" \
    run python python3 "$tests/test_python.py" "${engine_switches[@]}"

if selected cdp-command-line; then
log "CDP from the command line"
mark_ran cdp-command-line
need "$bin/tools/zebkit-run"
"${launch[@]}" "$bin/tools/zebkit-run" --no-sandbox --headless --user-data-dir="$work/profile-cdp-command-line" --remote-debugging-port=0 about:blank \
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
    if grep -q '"type": *"page"' <<< "$(curl -fsS "http://127.0.0.1:$port/json/list" || true)"; then
        echo "ok   /json/list has the page"
    else
        echo "FAIL /json/list has no page"; failed=1
    fi
    # What cdp-clients expects to differ from a newer Chrome hides this one.
    if curl -fsS --compressed "http://127.0.0.1:$port/json/protocol" | grep '"domains"' > /dev/null; then
        echo "ok   /json/protocol"
    else
        echo "FAIL /json/protocol"; failed=1
    fi
fi
fi

if [[ "$ZEBKIT_HOST" == windows ]]; then
    # Nothing of the suite's in the person's own application data.
    if [[ "$( (ls -A "$real_local_app_data/zebkit" 2> /dev/null || true) | wc -l)" != "$zebkit_before" || -e "$real_local_app_data/content_shell" ]]; then
        echo "FAIL the suite wrote in %LOCALAPPDATA%"; failed=1
    else
        echo "ok   nothing written in %LOCALAPPDATA%"
    fi
fi
# The result line of every test run() ran and that passed (no_result_line_yet).
without_result=()
for name in $run_tests; do
    [[ "$failed_tests" == *" $name "* || "$result_checked" == *" $name "* ]] && continue
    grep -q '^\([1-9][0-9]*\) of \1 checks passed$' "$work/$name.log" 2> /dev/null && continue
    if [[ "$name" =~ $no_result_line_yet ]]; then
        without_result+=("$name")
    else
        echo "FAIL $name: it exited 0 but printed no \"N of N checks passed\" line, and it has no finished_line"
        failed=1
    fi
done
if (( ${#without_result[@]} )); then
    log "passed by exiting 0, with no summary line yet (no_result_line_yet): ${without_result[*]}"
fi

# What was not run, and why; what passed while printing failures. Under
# --strict a skip fails the run unless ENGINE_TEST_EXPECTED_SKIPS names it.
unexpected=()
if (( ${#skipped[@]} )); then
    log "skipped, ${#skipped[@]}:"
    for entry in "${skipped[@]}"; do
        if expected_skip "$entry"; then
            echo "  $entry (expected here)"
        else
            echo "  $entry"
            unexpected+=("$entry")
        fi
    done
fi
if (( ${#unsure[@]} )); then
    log "exited 0 but printed FAIL lines: ${unsure[*]}"
fi
if (( ran == 0 )); then
    if [[ -n "$only" ]] && (( ${#skipped[@]} == 0 )); then
        die "--only '$only' matches no test"
    fi
    die "no test ran"
fi
(( failed )) && die "engine tests failed ($ran run)"
if [[ -n "$strict" ]] && (( ${#unexpected[@]} )); then
    die "${#unexpected[@]} skipped that --strict (ENGINE_TEST_STRICT=1) does not allow (not in ENGINE_TEST_EXPECTED_SKIPS); $ran passed"
fi
log "engine tests passed: $ran run, ${#skipped[@]} skipped${strict:+ (strict)}"
