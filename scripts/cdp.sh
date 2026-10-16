#!/usr/bin/env bash
# Drive a built profile over the Chrome DevTools Protocol and check that it
# behaves as CDP clients expect.
#
#   scripts/cdp.sh <profile>              raw protocol checks (tests/cdp/protocol.py)
#   scripts/cdp.sh <profile> --clients    also Puppeteer and Playwright (needs Node; see below)
#   scripts/cdp.sh <profile> --serve      leave the browser running for manual use
#
# The browser under test is tools/cdphost, a minimal application embedding
# the engine. Test pages come from two local servers on different ports, so
# that one is a different origin from the other.
#
# The raw checks include one run with discovery switched off
# (tests/cdp/private.py). They also compare every message with the standard
# protocol definition in deps/devtools-protocol, and the same checks
# run against Chrome itself with scripts/cdp-reference.sh.
#
# The client tests need Node.js and the npm packages puppeteer-core and
# playwright-core. Neither is part of this repository; --clients downloads
# them into .cache/ on first use.

source "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

profile="${1:-}"
require_profile "$profile"
mode="${2:-}"

port="${CDP_PORT:-9333}"
page_port="${CDP_PAGE_PORT:-8771}"
other_port=$(( page_port + 1 ))

stage_profile "$profile"

# The table the endpoint checks parameters against is generated from the
# protocol definition and the sources. A stale one is a build that does not
# match what is tested here.
python3 -I "$ZEBKIT_ROOT/scripts/gen-cdp-protocol.py" --check || die "regenerate src/cdp/CDPProtocol.inc and rebuild"

# The endpoint's portable files must stay portable: what belongs to one
# platform goes through src/cdp/CDPPlatform.h (the port to another platform
# replaces only the files named after this one).
portable=("$ZEBKIT_ROOT"/src/cdp/CDPServer.cpp "$ZEBKIT_ROOT"/src/cdp/CDPPage.cpp "$ZEBKIT_ROOT"/src/cdp/CDPDomains.cpp
          "$ZEBKIT_ROOT"/src/cdp/CDPView.cpp "$ZEBKIT_ROOT"/src/cdp/CDPWebSocket.cpp)
if leaks="$(grep -nE '\b(webkit_[a-z_]+|wpe_[a-z_]+|soup_[a-z_]+|g_[a-z_]+)\(|\b(GRefPtr|GUniquePtr|GError|gpointer|WebKitWebView|WEBKIT_[A-Z_]+|Soup[A-Z][A-Za-z]+)\b|#include <(glib|gio|libsoup|wpe)' "${portable[@]}")"; then
    echo "$leaks" >&2
    die "platform API used in the endpoint's portable files"
fi

host="$build_dir/tools/cdphost"
if [[ "$ZEBKIT_HOST" == windows ]]; then
    # A program for Windows, built here like the engine and run under Wine
    # (scripts/wine.sh, which looks for it beside the engine).
    host=cdphost.exe
    source_file="$ZEBKIT_ROOT/tools/cdphost/cdphost-win.c"
    if [[ ! -f "$build_dir/bin/$host" || "$source_file" -nt "$build_dir/bin/$host" || "$build_dir/lib/WebKit2.lib" -nt "$build_dir/bin/$host" ]]; then
        log "building cdphost for $profile"
        sysroot="$WINDOWS_TOOLCHAIN_DIR/sysroot"
        mkdir -p "$build_dir/tools"
        "$WINDOWS_TOOLCHAIN_DIR/llvm/bin/clang-cl" /nologo /MD /O1 /W3 -D_CRT_SECURE_NO_WARNINGS -Wno-unused-function \
            -imsvc "$sysroot/crt/include" -imsvc "$sysroot/sdk/include/ucrt" -imsvc "$sysroot/sdk/include/um" -imsvc "$sysroot/sdk/include/shared" \
            -I "$ZEBKIT_ROOT/src/cdp" -I "$build_dir/WebKit/Headers" "$source_file" "/Fo$build_dir/tools/" -fuse-ld=lld \
            /link "-libpath:$sysroot/crt/lib/x64" "-libpath:$sysroot/sdk/lib/ucrt/x64" "-libpath:$sysroot/sdk/lib/um/x64" \
            "$build_dir/lib/WebKit2.lib" user32.lib "/out:$build_dir/bin/$host"
    fi
elif [[ "$ZEBKIT_HOST" == macos ]]; then
    source_file="$ZEBKIT_ROOT/tools/cdphost/cdphost.m"
    # Rebuilt when its source changes, and when the engine it links against does.
    if [[ ! -x "$host" || "$source_file" -nt "$host" || "$products_dir/WebKit.framework/WebKit" -nt "$host" ]]; then
        log "building cdphost for $profile"
        mkdir -p "$(dirname "$host")"
        clang -fobjc-arc -O1 -Wall -Wno-deprecated-declarations -I "$ZEBKIT_ROOT/src/cdp" -o "$host" "$source_file" \
            -F"$products_dir" -framework WebKit -framework Cocoa
    fi
else
    source_file="$ZEBKIT_ROOT/tools/cdphost/cdphost.c"
    pc_path="$stage_dir/lib/pkgconfig"
    [[ "${run_env[*]}" == *"$OUT_DIR/deps/prefix"* ]] && pc_path+=":$OUT_DIR/deps/prefix/lib/pkgconfig"
    # Rebuilt when its source changes, and when the engine it links against does.
    if [[ ! -x "$host" || "$source_file" -nt "$host" || "$stage_dir/lib/pkgconfig/wpe-webkit-2.0.pc" -nt "$host" ]]; then
        log "building cdphost for $profile"
        mkdir -p "$(dirname "$host")"
        # shellcheck disable=SC2046
        PKG_CONFIG_PATH="$pc_path" cc -O1 -Wall -I "$ZEBKIT_ROOT/src/cdp" -o "$host" "$source_file" \
            $(PKG_CONFIG_PATH="$pc_path" pkg-config --cflags --libs wpe-webkit-2.0 wpe-platform-headless-2.0)
    fi
fi

work="$(mktemp -d)"
pids=()
# end_browser: under Wine the browser's processes are not this script's.
end_browser() {
    [[ "$ZEBKIT_HOST" != windows ]] || "$ZEBKIT_ROOT/scripts/wine.sh" --stop
}
trap 'end_browser; kill "${pids[@]}" 2>/dev/null || true; rm -r "$work"' EXIT

python3 -u "$ZEBKIT_ROOT/tests/serve.py" "$ZEBKIT_ROOT/tests/cdp/pages" "$page_port" > "$work/pages.log" 2>&1 &
pids+=($!)
python3 -u "$ZEBKIT_ROOT/tests/serve.py" "$ZEBKIT_ROOT/tests/cdp/pages" "$other_port" > "$work/other.log" 2>&1 &
pids+=($!)

# start_browser [cdphost options]: each start has its own section of the log.
start_browser() {
    local started
    started="$(grep -c "^DevTools listening on" "$build_dir/cdp.log" || true)"
    "${run_env[@]}" "$host" --headless --port "$port" "$@" "http://127.0.0.1:$page_port/index.html" \
        >> "$build_dir/cdp.log" 2>&1 &
    browser_pid=$!
    pids+=("$browser_pid")
    # Ten seconds; a minute under Wine, which starts slowly.
    for _ in $(seq "$([[ "$ZEBKIT_HOST" == windows ]] && echo 300 || echo 50)"); do
        (( $(grep -c "^DevTools listening on" "$build_dir/cdp.log" || true) > started )) && return
        kill -0 "$browser_pid" 2>/dev/null || die "cdphost exited; see $build_dir/cdp.log"
        sleep 0.2
    done
    die "the CDP endpoint did not come up on port $port; see $build_dir/cdp.log"
}

stop_browser() {
    end_browser
    kill "$browser_pid" 2>/dev/null || true
    wait "$browser_pid" 2>/dev/null || true
}

: > "$build_dir/cdp.log"
start_browser

if [[ "$mode" == "--serve" ]]; then
    log "CDP endpoint: http://127.0.0.1:$port (pages on $page_port and $other_port); Ctrl-C to stop"
    wait "$browser_pid"
    exit 0
fi

failed=0
log "raw protocol checks"
python3 -I "$ZEBKIT_ROOT/tests/cdp/protocol.py" "$port" "$page_port" "$other_port" || failed=1
stop_browser

log "endpoint without discovery"
start_browser --private
python3 -I "$ZEBKIT_ROOT/tests/cdp/private.py" "$port" "$build_dir/cdp.log" || failed=1
stop_browser

if [[ "$mode" == "--clients" ]]; then
    node_dir="$CACHE_DIR/cdp-clients"
    "$ZEBKIT_ROOT/scripts/fetch-cdp-clients.sh" "$node_dir"
    node="$node_dir/node/bin/node"
    for client in puppeteer playwright; do
        log "$client"
        start_browser
        NODE_PATH="$node_dir/node_modules" "$node" "$ZEBKIT_ROOT/tests/cdp/$client.js" "$port" "$page_port" "$other_port" || failed=1
        stop_browser
    done
fi

(( failed == 0 )) || die "CDP checks failed for profile '$profile'; browser log: $build_dir/cdp.log"
log "PASS"
