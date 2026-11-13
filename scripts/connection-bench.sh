#!/usr/bin/env bash
# What zebkit's connection and cache reuse saves (docs/CONNECTION_REUSE.md,
# step 0): the cases A to I (and X, P1, S) of that document, offline, on this
# machine. A local HTTPS server (tests/bench/connections/server.mjs) counts TCP
# connections, TLS handshakes and requests, behind a proxy that adds a round
# trip delay; the Python binding drives the engine (client.py), each case in
# engine processes of its own; names are mapped with --host-resolver-rules and
# the certificate trusted by its key (--ignore-certificate-errors-spki-list).
#
#   scripts/connection-bench.sh [--profile PROFILE] [--package DIR] [--runs N] [--rtt MS]
#                               [--idle] [--idle-runs N] [--long] [--spare] [--preconnect-setting] [--partitioned]
#                               [--only REGEX] [--switch SWITCH]... [--label TEXT]
#                               [--compare RESULT.json]... [--no-report]
#
# --idle adds F and G (30 s and 5 min idle; --idle-runs cycles, default 3,
# about 6 minutes each, both protocols at once), --long P90 (90 s), --spare
# the spare renderer's S (on and off, with its memory), --preconnect-setting
# D-pre (D with the startup setting preconnect), --partitioned runs
# with Chrome's partitioning of the cache and connections by top-level site.
# The result is out/blink/connection-bench/<label or "result">.json; the
# report docs/reports/connection-bench.md and .json, with lib.sh's
# report_engine line, unless --no-report; --compare adds other results' times
# as columns. Needs Python 3, Node.js (mise.toml's pin), openssl and the
# package (scripts/package.sh). Run it on a quiet machine.

source "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

profile=slim
package=
runs=10
rtt=40
idle=0
idle_runs=3
long=0
spare=0
preconnect_setting=0
report=1
only=
label=
switches=()
compare=()
while (( $# )); do
    case "$1" in
        --profile) profile="$2"; shift ;;
        --package) package="$2"; shift ;;
        --runs) runs="$2"; shift ;;
        --rtt) rtt="$2"; shift ;;
        --idle) idle=1 ;;
        --idle-runs) idle_runs="$2"; shift ;;
        --long) long=1 ;;
        --spare) spare=1 ;;
        --preconnect-setting) preconnect_setting=1 ;;
        --partitioned) switches+=("--enable-features=SplitCacheByNetworkIsolationKey,PartitionConnectionsByNetworkIsolationKey") ;;
        --only) only="$2"; shift ;;
        --switch) switches+=("$2"); shift ;;
        --label) label="$2"; shift ;;
        --compare) compare+=("$2"); shift ;;
        --no-report) report=0 ;;
        *) die "usage: $(basename "$0") [--profile PROFILE] [--package DIR] [--runs N] [--rtt MS] [--idle] [--idle-runs N] [--long] [--spare] [--preconnect-setting] [--partitioned] [--only REGEX] [--switch SWITCH]... [--label TEXT] [--compare RESULT.json]... [--no-report]" ;;
    esac
    shift
done

package="${package:-$OUT_DIR/$profile/package}"
[[ -f "$package/NOTICE" || -f "$package/libzebkit.so.1" || -d "$package/zebkit.framework" ]] \
    || die "no package in $package: scripts/package.sh $profile"
package="$(cd "$package" && pwd)"
work="$OUT_DIR/connection-bench"
bench="$ZEBKIT_ROOT/tests/bench/connections"
mkdir -p "$work"

# One key, a certificate for each name: HTTP/2 cannot pool the hosts on one
# connection, and the engine trusts the key, not a CA.
certs="$work/certs"
rm -rf "$certs"
mkdir -p "$certs"
openssl genpkey -algorithm EC -pkeyopt ec_paramgen_curve:P-256 -out "$certs/key.pem" 2> /dev/null
for host in a.bench.test b.bench.test c.other.test cdn.shared.test; do
    openssl req -x509 -key "$certs/key.pem" -days 2 -subj "/CN=$host" -addext "subjectAltName=DNS:$host" \
        -out "$certs/$host.pem" 2> /dev/null
done
spki="$(openssl pkey -in "$certs/key.pem" -pubout -outform der | openssl dgst -sha256 -binary | base64)"

"${node_cmd[@]}" "$bench/server.mjs" "$certs" "$rtt" > "$work/server.json" 2> "$work/server.log" &
server=$!
trap 'kill $server 2> /dev/null || true' EXIT
for _ in $(seq 100); do
    [[ -s "$work/server.json" ]] && break
    sleep 0.1
done
[[ -s "$work/server.json" ]] || die "the server did not start: $work/server.log"
read -r control h2 h1 < <(python3 -I -c 'import json, sys; d = json.load(open(sys.argv[1])); print(d["control"], d["h2"], d["h1"])' "$work/server.json")

options=(--control "$control" --h2 "$h2" --h1 "$h1" --spki "$spki" --work "$work/run" --runs "$runs"
         --idle-runs "$idle_runs" --label "$label")
(( idle )) && options+=(--idle)
(( long )) && options+=(--long)
(( spare )) && options+=(--spare)
(( preconnect_setting )) && options+=(--preconnect-setting)
[[ -n "$only" ]] && options+=(--only "$only")
for switch in "${switches[@]}"; do options+=("--switch=$switch"); done

result="$work/${label:-result}.json"
log "measuring ($runs runs, $rtt ms a round trip) into $result"
ZEBKIT_DIR="$package" python3 -I "$bench/client.py" run "${options[@]}" > "$result.tmp"
mv "$result.tmp" "$result"
rm -rf "$work/run"

engine="$(report_engine "$package" "$profile")"
python3 -I "$bench/report.py" "$engine" "$rtt" "$runs" "$result" "${compare[@]}" > "$work/report.md"
cat "$work/report.md"
if (( report )); then
    cp "$work/report.md" "$ZEBKIT_ROOT/docs/reports/connection-bench.md"
    python3 -I -c 'import json, sys; r = json.load(open(sys.argv[1])); r["engine"] = sys.argv[2]; json.dump(r, open(sys.argv[3], "w"), indent=1)' \
        "$result" "$engine" "$ZEBKIT_ROOT/docs/reports/connection-bench.json"
fi
