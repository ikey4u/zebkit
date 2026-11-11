#!/usr/bin/env bash
# The checks a release is published after ("mise run check"), and the quick
# ones that need no build ("mise run check:static").
#
#   scripts/check.sh [--profile PROFILE]   everything, on out/blink/<profile>/package (slim)
#   scripts/check.sh --static              only: bash -n, Python syntax, check-bindings.py,
#                                          check-view-guards.py
#
# Everything is: the static checks; engine-test.sh's downloads (--fetch), then
# the suite in strict mode (the suite also runs check-surfaces.py and
# check-bindings.py); glibc-test.sh; web-bench.sh --check.
#
# Strict: a test, or a part of one, that was not run fails the run, and so does
# a test that exits 0 after printing FAIL lines, except the skips named below
# (expected_skips), which no build machine can avoid. ENGINE_TEST_STRICT=0
# lets every skip pass; such a pass is recorded as not strict, which
# release.sh --publish does not accept.
#
# A full pass is recorded in out/blink/check/passed-<key>, the key being these
# sources (lib.sh's source_tree_key); the file also holds the profile, whether
# the run was strict and the tested package's stamp
# (out/blink/<profile>/package.stamp). scripts/release.sh --publish looks for
# all of it.

source "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

profile=slim
static=0
while (( $# )); do
    case "$1" in
        --static) static=1 ;;
        --profile) profile="${2:?a profile}"; shift ;;
        *) die "usage: $(basename "$0") [--static] [--profile PROFILE]" ;;
    esac
    shift
done

key=
if (( ! static )); then
    key="$(source_tree_key)"
    [[ -n "$key" ]] || die "no key for the sources (git add failed?)"
fi

# The skips strict mode accepts, on Linux (this machine and docker/'s image),
# where only these cannot be avoided. Extended regular expressions on
# engine-test.sh's entries, "<test>: <why>" or "<test>: <part>: <why>"
# (tests/engine/harness.py matches its skips with the same); every other skip
# fails the run. ENGINE_TEST_EXPECTED_SKIPS adds lines of its own.
expected_skips=(
    # No USB, HID or serial device on the machine to choose.
    '^device-chooser: (usb|hid|serial): a device chosen: no device on this machine$'
    # No Chrome to compare with: --fetch installs the CDP clients without the
    # browsers they could download (--ignore-scripts), and nothing installs Chrome.
    '^cdp-clients: no Chrome$'
    '^network-stages: the comparison with Chrome: no Chrome here$'
    # The native hosts' PDF pane: the hosts are built apart (examples/native-host/README.md).
    '^native-host-pdf-[a-z-]+: no [^ ]+: examples/native-host/README\.md builds it$'
    # Parts of macOS and Windows runs (engine-test.sh --prebuilt there, given
    # this list) that a machine without a person at its screen cannot have:
    # the person's clipboard, the Local Network grant, a window's focus.
    '^pdf-in-page: Select All and Copy: the clipboard here is the person.s$'
    '^media-tap: WebRTC loopback: .*macOS Local Network permission'
    '^(window-frame|system-holes)-(mac|win): (window actions|pointer lock): the window cannot have focus'
)

log "shell scripts: bash -n"
shells=()
while IFS= read -r -d '' file; do shells+=("$file"); done \
    < <(find "$ZEBKIT_ROOT/scripts" "$ZEBKIT_ROOT/docker" -maxdepth 1 -type f -name '*.sh' -print0)
for file in "${shells[@]}"; do bash -n "$file" || die "${file#"$ZEBKIT_ROOT"/}: syntax error"; done
sh -n "$ZEBKIT_ROOT/scripts/zebkit-sandbox-install" || die "scripts/zebkit-sandbox-install: syntax error"

log "Python: syntax"
# Parsed, not compiled to files: nothing is written beside the sources.
find "$ZEBKIT_ROOT/scripts" "$ZEBKIT_ROOT/tests" "$ZEBKIT_ROOT/bindings/python" -name '*.py' -not -path '*/node_modules/*' -print0 \
    | xargs -0 python3 -I -c '
import ast, sys
bad = 0
for name in sys.argv[1:]:
    try:
        ast.parse(open(name, "rb").read(), name)
    except SyntaxError as error:
        print(error, file=sys.stderr)
        bad = 1
sys.exit(bad)' || die "Python syntax errors"

log "every C function in every binding"
python3 -I "$ZEBKIT_ROOT/scripts/check-bindings.py" || die "a binding misses functions of the C API"
python3 -I "$ZEBKIT_ROOT/scripts/check-view-guards.py" "$ZEBKIT_ROOT" || die "a view function of the C API uses its view unchecked"

if (( static )); then
    log "static checks passed"
    exit 0
fi

strict="${ENGINE_TEST_STRICT:-1}"
[[ "$strict" == 0 ]] || strict=1
expected="$(printf '%s\n' "${expected_skips[@]}" ${ENGINE_TEST_EXPECTED_SKIPS:+"$ENGINE_TEST_EXPECTED_SKIPS"})"
package_stamp="$OUT_DIR/$profile/package.stamp"
[[ -s "$package_stamp" ]] || die "no $package_stamp: run scripts/package.sh $profile first"
tested="$(cat "$package_stamp")"
"$ZEBKIT_ROOT/scripts/engine-test.sh" "$profile" --fetch
ENGINE_TEST_STRICT="$strict" ENGINE_TEST_EXPECTED_SKIPS="$expected" "$ZEBKIT_ROOT/scripts/engine-test.sh" "$profile"
"$ZEBKIT_ROOT/scripts/glibc-test.sh" "$profile"
"$ZEBKIT_ROOT/scripts/web-bench.sh" --check --profile "$profile"

[[ "$(source_tree_key)" == "$key" ]] || die "the sources changed while they were checked; run the checks again"
[[ "$(cat "$package_stamp" 2> /dev/null || true)" == "$tested" ]] \
    || die "out/blink/$profile/package changed while it was checked; run the checks again"
mkdir -p "$OUT_DIR/check"
{
    echo "profile $profile"
    echo "strict $strict"
    echo "package $tested"
    echo "date $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "commit $(git -C "$ZEBKIT_ROOT" rev-parse HEAD)"
} > "$OUT_DIR/check/passed-$key"
log "all checks passed$( (( strict )) || echo ", not strict (ENGINE_TEST_STRICT=0)"); recorded for these sources: out/blink/check/passed-$key"
