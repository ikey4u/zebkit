#!/usr/bin/env bash
# Install a built profile into out/<profile>/stage and load tests/smoke/ in a
# headless MiniBrowser, once over HTTP and once over HTTPS. The page exercises
# layout, canvas, WebGL, JS (including code hot enough for the optimizing
# JIT), WebAssembly and Intl, then reports back to the local server; we compare the
# report.

source "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

profile="${1:-}"
require_profile "$profile"

port="${SMOKE_PORT:-8765}"
tls_port=$(( port + 1 ))
expected='layout=321x45&px=255.0.0.255&wasm=42&loop=8999994&intl=1%2C234%2C567.5&wrongTypes=0&webgl=0.255.0.255'

stage_profile "$profile"

# JavaScriptCore and the DOM bindings tell types apart by the address of each
# class's ClassInfo. If the linker ever merges two of them (identical-code
# folding did), type checks silently go wrong, so refuse to go on.
read -r infos shared < <(nm "$build_dir"/lib/libWPEWebKit-*.so.*.*.* 2>/dev/null \
    | awk '$3 ~ /6s_infoE$/ { total++; count[$1]++ } END { for (a in count) if (count[a] > 1) n++; print total + 0, n + 0 }')
if (( infos == 0 )); then
    # The cross toolchain strips the symbol table at link time, so there is
    # nothing to inspect; the JIT-hot type check in the page still runs.
    log "ClassInfo addresses not checked: the engine has no symbol table"
else
    (( shared == 0 )) || die "$shared groups of ClassInfo objects share an address in profile '$profile'; a linker option is merging data"
    log "$infos ClassInfo objects, all distinct"
fi

work="$(mktemp -d)"
pids=()
trap 'kill "${pids[@]}" 2>/dev/null; rm -r "$work"' EXIT

# One plain-HTTP server and one HTTPS server with a throwaway self-signed
# certificate, so the TLS backend gets exercised without network access.
openssl req -x509 -newkey rsa:2048 -nodes -days 1 -subj /CN=127.0.0.1 \
    -keyout "$work/cert.pem" -out "$work/cert.pem" 2>/dev/null
python3 -u "$ZEBKIT_ROOT/tests/serve.py" "$ZEBKIT_ROOT/tests/smoke" "$port" > "$work/http.log" 2>&1 &
pids+=($!)
python3 -u "$ZEBKIT_ROOT/tests/serve.py" "$ZEBKIT_ROOT/tests/smoke" "$tls_port" "$work/cert.pem" > "$work/https.log" 2>&1 &
pids+=($!)
sleep 1

# load_page <url> <server log>: load the page and check what it reported.
# The query string defeats the HTTP cache shared between runs and profiles.
load_page() {
    local url="$1" server_log="$2" got
    log "loading $url"
    "${run_env[@]}" timeout 20 "$build_dir/bin/MiniBrowser" --headless --ignore-tls-errors \
        "$url/index.html?run=$(date +%s%N)" >> "$build_dir/smoke.log" 2>&1 || true
    got="$(grep -oE 'GET /result\?[^ ]+' "$server_log" | head -1 | sed 's|GET /result?||' || true)"
    if [[ "$got" != "$expected" ]]; then
        printf 'expected: %s\n     got: %s\n' "$expected" "${got:-<no report; see $build_dir/smoke.log>}" >&2
        die "smoke test failed for profile '$profile' at $url"
    fi
    log "PASS: $got"
}

: > "$build_dir/smoke.log"
load_page "http://127.0.0.1:$port" "$work/http.log"
load_page "https://127.0.0.1:$tls_port" "$work/https.log"
