#!/usr/bin/env bash
# zebkit and the latest stable Electron, measured the same way: the same
# harness, pages and switches, one host each that does the same thing
# (tests/bench/equal/host.c and electron/main.js), packages as they ship, on
# one quiet machine, the engines taking turns. docs/SIZE_AND_SPEED.md has what
# "the same" covers and what could not be made the same.
#
#   scripts/electron-compare.sh [--profile PROFILE] [--series a,b] [--runs N] [--switch S]...
#   scripts/electron-compare.sh --report
#
# Series: probe, cold, warm, layout, js, calls, document, pages (or page:<name>),
# sites (or site:<host/path>), size. By default all; runs are 5, 7 for startup and
# layout. Results accumulate in out/blink/electron-compare/results.jsonl, and
# --report writes docs/reports/electron-compare.md and .json from them; delete
# that file to start again.
#
# Needs Xvfb, Python 3, out/blink/<profile>/package (scripts/package.sh) and
# Electron at upstream.lock's version (scripts/electron-baseline.sh).

source "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

profile=slim
series=""
runs=5
report=0
# Both without their sandbox: each needs a set-user-ID helper or user
# namespaces, which a build machine and a container often do not give.
switches=(--switch=--no-sandbox)
while (( $# )); do
    case "$1" in
        --profile) profile="$2"; shift ;;
        --series) series="$2"; shift ;;
        --runs) runs="$2"; shift ;;
        --switch) switches+=(--switch="$2"); shift ;;
        --report) report=1 ;;
        *) die "usage: $(basename "$0") [--profile PROFILE] [--series a,b] [--runs N] [--switch S] | --report" ;;
    esac
    shift
done

bench="$ZEBKIT_ROOT/tests/bench"
package="$OUT_DIR/$profile/package"
electron_dir="$OUT_DIR/electron/package"
work="$OUT_DIR/electron-compare"
mkdir -p "$work"
[[ -d "$package" ]] || die "no package in $package: scripts/package.sh $profile"
[[ -x "$electron_dir/electron" && "$(cat "$electron_dir/version")" == "$ELECTRON_VERSION" ]] \
    || die "no Electron $ELECTRON_VERSION in $electron_dir: scripts/electron-baseline.sh"

mib() { awk -v b="$1" 'BEGIN { printf "%.1f", b / 1048576 }'; }
bytes() { find "$@" -type f -printf '%s\n' | awk '{ s += $1 } END { print s + 0 }'; }

# The hosts: the zebkit one built against the package, as an application is;
# the Electron one in resources/app, as electron-packager puts it.
host="$work/zebkit-host"
build_against_package "$package" "$host" "$bench/equal/host.c"
electron="$work/electron"
if [[ ! -x "$electron/electron" || "$(cat "$electron/version")" != "$ELECTRON_VERSION" ]]; then
    rm -rf "$electron"
    cp -a "$electron_dir" "$electron"
fi
rm -rf "$electron/resources/app"
cp -a "$bench/equal/electron" "$electron/resources/app"

# What is shipped, each as it comes and as an application ships it: the runtime
# trimmed as far as it goes without a build, the application added. For zebkit
# that is the package without the headers an application is built against.
size_series() {
    local ship="$work/size"
    rm -rf "$ship"
    mkdir -p "$ship"
    cp -a "$package" "$ship/zebkit"
    rm -rf "$ship/zebkit/include"
    cp -a "$host" "$ship/zebkit/application"
    cp -a "$electron_dir" "$ship/electron"
    rm -rf "$ship/electron/resources/app"
    cp -a "$bench/equal/electron" "$ship/electron/resources/app"
    find "$ship/electron/locales" -name '*.pak' ! -name en-US.pak -delete
    rm -f "$ship/electron/LICENSES.chromium.html"
    {
        printf '{"package_bytes": %d, "package_xz": %d, "package_zip": %d, ' \
            "$(bytes "$package")" "$(compressed_size xz "$package")" "$(compressed_size zip "$package")"
        printf '"shipped_bytes": %d, "shipped_xz": %d, "shipped_zip": %d, ' \
            "$(bytes "$ship/zebkit")" "$(compressed_size xz "$ship/zebkit")" "$(compressed_size zip "$ship/zebkit")"
        printf '"electron_package_bytes": %d, "electron_package_xz": %d, "electron_package_zip": %d, ' \
            "$(bytes "$electron_dir")" "$(compressed_size xz "$electron_dir")" "$(compressed_size zip "$electron_dir")"
        printf '"electron_shipped_bytes": %d, "electron_shipped_xz": %d, "electron_shipped_zip": %d}\n' \
            "$(bytes "$ship/electron")" "$(compressed_size xz "$ship/electron")" "$(compressed_size zip "$ship/electron")"
    } > "$work/size.json"
    rm -rf "$ship"
    cat "$work/size.json"
}

if (( report )); then
    python3 -I "$bench/equal/report-meta.py" "$work/size.json" "$ELECTRON_VERSION" "$CHROMIUM_VERSION" "$(cat "$electron_dir/version")" "$profile" > "$work/meta.json"
    exec python3 -I "$bench/equal/compare.py" report --work "$work" --out "$REPORTS_DIR" --meta "$work/meta.json"
fi

if [[ -z "$series" || ",$series," == *,size,* ]]; then
    size_series
    [[ "$series" == size ]] && exit 0
fi

pdf="$CACHE_DIR/pdf/PDF32000_2008.pdf"
[[ -f "$pdf" ]] || pdf=""
ua="Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/${CHROMIUM_VERSION%%.*}.0.0.0 Safari/537.36"
exec python3 -I "$bench/equal/compare.py" run --work "$work" --zebkit "$host" --package "$package" \
    --electron "$electron/electron" --bench "$bench" ${pdf:+--pdf "$pdf"} --ua "$ua" --runs "$runs" \
    ${series:+--series "$series"} "${switches[@]}"
