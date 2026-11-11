#!/usr/bin/env bash
# Configure and build one profile into upstream/chromium/src/out/<profile>.
#
#   scripts/build.sh <profile> [--configure-only] [targets...]
#
# The arguments are profiles/_common.gn followed by profiles/<profile>.gn,
# and before it the profile it names on a line "# zebkit-base: <profile>"
# (slim-macos is slim, for macOS).
# The default target is the engine: libzebkit.so.1 and zebkit-helper.

source "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

if [[ "$ZEBKIT_HOST" == macos && -z "${DEVELOPER_DIR:-}" && -d /Applications/Xcode.app/Contents/Developer ]]; then
    export DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer
fi

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
# One build at a time on the machine, at the lowest priority (lib.sh's heavy),
# with ZEBKIT_JOBS jobs: 12 unless set.
jobs="$ZEBKIT_JOBS"
if [[ "$ZEBKIT_HOST" == macos && -z "${ZEBKIT_JOBS_SET:-}" ]]; then
    # Two GiB per compiler keeps the local build from exhausting memory.
    memory_jobs=$(( $(sysctl -n hw.memsize) / 2147483648 ))
    jobs="$(sysctl -n hw.ncpu)"
    if (( jobs > memory_jobs )); then jobs="$memory_jobs"; fi
    if (( jobs < 1 )); then jobs=1; fi
fi

# How many links run at once is not set here: with ThinLTO Chromium works
# it out from the machine's memory (build/toolchain/concurrent_links.gni).
args="$(profile_args "$profile")"
if [[ ! -f "$build_dir/build.ninja" || ! -f "$build_dir/args.gn" || "$(cat "$build_dir/args.gn")" != "$args" ]]; then
    printf '%s\n' "$args" > "$build_dir/args.gn"
    log "configuring $profile"
    # On macOS configure only the engine and its dependencies.
    roots=()
    if grep -q '^target_os = "mac"' <<< "$args"; then roots=(--root-pattern=//:zebkit); fi
    (cd "$SRC_DIR" && gn gen "${roots[@]}" "out/$profile")
fi
(( configure_only )) && exit 0

# Under load the compiler now and then crashes, or reports nonsense errors,
# on a file it compiles fine the next time. A failure is retried; one that
# repeats on the same step is a real error.
failed_before=
for attempt in 1 2 3; do
    log "building ${targets[*]} for $profile"
    if (cd "$SRC_DIR" && heavy autoninja -C "out/$profile" -j "$jobs" "${targets[@]}" 2>&1 \
            | tee "$build_dir/zebkit-build.log"); then
        exit 0
    fi
    failed="$(grep -ao '^FAILED: [^ ]* "[^"]*"' "$build_dir/siso_output" 2>/dev/null | cut -d' ' -f3 | sort)"
    [[ -n "$failed" && "$failed" != "$failed_before" ]] || exit 1
    failed_before="$failed"
    log "failed on $(echo $failed) (attempt $attempt); building again"
done
exit 1
