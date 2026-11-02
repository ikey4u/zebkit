#!/usr/bin/env bash
# Test the engine library from programs built against the package, as an
# application is built against it.
#
#   scripts/engine-test.sh <profile> [--only REGEX]
#   scripts/engine-test.sh <profile> --build-only DIR
#   DIR/scripts/engine-test.sh <profile> --prebuilt DIR/programs [--only REGEX]
#
# --build-only makes DIR, which holds all the suite needs: the programs (for a
# macOS profile on Linux, cross-compiled and signed ad hoc), the package, the
# tests' scripts and pages. Copy it to a machine with no compiler or source and
# run the suite there with --prebuilt; docs/MACOS.md has the steps. --only runs the
# tests whose names match.
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
while (( $# )); do
    case "$1" in
        --build-only) build_only="${2:?a directory}"; shift 2 ;;
        --prebuilt) prebuilt="${2:?a directory}"; shift 2 ;;
        --only) only="${2:?a regular expression}"; shift 2 ;;
        *) die "unknown option $1" ;;
    esac
done
set_target_for_profile "$profile"
[[ "$ZEBKIT_TARGET" == "$ZEBKIT_HOST" || -n "$build_only" ]] \
    || die "$profile's programs do not run on $ZEBKIT_HOST: use --build-only and run them there"
package="${ZEBKIT_TEST_PACKAGE:-$OUT_DIR/$profile/package}"
library="$(engine_library "$package")"
[[ -f "$library" ]] || die "no engine in $package; run scripts/package.sh $profile first"

work="$(mktemp -d "$TMPDIR/engine-test.XXXXXX")"
pids=()
offscreen_key_identity=
webauthn_key_identity=
cleanup() {
    for pid in "${pids[@]}"; do kill "$pid" 2>/dev/null || true; done
    if [[ "$ZEBKIT_HOST" == macos && -n "${ENGINE_TEST_REAL_KEYCHAIN:-}" ]]; then
        # Only the throwaway keys of this invocation's test executables.
        for identity in "$offscreen_key_identity" "$webauthn_key_identity"; do
            /usr/bin/security delete-generic-password -s "Zebkit Cookies: $identity" \
                -a "$identity" >/dev/null 2>&1 || true
        done
    fi
    if [[ -n "${ENGINE_TEST_KEEP_WORK:-}" ]]; then echo "kept: $work"; else rm -rf "$work"; fi
}
trap cleanup EXIT

# The programs: built for this run, or built before. In a directory that moves,
# they find the engine from where they are.
bin="$work"
if [[ -n "$prebuilt" ]]; then
    bin="$(cd "$prebuilt" && pwd -P)"
elif [[ -n "$build_only" ]]; then
    mkdir -p "$build_only/programs"
    build_only="$(cd "$build_only" && pwd -P)"
    bin="$build_only/programs"
    origin='$ORIGIN'
    [[ "$ZEBKIT_TARGET" == macos ]] && origin='@executable_path'
    # programs/x and programs/tools/x.
    ZEBKIT_TARGET_RPATHS="$origin/../out/blink/$profile/package $origin/../../out/blink/$profile/package"
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

if [[ -z "$prebuilt" ]]; then
log "building the test programs against $package"
build_against_package "$package" "$bin/vendor" "$tests/vendor.c"
# shellcheck disable=SC2046
target_cxx -std=c++17 -Wall -Werror -I"$package/include" "$tests/vendor.cc" \
    $(engine_link_options "$package") -o "$bin/vendor-cxx"
build_against_package "$package" "$bin/services" "$tests/services.c"
build_against_package "$package" "$bin/pdf" "$tests/pdf.c"
build_against_package "$package" "$bin/canvas" "$tests/canvas.c"
build_against_package "$package" "$bin/pdf-annotations" "$tests/pdf_annotations.c"
build_against_package "$package" "$bin/pdf-hooks" "$tests/pdf_hooks.c" "$ZEBKIT_ROOT/examples/pdf-plugin/plugin.c" -- -D_GNU_SOURCE -I"$ZEBKIT_ROOT/examples/pdf-plugin"
build_against_package "$package" "$bin/api" "$tests/api.c" -- -pthread
target_cc -std=c11 -O1 -Wall -Werror -I"$package/include" "$tests/late_load.c" -ldl -pthread -o "$bin/late-load"
build_against_package "$package" "$bin/lifecycle" "$tests/lifecycle.c" -- -pthread
build_against_package "$package" "$bin/host-loop" "$tests/host_loop.c" -- -pthread
if [[ "$ZEBKIT_TARGET" == linux ]] && pkg-config --exists glib-2.0; then
    read -r -a glib_flags <<< "$(pkg-config --cflags --libs glib-2.0)"
    build_against_package "$package" "$bin/host-loop-glib" "$tests/host_loop_glib.c" -- "${glib_flags[@]}" -pthread
fi
build_against_package "$package" "$bin/startup-settings" "$tests/startup_settings.c"
build_against_package "$package" "$bin/edit-commands" "$tests/edit_commands.c"
build_against_package "$package" "$bin/hooks" "$tests/hooks.c"
build_against_package "$package" "$bin/certificate" "$tests/certificate.c"
build_against_package "$package" "$bin/http_auth" "$tests/http_auth.c"
build_against_package "$package" "$bin/hook-policies" "$tests/hook_policies.c"
build_against_package "$package" "$bin/runtime-settings" "$tests/runtime_settings.c"
build_against_package "$package" "$bin/offscreen-scale" "$tests/offscreen_scale.c"
build_against_package "$package" "$bin/bridge" "$tests/bridge.c"
if [[ "$ZEBKIT_TARGET" == windows ]]; then
    # An application's own window and loop, and the sandbox with its executable
    # as the child (docs/WINDOWS.md); frames through file mappings and textures.
    build_against_package "$package" "$bin/win-smoke" "$tests/win_smoke.c"
    build_against_package "$package" "$bin/win-host" "$tests/win_host.c"
    build_against_package "$package" "$bin/win-offscreen" "$tests/win_offscreen.c"
    build_against_package "$package" "$bin/win-smoke-sandbox" "$tests/win_smoke.c" -- -DZEBKIT_TEST_SANDBOX "$package/zebkit_sandbox.lib" -Wl,/STACK:0x800000
    build_against_package "$package" "$bin/win-host-sandbox" "$tests/win_host.c" -- -DZEBKIT_TEST_SANDBOX "$package/zebkit_sandbox.lib" -Wl,/STACK:0x800000
    build_against_package "$package" "$bin/win-native-parent" "$tests/win_native_parent.c"
    build_against_package "$package" "$bin/win-native-parent-sandbox" "$tests/win_native_parent.c" -- -DZEBKIT_TEST_SANDBOX "$package/zebkit_sandbox.lib" -Wl,/STACK:0x800000
    # shellcheck disable=SC2046
    target_cxx -std=c++20 -O1 -I"$package/include" "$tests/win_sandbox_stubs.cc" "$package/zebkit_sandbox.lib" \
        $(engine_link_options "$package") -Wl,/STACK:0x800000 -o "$bin/win-sandbox-stubs"
fi
if [[ "$ZEBKIT_TARGET" == linux ]] && [[ -f /usr/include/X11/Xlib.h ]]; then
    build_against_package "$package" "$bin/native-parent" "$tests/native_parent.c" -- -lX11 -ldl
    build_against_package "$package" "$bin/mixed-views" "$tests/mixed_views.c" -- -DMIXED_X11 -lX11 -ldl
    build_against_package "$package" "$bin/native-host-x11" "$ZEBKIT_ROOT/examples/native-host/x11.c" -- -lX11
fi
if [[ "$ZEBKIT_TARGET" == macos ]]; then
    build_against_package "$package" "$bin/native-parent-mac" "$tests/native_parent_mac.m" -- -fobjc-arc -framework Cocoa
    build_against_package "$package" "$bin/mixed-views" "$tests/mixed_views.c" -- -framework IOSurface
    target_cxx -std=c++17 -fobjc-arc -O1 -Wall -Wextra -Wno-deprecated-declarations -I"$package/include" "$ZEBKIT_ROOT/examples/native-host/appkit.mm" \
        -framework Cocoa -framework IOSurface $(engine_link_options "$package") -o "$bin/native-host-appkit"
fi
build_against_package "$package" "$bin/scheme" "$tests/scheme.c"
offscreen_options=()
[[ "$ZEBKIT_TARGET" == macos ]] && offscreen_options=(-framework IOSurface)
build_against_package "$package" "$offscreen_test" "$tests/offscreen.c" -- "${offscreen_options[@]}"
build_against_package "$package" "$bin/page" "$tests/page.c"
build_against_package "$package" "$bin/popup" "$tests/popup.c"
build_against_package "$package" "$bin/cookie-host" "$tests/cookie_host.c"
build_against_package "$package" "$bin/cookie-file-runner" "$ZEBKIT_ROOT/tools/zebkit-run/zebkit-run.c"
build_against_package "$package" "$bin/automation" "$tests/automation.c"
build_against_package "$package" "$bin/automation-reentry" "$tests/automation_reentry.c"
build_against_package "$package" "$bin/media" "$tests/media.c"
build_against_package "$package" "$bin/media-probe" "$tests/media_probe.c"
build_against_package "$package" "$bin/mode" "$tests/mode.c"
build_against_package "$package" "$webauthn_host" "$tests/webauthn_host.c"
render_options=(-pthread)
[[ "$ZEBKIT_TARGET" == macos ]] && render_options+=(-framework IOSurface)
# The example of another process's frames: Unix sockets and mach ports, not on Windows.
if [[ "$ZEBKIT_TARGET" != windows ]]; then
build_against_package "$package" "$bin/offscreen-render" "$ZEBKIT_ROOT/examples/offscreen-host/render.c" -- "${render_options[@]}"
host_options=()
[[ "$ZEBKIT_TARGET" == macos ]] && host_options=(-framework CoreVideo -framework IOSurface -framework CoreFoundation)
target_cc -std=c11 -O1 -Wall -Werror "$ZEBKIT_ROOT/examples/offscreen-host/show.c" "${host_options[@]}" -o "$bin/offscreen-show"
fi
build_against_package "$package" "$bin/tools/zebkit-run" "$ZEBKIT_ROOT/tools/zebkit-run/zebkit-run.c"
build_against_package "$package" "$bin/tools/zebkit-js" "$ZEBKIT_ROOT/tools/zebkit-js/zebkit-js.c"
if [[ "$ZEBKIT_TARGET" == macos ]]; then
    keychain_source="$tests/macos_private_keychain.c"
    keychain_frameworks=(-framework Security -framework CoreFoundation)
    target_cc -std=c11 -O1 -Wall -Werror -dynamiclib "$keychain_source" "${keychain_frameworks[@]}" -o "$bin/macos-private-keychain.dylib"
    target_cc -std=c11 -O1 -Wall -Werror -DMACOS_KEYCHAIN_LAUNCH "$keychain_source" -o "$bin/macos-keychain-launch"
    target_cc -std=c11 -O1 -Wall -Werror -DMACOS_KEYCHAIN_CHECK "$keychain_source" "${keychain_frameworks[@]}" -o "$bin/macos-keychain-check"
fi
fi

engine_switches=(--no-sandbox --headless --user-data-dir="$work/profile")
failed=0

selected() { [[ -z "$only" || "$1" =~ $only ]]; }

# finished <name> <what>: its log ends with every check passed.
finished() {
    selected "$1" || return 0
    grep -q '^\([0-9]*\) of \1 checks passed$' "$work/$1.log" || { echo "FAIL $2 did not finish"; failed=1; }
}

# run <name> <command...>: a test program; its output, and the engine's on failure.
run() {
    local name="$1"
    shift
    selected "$name" || return 0
    log "$name"
    if ! (
        # Protected programs clear DYLD_*, so env is done here, before the launcher.
        if (( ${#launch[@]} )) && [[ "$1" == env ]]; then
            shift
            while [[ "$1" == *=* ]]; do export "${1%%=*}=${1#*=}"; shift; done
        fi
        limited "${run_limit:-180}" "${launch[@]}" "$@"
    ) > "$work/$name.log" 2> "$work/$name.err"; then
        failed=1
        cat "$work/$name.log"
        echo "FAIL $name: it did not exit 0"
        echo "it printed on stderr:"; tail -20 "$work/$name.err"
    else
        cat "$work/$name.log"
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
            | grep -vxE 'CrashForExceptionInNonABICompliantCodeRange|GetHandleVerifier|IsSandboxedProcess|sqlite3_dbdata_init')"
    elif [[ "$ZEBKIT_TARGET" == macos ]]; then
        # Mach-O puts an underscore before every C name.
        exported="$("${nm_tool[@]}" -gU "$library" | awk '{ print substr($3, 2) }')"
    else
        exported="$("${nm_tool[@]}" -D --defined-only "$library" | awk '{ print $3 }')"
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
        loads="$("${otool_tool[@]}" -L "$library" | tail -n +2 | awk '{ print $1 }')"
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
# The real long PDF the suite opens; nothing fails without it.
real_pdf="${ZEBKIT_TEST_REAL_PDF:-$CACHE_DIR/pdf/PDF32000_2008.pdf}"
if [[ -z "$prebuilt" && ! -f "$real_pdf" ]]; then
    mkdir -p "$(dirname "$real_pdf")"
    curl -fsSL --max-time 120 -o "$real_pdf.part" "$PDF_REAL_URL" \
        && [[ "$(sha256 "$real_pdf.part" | cut -d' ' -f1)" == "$PDF_REAL_SHA256" ]] \
        && mv "$real_pdf.part" "$real_pdf" || rm -f "$real_pdf.part"
fi
if [[ -n "$build_only" ]]; then
    check_engine_library
    # The Rust hook program, if this machine's toolchain has the target.
    rust_target=
    [[ "$ZEBKIT_TARGET" == macos && "$ZEBKIT_HOST" != macos ]] && rust_target=aarch64-apple-darwin
    [[ "$ZEBKIT_TARGET" == windows ]] && rust_target=x86_64-pc-windows-msvc
    if command -v cargo > /dev/null && { [[ -z "$rust_target" ]] || rustup target list --installed 2> /dev/null | grep -qx "$rust_target"; }; then
        log "building the Rust hook program"
        rust_options=()
        rust_env=(ZEBKIT_DIR="$package" CARGO_TARGET_DIR="$work/rust-target")
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
        (cd "$ZEBKIT_ROOT/rust" && env "${rust_env[@]}" RUSTFLAGS="$rust_flags" \
            cargo build -q -p zebkit --example hook_policies --example runtime_settings "${rust_options[@]}") \
            || die "the Rust hook program did not build"
        rust_exe=
        [[ "$ZEBKIT_TARGET" == windows ]] && rust_exe=.exe
        cp "$work/rust-target/${rust_target:+$rust_target/}debug/examples/hook_policies$rust_exe" "$bin/rust/hook_policies$rust_exe"
        cp "$work/rust-target/${rust_target:+$rust_target/}debug/examples/runtime_settings$rust_exe" "$bin/rust/runtime_settings$rust_exe"
    else
        log "hook-rust: not built (no cargo or no ${rust_target:-host} target)"
    fi
    (( failed )) && die "the engine's checks failed"
    log "copying what the suite needs into $build_only"
    mkdir -p "$build_only/out/blink/$profile"
    rm -rf "$build_only/out/blink/$profile/package"
    cp -a "$package" "$build_only/out/blink/$profile/package"
    ahem_relative="${SRC_DIR#"$ZEBKIT_ROOT"/}/third_party/pdfium/testing/resources/fonts/ahem/Ahem.ttf"
    tar -C "$ZEBKIT_ROOT" --exclude=__pycache__ -cf - scripts/engine-test.sh scripts/lib.sh upstream.lock profiles tests/serve.py \
        tests/engine bindings/python examples/pdf-plugin examples/native-host/sample.pdf "$ahem_relative" | tar -C "$build_only" -xf -
    if [[ -f "$real_pdf" ]]; then
        mkdir -p "$build_only/.cache/pdf"
        cp "$real_pdf" "$build_only/.cache/pdf/"
    fi
    log "built: run $build_only/scripts/engine-test.sh $profile --prebuilt $build_only/programs where they run"
    exit 0
fi
if [[ -z "$prebuilt" ]]; then
    check_engine_library
else
    log "what the engine exports and loads: checked by --build-only"
fi

# On Windows a DLL is found on PATH; python3 there may be the Store's stub.
if [[ "$ZEBKIT_HOST" == windows ]]; then
    # A copy of python.exe under that name, with the real one's library and a
    # pyvenv.cfg to find its standard library by: a script of that name is not a
    # program on a shared folder.
    mkdir -p "$work/path"
    python_home="$(python -c 'import sys; print(sys.base_prefix)' | tr -d '\r')"
    python_dll="$(python -c 'import sys; print("python%d%d" % sys.version_info[:2])' | tr -d '\r')"
    python_home="$(cygpath -u "$python_home")"
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
    ln -s "$(python3 -I -c 'import os, sys; print(os.path.realpath(sys.executable))')" "$work/path/python3"
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
        [[ "$(limited 30 "${launch[@]}" python3 -I -c 'import os; print(os.environ.get("DYLD_INSERT_LIBRARIES", ""))')" == "$ZEBKIT_TEST_KEYCHAIN_LIBRARY" ]] \
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
if [[ -x "$bin/host-loop-glib" ]]; then
    run host-loop-glib "$bin/host-loop-glib" --no-sandbox --headless --user-data-dir="$work/profile-host-loop-glib"
fi
mkdir -p "$work/startup-settings-files"
ZEBKIT_TEST_SETTINGS_DIR="$work/startup-settings-files" run startup-settings "$bin/startup-settings" --user-agent=argv-agent --user-data-dir="$work/unused-argv-profile"
for log_mode in none file; do
    run "startup-settings-python-$log_mode" env ZEBKIT_DIR="$package" PYTHONPATH="$ZEBKIT_ROOT/bindings/python" \
        ZEBKIT_TEST_LOG_MODE="$log_mode" python3 "$tests/startup_settings_python.py"
done
if [[ -x "$bin/native-parent" ]]; then
    run native-parent-refused "$bin/native-parent" --no-sandbox --headless --user-data-dir="$work/profile-native-parent-refused"
    if command -v Xvfb > /dev/null; then
        exec 9< <(Xvfb -displayfd 1 -screen 0 2200x1400x24 -nolisten tcp 2> /dev/null)
        xvfb=$!
        pids+=("$xvfb")
        read -r -t 15 -u 9 xvfb_display || xvfb_display=
        if [[ -n "$xvfb_display" ]]; then
            run native-parent env -u WAYLAND_DISPLAY DISPLAY=":$xvfb_display" "$bin/native-parent" \
                --no-sandbox --user-data-dir="$work/profile-native-parent"
            run mixed-views env -u WAYLAND_DISPLAY DISPLAY=":$xvfb_display" "$bin/mixed-views" \
                --no-sandbox --user-data-dir="$work/profile-mixed-views"
            run native-parent-python env -u WAYLAND_DISPLAY DISPLAY=":$xvfb_display" ZEBKIT_DIR="$package" \
                PYTHONPATH="$ZEBKIT_ROOT/bindings/python" python3 "$tests/native_parent_python.py" \
                --no-sandbox --user-data-dir="$work/profile-native-parent-python"
            if [[ -z "$prebuilt" ]] && command -v cargo > /dev/null; then
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
# Windows on the screen, and the keyboard for a moment: only when asked, on a person's Mac.
if [[ "$ZEBKIT_HOST" == macos ]]; then
    run gpu-status-headless python3 "$tests/gpu_status.py" "$bin/tools/zebkit-run" "${engine_switches[@]/profile/profile-gpu}"
    run native-parent-refused "$bin/native-parent-mac" --no-sandbox --headless --user-data-dir="$work/profile-native-parent-refused"
    if [[ -n "${ENGINE_TEST_WINDOWS:-}" ]]; then
        run mixed-views "$bin/mixed-views" --no-sandbox --user-data-dir="$work/profile-mixed-views"
        finished mixed-views "the mixed views' test"
        run native-parent "$bin/native-parent-mac" --no-sandbox --user-data-dir="$work/profile-native-parent"
        finished native-parent "the attached view's test"
        run gpu-status-window python3 "$tests/gpu_status.py" "$bin/tools/zebkit-run" --no-sandbox --user-data-dir="$work/profile-gpu-window"
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
    cp "$bin/win-smoke-sandbox.exe" "$bin/win-host-sandbox.exe" "$bin/win-native-parent-sandbox.exe" "$package/"
    run win-smoke "$bin/win-smoke" --no-sandbox --user-data-dir="$work/profile-win-smoke" "$work/win-smoke.png"
    run win-smoke-sandbox "$package/win-smoke-sandbox" --user-data-dir="$work/profile-win-smoke-sandbox" "$work/win-smoke-sandbox.png"
    run win-host "$bin/win-host" --no-sandbox --user-data-dir="$work/profile-win-host"
    run win-host-sandbox "$package/win-host-sandbox" --user-data-dir="$work/profile-win-host-sandbox"
    run win-offscreen "$bin/win-offscreen" --no-sandbox --headless --user-data-dir="$work/profile-win-offscreen"
    if selected win-sandbox-stubs; then
        log win-sandbox-stubs
        stub_code=0
        cp "$bin/win-sandbox-stubs.exe" "$package/"
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
    --expect-automation --user-data-dir="$work/profile-automation"
run mode-driver-port "$bin/mode" --no-sandbox --headless --remote-debugging-port=0 \
    --expect-automation --user-data-dir="$work/profile-driver-port"

port="${ENGINE_TEST_PORT:-8796}"
python3 -u "$ZEBKIT_ROOT/tests/serve.py" "$tests/pages" "$port" > "$work/server.log" 2>&1 &
pids+=($!)
sleep 1
mkdir -p "$work/files"
ZEBKIT_TEST_BASE="http://127.0.0.1:$port" ZEBKIT_TEST_OTHER="http://localhost:$port" \
    ZEBKIT_TEST_FILES="$work/files" run bridge "$bin/bridge" "${engine_switches[@]}"

ZEBKIT_TEST_BASE="http://127.0.0.1:$port" ZEBKIT_TEST_SHOTS="${ENGINE_TEST_SHOTS:-}" \
    run offscreen "$offscreen_test" "${engine_switches[@]}"
finished offscreen "the off-screen test"
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
run automation-input-held env ZEBKIT_DIR="$package" PYTHONPATH="$ZEBKIT_ROOT/bindings/python" \
    python3 "$tests/automation_input.py" --zebkit-test-no-input-wait --no-sandbox --headless --user-data-dir="$work/profile-input-held"
run user-agent-atomic python3 "$tests/user_agent_atomic.py" "$bin/tools/zebkit-run" "${engine_switches[@]}"
run user-agent-clients env ZEBKIT_DIR="$package" PYTHONPATH="$ZEBKIT_ROOT/bindings/python" \
    python3 "$tests/user_agent_clients.py"
run user-agent-native env ZEBKIT_DIR="$package" PYTHONPATH="$ZEBKIT_ROOT/bindings/python" \
    python3 "$tests/user_agent_native.py"
run offscreen-scale "$bin/offscreen-scale" "${engine_switches[@]}"
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
run certificate-c python3 "$tests/certificate_c.py" "$bin/certificate"
run http-auth-c python3 "$tests/http_auth_c.py" "$bin/http_auth"
if [[ -d "$CACHE_DIR/playwright-tools/node_modules/playwright-core" ]] && "$engine_test_node_ready"; then
    run playwright-connect "${pw_node[@]}" "$tests/playwright_connect.mjs" "$CACHE_DIR/playwright-tools/node_modules/playwright-core" \
        "$bin/tools/zebkit-run" --no-sandbox --headless --user-data-dir="$work/profile-connect"
else
    log "playwright-connect: skipped (no Node.js or playwright-core)"
fi
# Real CDP clients, Chrome's results as the reference (tests/engine/cdp_clients.mjs).
cdp_clients="$CACHE_DIR/cdp-clients"
if selected cdp-clients && "$engine_test_node_ready" && [[ "$ZEBKIT_HOST" == linux ]]; then
    if [[ "$(sed -n 's/.*"version": "\(.*\)".*/\1/p' "$cdp_clients/node_modules/puppeteer-core/package.json" 2> /dev/null | head -n 1)" != "$PUPPETEER_CORE_VERSION" \
        || "$(sed -n 's/.*"version": "\(.*\)".*/\1/p' "$cdp_clients/node_modules/playwright-core/package.json" 2> /dev/null | head -n 1)" != "$PLAYWRIGHT_CORE_VERSION" ]]; then
        pw_npm=(npm)
        command -v mise > /dev/null && pw_npm=(mise exec node@24 -- npm)
        log "installing puppeteer-core $PUPPETEER_CORE_VERSION and playwright-core $PLAYWRIGHT_CORE_VERSION"
        mkdir -p "$cdp_clients"
        (cd "$cdp_clients" && "${pw_npm[@]}" install --silent --no-audit --no-fund --no-save --ignore-scripts \
            "puppeteer-core@$PUPPETEER_CORE_VERSION" "playwright-core@$PLAYWRIGHT_CORE_VERSION") || log "cdp-clients: the install failed"
    fi
    # Unless named, the Chromium Playwright installed for itself, as it finds it:
    # in the user's cache, which lib.sh's XDG_CACHE_HOME would hide (its ffmpeg too).
    cdp_chrome="${CHROME:-$(cd "$cdp_clients" && env -u XDG_CACHE_HOME "${pw_node[@]}" -e "console.log(require('playwright-core').chromium.executablePath())" 2> /dev/null)}"
    if [[ ! -x "$cdp_chrome" ]]; then
        log "cdp-clients: skipped (no Chrome)"
    elif [[ -d "$cdp_clients/node_modules/puppeteer-core" ]]; then
        # Not run(): two browsers, one after the other, take longer than its limit.
        log cdp-clients
        if ! nice -n 19 ionice -c3 timeout -k 10 2400 env -u XDG_CACHE_HOME "${pw_node[@]}" "$tests/cdp_clients.mjs" "$cdp_clients" "$tests/cdp_clients_expected.txt" \
            --zebkit "$bin/tools/zebkit-run" --chrome "$cdp_chrome" --work "$work/cdp-clients" \
            > "$work/cdp-clients.log" 2> "$work/cdp-clients.err"; then
            failed=1
        fi
        cat "$work/cdp-clients.log"
    fi
else
    selected cdp-clients && log "cdp-clients: skipped (no Node.js)"
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
if [[ -z "$prebuilt" ]] && command -v cargo > /dev/null && "$engine_test_node_ready"; then
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
            ! selected mcp-agent-run || grep -q '^\([0-9]*\) of \1 checks passed$' "$work/mcp-agent-run.log" || { echo "FAIL the scripted MCP run did not finish"; failed=1; }
        else
            log "mcp-server: skipped (the MCP client could not be installed)"
        fi
    else
        echo "FAIL tools/zebkit-mcp did not build"; failed=1
    fi
else
    log "mcp-server: skipped (prebuilt, or no cargo or Node.js)"
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
    ZEBKIT_MEDIA_PROBE="$bin/media-probe" python3 "$tests/media_tools.py"
run media-tap env ZEBKIT_DIR="$package" PYTHONPATH="$ZEBKIT_ROOT/bindings/python" \
    ZEBKIT_MEDIA_PROBE="$bin/media-probe" \
    python3 "$tests/media_tap.py" "http://127.0.0.1:$port" "${engine_switches[@]}" \
    --autoplay-policy=no-user-gesture-required

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
ZEBKIT_MEDIA_PROBE="$bin/media-probe" run cdp-page python3 -I "$tests/cdp_page.py" "$bin/tools/zebkit-run" "http://127.0.0.1:$port" \
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
    run media-history "$bin/media" "${engine_switches[@]/profile/profile-history}" \
    --autoplay-policy=no-user-gesture-required
finished media-history "the media history test"
run cdp-browser python3 -I "$tests/cdp_browser.py" "$bin/tools/zebkit-run" "http://127.0.0.1:$port" \
    --no-sandbox --headless --user-data-dir="$work/profile-cdp-browser"
# What CDP changes at runtime, by what the page and a server saw (docs/ROADMAP.md).
run cdp-overrides python3 -I "$tests/cdp_overrides.py" "$bin/tools/zebkit-run" \
    --no-sandbox --headless --user-data-dir="$work/profile-cdp-overrides"
finished cdp-overrides "the CDP overrides test"
run pdf-in-page env ZEBKIT_TEST_REAL_PDF="$real_pdf" python3 -I "$tests/pdf_in_page.py" "$bin/tools/zebkit-run" \
    --no-sandbox --headless --user-data-dir="$work/profile-pdf-in-page"
finished pdf-in-page "the PDF in pages test"
# The PDF pane of the native hosts: built apart (examples/native-host/README.md) on Linux.
if [[ "$ZEBKIT_HOST" == linux ]] && command -v Xvfb > /dev/null; then
    for pdf_host in "$OUT_DIR/examples/gtk-host" "$OUT_DIR/examples/qt-host" \
        "$ZEBKIT_ROOT/examples/native-host/winit/target/debug/zebkit-winit-host"; do
        [[ -x "$pdf_host" ]] || { log "native-host-pdf: no $pdf_host"; continue; }
        # winit drops XTest's wheel: that host wheels itself.
        self_driven=()
        pdf_document="$([[ -f "$real_pdf" ]] && echo "$real_pdf" || echo generated)"
        # It is a debug build, and wheels by itself: the generated document is long enough.
        run_limit=180
        [[ "$pdf_host" == *winit* ]] && { self_driven=(--self); pdf_document=generated; run_limit=450; }
        run "native-host-pdf-$(basename "$pdf_host")" env LD_LIBRARY_PATH="$CACHE_DIR/native-toolkits/usr/lib/x86_64-linux-gnu" python3 -I "$tests/native_host_pdf_scroll.py" "${self_driven[@]}" "$pdf_host" \
            "$pdf_document" --no-sandbox --disable-gpu
    done
elif [[ "$ZEBKIT_HOST" == macos && -n "${ENGINE_TEST_WINDOWS:-}" ]]; then
    run_limit=450
    run native-host-appkit-pdf python3 -I "$tests/native_host_pdf_scroll.py" --self "$bin/native-host-appkit" \
        generated:40 --no-sandbox
    run_limit=180
fi

if [[ "$ZEBKIT_HOST" == linux ]]; then
    run sandbox-startup python3 -I "$tests/sandbox_startup.py" "$bin/tools/zebkit-run"
fi
run cookie-host python3 -I "$tests/cookie_host.py" "$bin/cookie-host" --no-sandbox --headless
run cookie-file python3 -I "$tests/cookie_file.py" "$bin/cookie-host" --no-sandbox --headless
run cookie-file-options python3 -I "$tests/cookie_file_options.py" "$bin/cookie-file-runner" --no-sandbox --headless

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
if [[ "$ZEBKIT_HOST" == macos || "$ZEBKIT_HOST" == windows || "${ENGINE_TEST_PERSISTENT_COOKIES:-0}" == 1 ]]; then
    run cookies python3 -I "$tests/cookies.py" "$bin/tools/zebkit-run" --no-sandbox --headless
fi

mkdir -p "$work/scheme-files"
ZEBKIT_TEST_FILES="$work/scheme-files" run scheme "$bin/scheme" "${engine_switches[@]}"

mkdir -p "$work/script-files"
# The headers the network adds, against the server, CDP and real Chrome (tests/engine/network_stages.py).
run network-stages env ZEBKIT_DIR="$package" PYTHONPATH="$ZEBKIT_ROOT/bindings/python:$tests" \
    python3 "$tests/network_stages.py" --no-sandbox --headless
finished network-stages "the network stages test"
# <zebkit/hooks.h> in every language, one engine per mode (tests/engine/hook_policies.py):
# links to other programs, downloads, client certificates, isolated worlds, file
# choosers, media, keys, context menus, fullscreen and the network.
hook_groups=(external download certificate world page network)
for hook_group in "${hook_groups[@]}"; do
    run "hook-c-$hook_group" env PYTHONPATH="$tests" python3 "$tests/hook_policies.py" "c:$hook_group" "$bin/hook-policies"
    run "hook-javascript-$hook_group" env PYTHONPATH="$tests" python3 "$tests/hook_policies.py" "javascript:$hook_group" \
        "$bin/tools/zebkit-js" "$tests/script/hook_policies.mjs"
    run "hook-python-$hook_group" env ZEBKIT_DIR="$package" PYTHONPATH="$ZEBKIT_ROOT/bindings/python:$tests" \
        python3 "$tests/hook_policies.py" "python:$hook_group" python3 "$tests/hook_policies_python.py"
done
run hook-python-popup env ZEBKIT_DIR="$package" PYTHONPATH="$ZEBKIT_ROOT/bindings/python:$tests" \
    python3 "$tests/hook_policies.py" "python:popup" python3 "$tests/hook_policies_python.py"
# What zebkit decides, changed while it runs, in every language (tests/engine/runtime_settings.py).
run runtime-settings-c python3 "$tests/runtime_settings.py" c "$bin/runtime-settings" @switches@
finished runtime-settings-c "the C runtime settings test"
run runtime-settings-python env ZEBKIT_DIR="$package" PYTHONPATH="$ZEBKIT_ROOT/bindings/python" \
    python3 "$tests/runtime_settings.py" python python3 "$tests/runtime_settings_python.py" @switches@
finished runtime-settings-python "the Python runtime settings test"
run runtime-settings-javascript python3 "$tests/runtime_settings.py" javascript \
    "$bin/tools/zebkit-js" @switches@ "$tests/script/runtime_settings.mjs"
finished runtime-settings-javascript "the JavaScript runtime settings test"
rust_hook_program=
rust_settings_program=
if [[ -n "$prebuilt" ]]; then
    [[ -x "$bin/rust/hook_policies" || -x "$bin/rust/hook_policies.exe" ]] && rust_hook_program="$bin/rust/hook_policies"
    [[ -x "$bin/rust/runtime_settings" || -x "$bin/rust/runtime_settings.exe" ]] && rust_settings_program="$bin/rust/runtime_settings"
elif command -v cargo > /dev/null; then
    log "building the Rust hook program"
    if (cd "$ZEBKIT_ROOT/rust" && ZEBKIT_DIR="$package" cargo build -q -p zebkit --example hook_policies --example runtime_settings); then
        rust_hook_program="$ZEBKIT_ROOT/rust/target/debug/examples/hook_policies"
        rust_settings_program="$ZEBKIT_ROOT/rust/target/debug/examples/runtime_settings"
    else
        echo "FAIL the Rust hook program did not build"; failed=1
    fi
fi
if [[ -n "$rust_hook_program" ]]; then
    for hook_group in "${hook_groups[@]}"; do
        run "hook-rust-$hook_group" env LD_LIBRARY_PATH="$package" PYTHONPATH="$tests" python3 "$tests/hook_policies.py" \
            "rust:$hook_group" "$rust_hook_program"
    done
else
    log "hook-rust: skipped (no program)"
fi
if [[ -n "$rust_settings_program" ]]; then
    run runtime-settings-rust env LD_LIBRARY_PATH="$package" python3 "$tests/runtime_settings.py" rust "$rust_settings_program" @switches@
    finished runtime-settings-rust "the Rust runtime settings test"
fi

run hooks-javascript "$bin/tools/zebkit-js" --no-sandbox --headless --user-data-dir="$work/profile-hooks-javascript" "$tests/script/hooks.mjs" "http://127.0.0.1:$port"
run certificate-bindings python3 "$tests/certificate_bindings.py" "$bin/tools/zebkit-js"
run http-auth-bindings python3 "$tests/http_auth_bindings.py" "$bin/tools/zebkit-js"
run user-agent-bindings python3 "$tests/user_agent_bindings.py" "$bin/tools/zebkit-js"
run window-open-javascript "$bin/tools/zebkit-js" --no-sandbox --headless --user-data-dir="$work/profile-window-open-javascript" "$tests/script/window_open.mjs" "http://127.0.0.1:$port"
! selected window-open-javascript || grep -q '^ok JavaScript original popup adoption' "$work/window-open-javascript.log" || { echo "FAIL window-open script did not finish"; failed=1; }
run script "$bin/tools/zebkit-js" "${engine_switches[@]}" "$tests/script/app.mjs" \
    "http://127.0.0.1:$port" "$work/script-files"
# It ends by itself once idle; ending before its last line is a failure.
finished script "the script"

run pdf-plugin "$bin/tools/zebkit-js" "${engine_switches[@]}" "$tests/script/pdf_plugin.mjs" "$ahem"
finished pdf-plugin "the PDF plugin's script"

run ui "$bin/tools/zebkit-js" "${engine_switches[@]}" "$tests/ui/app.mjs"
finished ui "zebkit.ui's script"

ZEBKIT_DIR="$package" PYTHONPATH="$ZEBKIT_ROOT/bindings/python" \
    run python-services python3 "$tests/test_python.py" --services-only
ZEBKIT_DIR="$package" PYTHONPATH="$ZEBKIT_ROOT/bindings/python" \
    run python python3 "$tests/test_python.py" "${engine_switches[@]}"

if selected cdp-command-line; then
log "CDP from the command line"
"${launch[@]}" "$bin/tools/zebkit-run" "${engine_switches[@]}" --remote-debugging-port=0 about:blank \
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
fi

if [[ "$ZEBKIT_HOST" == windows ]]; then
    # Nothing of the suite's in the person's own application data.
    if [[ "$( (ls -A "$real_local_app_data/zebkit" 2> /dev/null || true) | wc -l)" != "$zebkit_before" || -e "$real_local_app_data/content_shell" ]]; then
        echo "FAIL the suite wrote in %LOCALAPPDATA%"; failed=1
    else
        echo "ok   nothing written in %LOCALAPPDATA%"
    fi
fi
(( failed )) && die "engine tests failed"
log "engine tests passed"
