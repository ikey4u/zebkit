#!/usr/bin/env bash
# What a JavaScript library costs (docs/JS_LIBRARIES.md, "Measuring"):
# tests/bench/library/bench.c on a package, in fresh processes, the medians
# of what each one measured, written to docs/reports/library-bench.md.
#
#   scripts/library-bench.sh [profile] [--runs N] [--backends LIST] [--package DIR] [--no-report]
#
# profile: slim; N: 20; DIR: out/blink/<profile>/package (a copy measures
# while the tree builds on); LIST: "isolate page", each a column of the report (a
# backend the engine refuses is left out, with why). Each run is a fresh
# process with a new profile, then another on the same profile: the second
# loads with what the first left on disk (the code cache). With the bundles of
# examples/js-libraries (examples/js-libraries/build.sh, or engine-test.sh
# --fetch) it measures markdown-it and ECharts too. Run it on a quiet machine.

source "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

profile=slim runs=20 backends="isolate page" report=1 package=
while (( $# )); do
    case "$1" in
        --runs) runs="${2:?a number}"; shift 2 ;;
        --package) package="$(caller_path "${2:?a folder}")"; shift 2 ;;
        --backends) backends="${2:?a list}"; shift 2 ;;
        --no-report) report=0; shift ;;
        -*) die "unknown option $1" ;;
        *) profile="$1"; shift ;;
    esac
done
package="${package:-$OUT_DIR/$profile/package}"
[[ -f "$(engine_library "$package")" ]] || die "no engine in $package; run scripts/package.sh $profile first"
work="$(mktemp -d "$TMPDIR/library-bench.XXXXXX")"
trap 'rm -rf "$work"' EXIT
cflags=()
grep -q zebkit_library_answer "$package/include/zebkit/library.h" && cflags+=(-DBENCH_HOST)
cc -std=c11 -O2 -Wall -Werror ${cflags[@]+"${cflags[@]}"} -I"$package/include" "$ZEBKIT_ROOT/tests/bench/library/bench.c" \
    -L"$package" -l:libzebkit.so.1 -Wl,-rpath,"$package" -o "$work/bench" || die "the benchmark did not build"
bundles=()
[[ -f "$CACHE_DIR/js-libraries/charts/index.mjs" ]] && bundles=("$CACHE_DIR/js-libraries/markdown" "$CACHE_DIR/js-libraries/charts")

# run <backend> <profile dir> <out>: one process; its JSON line to <out>.
run() {
    limited 180 nice "$work/bench" "$ZEBKIT_ROOT/tests/bench/library" ${bundles[@]+"${bundles[@]}"} \
        --options "{\"backend\": \"$1\"}" --headless --no-sandbox --user-data-dir="$2" 2> "$work/err" \
        | tail -1 >> "$3"
    local status="${PIPESTATUS[0]}"
    (( status == 0 )) || cat "$work/err" >&2
    return "$status"
}

measured=()
for backend in $backends; do
    for (( i = 0; i < runs; ++i )); do
        if ! run "$backend" "$work/profile-$backend-$i" "$work/$backend.jsonl"; then
            if (( i == 0 )) && [[ "$backend" != isolate ]]; then
                why="$(grep -m1 'bench:' "$work/err" | sed 's/^bench: //')"
                log "backend $backend not measured: ${why:-it failed}"
                printf '%s\n' "${why:-it failed}" > "$work/$backend.skipped"
                continue 2
            fi
            die "$backend: run $i failed"
        fi
        # The same profile again: what the first left on disk.
        run "$backend" "$work/profile-$backend-$i" "$work/$backend-disk.jsonl" || die "$backend: run $i (again) failed"
    done
    measured+=("$backend")
done
(( ${#measured[@]} )) || die "no backend was measured"

engine_line="$(report_engine "$package" "$profile")"
python3 -I - "$work" "$runs" "$engine_line" "$report" "$ZEBKIT_ROOT/docs/reports/library-bench.md" \
    $backends <<'PY'
import json, os, statistics, sys
work, runs, engine, report, path, backends = sys.argv[1], int(sys.argv[2]), sys.argv[3], sys.argv[4] == "1", sys.argv[5], sys.argv[6:]

def medians(name):
    rows = [json.loads(line) for line in open(name) if line.strip()] if os.path.exists(name) else []
    keys = {key for row in rows for key in row}
    return {key: statistics.median(row[key] for row in rows if key in row) for key in keys}

results, skipped = {}, {}
for backend in backends:
    if os.path.exists(os.path.join(work, backend + ".skipped")):
        skipped[backend] = open(os.path.join(work, backend + ".skipped")).read().strip()
        continue
    results[backend] = (medians(os.path.join(work, backend + ".jsonl")), medians(os.path.join(work, backend + "-disk.jsonl")))

for backend, (cold, disk) in results.items():
    print("%s: %d runs, medians (and with the disk cache):" % (backend, runs))
    for key in sorted(cold):
        print("  %-22s %10.2f %10.2f" % (key, cold[key], disk.get(key, float("nan"))))

# Rows: (title, key, which run, target per backend).
ROWS = [
    ("Load to ready: an empty library (ms)", "echoLoadMs", 0, {}),
    ("Load to ready: markdown-it, first in the process (ms)", "markdownLoadMs", 0, {"isolate": "15", "page": "150"}),
    ("Load to ready: markdown-it, again in the process (ms)", "markdownLoad2Ms", 0, {}),
    ("Load to ready: markdown-it, a new process with the disk cache (ms)", "markdownLoadMs", 1, {}),
    ("Load to ready: ECharts, first in the process (ms)", "chartsLoadMs", 0, {"isolate": "60", "page": "250"}),
    ("Load to ready: ECharts, again in the process (ms)", "chartsLoad2Ms", 0, {}),
    ("Load to ready: ECharts, a new process with the disk cache (ms)", "chartsLoadMs", 1, {}),
    ("ECharts' first render: first load / again / disk cache (ms)", ("chartsFirstRenderMs", "chartsFirstRender2Ms", "chartsFirstRenderMs"), (0, 0, 1), {}),
    ("ECharts' next renders, each (ms)", "chartsRenderMs", 0, {}),
    ("ECharts' first render after 3 s idle / the next (ms)", ("chartsRenderAfterIdleMs", "chartsRenderNextMs"), (0, 0), {}),
    ("Call, 100 B JSON each way: median (µs)", "callMedianUs", 0, {"isolate": "50", "page": "500"}),
    ("Call, 100 B JSON each way: p99 (µs)", "callP99Us", 0, {}),
    ("1 MiB part there and back (ms)", "partMiBMs", 0, {"isolate": "1", "page": "3"}),
    ("host.call() answered at once: median (µs)", "hostCallMedianUs", 0, {}),
    ("Memory: an empty instance (MiB)", "emptyInstanceMiB", 0, {"isolate": "3", "page": "35"}),
    ("Memory: an empty instance, with the processes it made (MiB, a renderer's PSS)", "emptyInstanceAllMiB", 0, {"page": "35"}),
    ("Memory: ECharts after 100 renders (MiB)", "chartsAfter100MiB", 0, {"isolate": "30", "page": "60"}),
    ("Memory: ECharts after 100 renders, with the processes it made (MiB)", "chartsAfter100AllMiB", 0, {"page": "60"}),
    ("Memory: ECharts after 100 renders and 3 s idle (MiB)", "chartsIdleMiB", 0, {"isolate": "30", "page": "60"}),
    ("40 ECharts renders at once: 1 instance (ms, wall)", "charts40On1Ms", 0, {}),
    ("40 ECharts renders at once: 4 instances (ms, wall)", "charts40On4Ms", 0, {}),
]

def cell(backend, key, which):
    if isinstance(key, tuple):
        values = [cell(backend, k, w) for k, w in zip(key, which)]
        return "" if not any(values) else " / ".join(v or "-" for v in values)
    value = results[backend][which].get(key)
    return "" if value is None else "%.2f" % value if value < 100 else "%.1f" % value

columns = list(results)
lines = ["# JavaScript libraries: what a library costs", "", engine + ".", "",
         "`scripts/library-bench.sh`, %d fresh processes per backend, each followed by another on the same profile "
         "(\"disk cache\": what the first left there); medians. tests/bench/library/bench.c, driven by the program's "
         "own loop; the bundles of examples/js-libraries (markdown-it 14.1.0, ECharts 5.6.0, minified). "
         "docs/JS_LIBRARIES.md says what each measure is." % runs, "",
         "| Measure | " + " | ".join("target (%s)" % b for b in columns) + " | " + " | ".join(columns) + " |",
         "|---|" + "---|" * (2 * len(columns))]
for title, key, which, targets in ROWS:
    cells = [cell(b, key, which) for b in columns]
    if not any(cells):
        continue
    lines.append("| %s | %s | %s |" % (title, " | ".join(targets.get(b, "") for b in columns), " | ".join(cells)))
for backend, why in skipped.items():
    lines += ["", "Backend %s: not measured (%s)." % (backend, why)]
text = "\n".join(lines) + "\n"
if report:
    open(path, "w").write(text)
    print("wrote %s" % path)
PY
