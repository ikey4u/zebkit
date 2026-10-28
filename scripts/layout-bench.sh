#!/usr/bin/env bash
# Text layout in zebkit and in Electron: a chapter of a book, paginated in
# columns, laid out again after its width changes by a pixel, as a reader
# lays it out when its window is resized (tests/bench/web/local/layout.html).
#
#   scripts/layout-bench.sh [--profile PROFILE]
#
# Each engine opens the page BENCH_RUNS (5) times, in a fresh process with
# a fresh profile; the page reports the median of its 50 layouts, and the
# median of the runs is printed. Same display (Xvfb), same fonts, same user
# agent as scripts/web-bench.sh, whose Electron this uses. Run it on an
# otherwise idle machine.

source "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

profile=slim
while (( $# )); do
    case "$1" in
        --profile) profile="$2"; shift ;;
        *) die "usage: $(basename "$0") [--profile PROFILE]" ;;
    esac
    shift
done

bench="$ZEBKIT_ROOT/tests/bench/web"
package="$OUT_DIR/$profile/package"
work="$OUT_DIR/layout-bench"
electron_dir="$OUT_DIR/electron/package"
[[ -d "$package" ]] || die "no package in $package: scripts/package.sh $profile"
[[ -x "$electron_dir/electron" ]] || die "no Electron in $electron_dir: scripts/electron-baseline.sh"
command -v Xvfb > /dev/null || die "Xvfb is missing"

mkdir -p "$work"
build_against_package "$package" "$work/zebkit-run" "$ZEBKIT_ROOT/tools/zebkit-run/zebkit-run.c"
rm -rf "$work/electron"
cp -a "$electron_dir" "$work/electron"
rm -rf "$work/electron/resources/app"
cp -a "$bench/electron-app" "$work/electron/resources/app"

display=:$((90 + RANDOM % 9))
Xvfb "$display" -screen 0 1920x1080x24 > /dev/null 2>&1 &
xvfb=$!
trap 'kill $xvfb 2>/dev/null || true' EXIT
sleep 1
export DISPLAY="$display"
unset WAYLAND_DISPLAY
export BENCH_UA="Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/${CHROMIUM_VERSION%%.*}.0.0.0 Safari/537.36"

url="file://$bench/local/layout.html"
runs="${BENCH_RUNS:-5}"

# run <port> <command...>: the page's median, read from its title over CDP.
run() {
    local port="$1"
    shift
    "$@" > /dev/null 2>&1 &
    local pid=$! title="" i
    for i in $(seq 300); do
        sleep 0.1
        title="$(curl -fs "http://127.0.0.1:$port/json/list" 2>/dev/null \
            | python3 -I -c 'import json, sys; print(next((t["title"] for t in json.load(sys.stdin) if t["type"] == "page"), ""))' 2>/dev/null || true)"
        [[ "$title" == layout\ * ]] && break
    done
    kill "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
    [[ "$title" == layout\ * ]] || die "the page gave no time ($*)"
    title="${title#layout }"
    echo "${title% ms}"
}

median() { sort -g | awk '{ v[NR] = $1 } END { print (NR % 2) ? v[(NR + 1) / 2] : (v[NR / 2] + v[NR / 2 + 1]) / 2 }'; }

zebkit=() electron=()
for i in $(seq "$runs"); do
    rm -rf "$work/profile-zebkit" "$work/profile-electron"
    zebkit+=("$(run 9421 "$work/zebkit-run" --no-sandbox --remote-debugging-port=9421 \
        --user-data-dir="$work/profile-zebkit" --window-size=1280,800 "$url")")
    electron+=("$(BENCH_URL="$url" run 9422 "$work/electron/electron" --no-sandbox --ozone-platform=x11 \
        --remote-debugging-port=9422 --user-data-dir="$work/profile-electron")")
    log "run $i of $runs: zebkit ${zebkit[-1]} ms, Electron ${electron[-1]} ms"
done
z="$(printf '%s\n' "${zebkit[@]}" | median)"
e="$(printf '%s\n' "${electron[@]}" | median)"
log "text layout, the median of $runs runs: zebkit $z ms, Electron $(cat "$electron_dir/version") $e ms"
