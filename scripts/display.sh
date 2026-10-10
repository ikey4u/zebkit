#!/usr/bin/env bash
# Check that a built profile picks the right display backend and works on it.
# Three situations are set up privately, without touching the desktop session:
#
#   wayland   a headless Wayland compositor AND an X server are running:
#             the engine must choose Wayland
#   x11       only an X server is running: the engine must open an X window,
#             draw the page into it and receive mouse, wheel and key input
#   none      neither is running: the engine must still load the page,
#             rendering offscreen
#
# Two more runs make the loader treat libraries as not installed
# (ZEBKIT_WITHOUT_LIBRARIES): without libwayland-client the engine must fall
# back to X11, and without any of the optional libraries it must still start
# and render offscreen.
#
# tests/display/index.html reports to a local server what reaches the page.
# Needs Xvfb, xdotool, ffmpeg and labwc on the build host.
#
#   scripts/display.sh <profile>

source "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

profile="${1:-}"
require_profile "$profile"
stage_profile "$profile"

for tool in Xvfb xdotool ffmpeg labwc; do
    command -v "$tool" > /dev/null || die "$tool is needed for this test"
done

port="${DISPLAY_PORT:-8801}"
xdisplay=":${DISPLAY_NUMBER:-97}"
work="$(mktemp -d)"
pids=()
cleanup() { kill "${pids[@]}" 2>/dev/null || true; rm -rf "$work"; }
trap cleanup EXIT

python3 -u "$ZEBKIT_ROOT/tests/serve.py" "$ZEBKIT_ROOT/tests/display" "$port" > "$work/server.log" 2>&1 &
pids+=($!)
Xvfb "$xdisplay" -screen 0 1280x800x24 -nolisten tcp > "$work/xvfb.log" 2>&1 &
pids+=($!)
mkdir -m 700 "$work/wayland" "$work/empty"
XDG_RUNTIME_DIR="$work/wayland" WLR_BACKENDS=headless WLR_LIBINPUT_NO_DEVICES=1 WLR_RENDERER=pixman \
    env -u DISPLAY -u WAYLAND_DISPLAY labwc > "$work/labwc.log" 2>&1 &
pids+=($!)
sleep 3
wayland_socket="$(find "$work/wayland" -maxdepth 1 -name 'wayland-*' ! -name '*.lock' -printf '%f\n' | head -1)"
[[ -n "$wayland_socket" ]] || die "the test compositor did not start; see $work/labwc.log"

failures=0
check() { # check <ok?> <description>
    if [[ "$1" == yes ]]; then echo "  PASS  $2"; else echo "  FAIL  $2"; failures=$(( failures + 1 )); fi
}
reported() { grep -oE "GET /event\?$1=[^ ]*" "$work/server.log" | grep -c "run=$run" || true; }
has() { [[ "$1" == *"$2"* ]] && echo yes || echo no; }

# start_browser <name> <env assignments...>: run MiniBrowser with a clean
# display environment plus the given variables; sets browser_pid and libs.
start_browser() {
    run="$1"; shift
    : > "$work/server.log"
    "${run_env[@]}" env -u DISPLAY -u WAYLAND_DISPLAY XDG_RUNTIME_DIR="$work/empty" ZEBKIT_SYSLIB_LOG=1 "$@" \
        "$build_dir/bin/MiniBrowser" "http://127.0.0.1:$port/index.html?run=$run" > "$work/$run.log" 2>&1 &
    browser_pid=$!
    sleep 8
    # Which display libraries the engine itself loaded. (The process map would
    # not tell: the system's GL driver may load libwayland-client on its own.)
    libs="$(grep -oE 'optional library lib(X11|wayland-client)\.so[.0-9]* loaded' "$work/$run.log" | grep -oE 'lib(X11|wayland-client)\.so' | sort -u | paste -sd' ' || true)"
}
stop_browser() { kill "$browser_pid" 2>/dev/null || true; wait "$browser_pid" 2>/dev/null || true; }
page_loaded() { grep -q "GET /index.html?run=$run" "$work/server.log" && grep -q "GET /event?loaded=" "$work/server.log" && echo yes || echo no; }

log "wayland and X both available"
start_browser wayland XDG_RUNTIME_DIR="$work/wayland" WAYLAND_DISPLAY="$wayland_socket" DISPLAY="$xdisplay"
check "$(page_loaded)" "page loads"
check "$(has "$libs" libwayland-client.so)" "Wayland client library is in use"
check "$([[ "$libs" != *libX11.so* ]] && echo yes || echo no)" "X11 was not touched: Wayland is preferred"
check "$([[ -z "$(DISPLAY="$xdisplay" xdotool search --onlyvisible --name 'zebkit display test' 2>/dev/null)" ]] && echo yes || echo no)" "no window on the X server"
stop_browser

log "X only"
start_browser x11 DISPLAY="$xdisplay"
check "$(page_loaded)" "page loads"
check "$(has "$libs" libX11.so)" "X11 library is in use"
window="$(DISPLAY="$xdisplay" xdotool search --onlyvisible --name 'zebkit display test' 2>/dev/null | head -1 || true)"
check "$([[ -n "$window" ]] && echo yes || echo no)" "an X window with the page title exists"
ffmpeg -v error -y -f x11grab -video_size 1280x800 -i "$xdisplay" -frames:v 1 "$work/x11.png" || true
pixels="$(python3 - "$work/x11.png" <<'PY' 2>/dev/null || true
import sys
from PIL import Image
image = Image.open(sys.argv[1]).convert("RGB")
print("%02x%02x%02x %02x%02x%02x" % (image.getpixel((250, 200)) + image.getpixel((700, 500))))
PY
)"
check "$([[ "$pixels" == "ffb300 1565c0" ]] && echo yes || echo no)" "the window shows the page (pixels: ${pixels:-none})"
DISPLAY="$xdisplay" xdotool mousemove 250 200 click 1 2>/dev/null || true
sleep 0.5
DISPLAY="$xdisplay" xdotool click 5 2>/dev/null || true
sleep 0.5
DISPLAY="$xdisplay" xdotool mousemove 200 355 click 1 type 'abc' 2>/dev/null || true
sleep 2
check "$(grep -q 'GET /event?click=250%2C200%2Cbutton0' "$work/server.log" && echo yes || echo no)" "mouse click arrives at the right place"
check "$(grep -q 'GET /event?move=' "$work/server.log" && echo yes || echo no)" "pointer motion arrives"
check "$(grep -q 'GET /event?wheel=1' "$work/server.log" && echo yes || echo no)" "wheel scrolling arrives, scrolling down"
check "$(grep -q 'GET /event?typed=abc' "$work/server.log" && echo yes || echo no)" "typed text arrives"
stop_browser

log "both servers running, but no libwayland-client installed"
start_browser no-wayland-library XDG_RUNTIME_DIR="$work/wayland" WAYLAND_DISPLAY="$wayland_socket" DISPLAY="$xdisplay" \
    ZEBKIT_WITHOUT_LIBRARIES=libwayland-client.so.0
check "$(page_loaded)" "page loads"
check "$([[ "$libs" == "libX11.so" ]] && echo yes || echo no)" "falls back to X11 (libraries in use: ${libs:-none})"
check "$([[ -n "$(DISPLAY="$xdisplay" xdotool search --onlyvisible --name 'zebkit display test' 2>/dev/null)" ]] && echo yes || echo no)" "an X window exists"
stop_browser

log "both servers running, but neither library installed"
start_browser no-libraries XDG_RUNTIME_DIR="$work/wayland" WAYLAND_DISPLAY="$wayland_socket" DISPLAY="$xdisplay" \
    ZEBKIT_WITHOUT_LIBRARIES=libwayland-client.so.0:libX11.so.6:libgbm.so.1:libdrm.so.2:libasound.so.2
check "$(page_loaded)" "page loads, rendered offscreen"
check "$([[ -z "$libs" ]] && echo yes || echo no)" "no display library is in use (found: ${libs:-none})"
stop_browser

log "neither Wayland nor X"
start_browser none
check "$(page_loaded)" "page loads, rendered offscreen"
check "$([[ "$libs" != *libX11.so* ]] && echo yes || echo no)" "X11 was not touched"
stop_browser

(( failures == 0 )) || die "$failures display check(s) failed for profile '$profile'; logs were in $work"
log "display backends OK"
