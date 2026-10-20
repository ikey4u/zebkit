#!/usr/bin/env bash
# Configure and build one profile into upstream/chromium/src/out/<profile>.
#
#   scripts/build.sh <profile> [--configure-only] [targets...]
#
# The arguments are profiles/_common.gn followed by profiles/<profile>.gn.
# The default target is the engine: libzebkit.so.1 and zebkit-helper.

source "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

profile="${1:-}"
require_profile "$profile"
shift

configure_only=0
if [[ "${1:-}" == --configure-only ]]; then
    configure_only=1
    shift
fi
targets=("${@:-zebkit/engine:engine}")

[[ -f "$GCLIENT_DIR/.zebkit-stamp" ]] || die "no complete upstream tree; run scripts/fetch-upstream.sh first"

build_dir="$SRC_DIR/out/$profile"
mkdir -p "$build_dir"

# How many links run at once is not set here: with ThinLTO Chromium works
# it out from the machine's memory (build/toolchain/concurrent_links.gni).
args="$(cat "$PROFILES_DIR/_common.gn" "$PROFILES_DIR/$profile.gn")"
if [[ ! -f "$build_dir/args.gn" || "$(cat "$build_dir/args.gn")" != "$args" ]]; then
    printf '%s\n' "$args" > "$build_dir/args.gn"
    log "configuring $profile"
    (cd "$SRC_DIR" && gn gen "out/$profile")
fi
(( configure_only )) && exit 0

# Under load the compiler now and then crashes, or reports nonsense errors,
# on a file it compiles fine the next time. A failure is retried; one that
# repeats on the same step is a real error.
failed_before=
for attempt in 1 2 3; do
    log "building ${targets[*]} for $profile"
    if (cd "$SRC_DIR" && autoninja -C "out/$profile" -j "${ZEBKIT_JOBS:-20}" "${targets[@]}" 2>&1 \
            | tee "$build_dir/zebkit-build.log"); then
        exit 0
    fi
    failed="$(grep -ao '^FAILED: [^ ]* "[^"]*"' "$build_dir/siso_output" 2>/dev/null | cut -d' ' -f3 | sort)"
    [[ -n "$failed" && "$failed" != "$failed_before" ]] || exit 1
    failed_before="$failed"
    log "failed on $(echo $failed) (attempt $attempt); building again"
done
exit 1
