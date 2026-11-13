#!/usr/bin/env bash
# What a JavaScript library costs (docs/JS_LIBRARIES.md, "Measuring"):
# tests/bench/library/bench.c on a package, in fresh processes, the medians
# of what each one measured.
#
#   scripts/library-bench.sh [profile] [--runs N]     profile: slim; N: 20
#
# With the bundles of examples/js-libraries (examples/js-libraries/build.sh,
# or engine-test.sh --fetch) it measures markdown-it and ECharts too.

source "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

profile=slim runs=20
while (( $# )); do
    case "$1" in
        --runs) runs="${2:?a number}"; shift 2 ;;
        -*) die "unknown option $1" ;;
        *) profile="$1"; shift ;;
    esac
done
package="$OUT_DIR/$profile/package"
[[ -f "$(engine_library "$package")" ]] || die "no engine in $package; run scripts/package.sh $profile first"
work="$(mktemp -d "$TMPDIR/library-bench.XXXXXX")"
trap 'rm -rf "$work"' EXIT
cc -std=c11 -O2 -Wall -Werror -I"$package/include" "$ZEBKIT_ROOT/tests/bench/library/bench.c" \
    -L"$package" -l:libzebkit.so.1 -Wl,-rpath,"$package" -o "$work/bench" || die "the benchmark did not build"
bundles=()
[[ -f "$CACHE_DIR/js-libraries/charts/index.mjs" ]] && bundles=("$CACHE_DIR/js-libraries/markdown" "$CACHE_DIR/js-libraries/charts")
for (( i = 0; i < runs; ++i )); do
    limited 120 nice "$work/bench" "$ZEBKIT_ROOT/tests/bench/library/echo" ${bundles[@]+"${bundles[@]}"} \
        --headless --no-sandbox --user-data-dir="$work/profile-$i" 2> "$work/err-$i" | tail -1 >> "$work/runs.jsonl" \
        || { cat "$work/err-$i" >&2; die "run $i failed"; }
done
python3 -I - "$work/runs.jsonl" <<'PY'
import json, statistics, sys
runs = [json.loads(line) for line in open(sys.argv[1]) if line.strip()]
print("%d runs, medians:" % len(runs))
for key in runs[0]:
    print("  %-22s %10.2f" % (key, statistics.median(run[key] for run in runs)))
PY
