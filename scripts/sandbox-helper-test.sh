#!/usr/bin/env bash
# The Linux sandbox's set-user-ID helper (chrome-sandbox), installed from a
# release archive the way docs/SANDBOX.md says, and the engine using it.
#
#   scripts/sandbox-helper-test.sh <zebkit-<version>-x86_64-unknown-linux-gnu.tar.xz | package dir>
#
# As any user it checks the archive's side of it: chrome-sandbox and
# zebkit-sandbox-install are in it, the helper is not set-user-ID as shipped,
# and the installer refuses to run as anyone but root. The rest needs root, and
# nothing here asks for it (no sudo): installing makes a file root's and
# set-user-ID, which only root can, and a user namespace cannot stand in (its
# root is not the system's, so the kernel would not honour the bit). Run as
# root, on a test machine or VM (not the build machine), it does the rest
# itself; by hand, the same steps are:
#
#   1. As root, unpack the archive where only root can write:
#        dir="$(mktemp -d /var/tmp/zebkit-helper.XXXXXX)"; chmod 755 "$dir"
#        tar -xJf zebkit-<version>-x86_64-unknown-linux-gnu.tar.xz -C "$dir"
#        package="$dir/zebkit-<version>-x86_64-unknown-linux-gnu"
#   2. Install the helper, as docs/SANDBOX.md says (there with sudo):
#        "$package/zebkit-sandbox-install"
#      It prints "installed .../chrome-sandbox (root, 4755)";
#      stat -c '%U %a' "$package/chrome-sandbox" says "root 4755".
#   3. As an ordinary user (here ZEBKIT_TEST_USER, default nobody), the engine
#      with user namespaces turned off, so that only the helper can sandbox it:
#        runuser -u nobody -- "$package/zebkit-run" --headless \
#            --disable-namespace-sandbox --user-data-dir="$(mktemp -d)" about:blank
#      It must start and stay (no "no sandbox for the page processes", no
#      "--no-sandbox" on standard error), and its page processes run in a PID
#      namespace of their own, which only the helper gives them there: in
#      /proc/<pid>/status of the renderer (--type=renderer), "NSpid:" has two
#      numbers.
#   4. chmod 755 "$package/chrome-sandbox" (no longer set-user-ID): the same
#      command refuses to start and names zebkit-sandbox-install.
#   5. rm -rf "$dir".
#
# Without root this prints "skip setuid helper: ..." after its own checks and
# exits 0; scripts/engine-test.sh lists the test as skipped (sandbox-helper),
# which scripts/check.sh accepts: one run as root is a step of every release
# (docs/SANDBOX.md, "What has been tested").

source "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

[[ "$ZEBKIT_HOST" == linux ]] || die "the set-user-ID helper is Linux's"
input="$(caller_path "${1:?usage: $(basename "$0") <release .tar.xz or package dir>}")"
user="${ZEBKIT_TEST_USER:-nobody}"
checks=0 failed=0
check() {
    checks=$(( checks + 1 ))
    if [[ "$1" == 0 ]]; then echo "ok   $2"; else echo "FAIL $2${3:+: $3}"; failed=$(( failed + 1 )); fi
}
finish() {
    echo "$(( checks - failed )) of $checks checks passed"
    exit $(( failed > 0 ))
}

# The package: the archive unpacked into a directory of root's (when run as
# root), or one of this user's.
if (( EUID == 0 )); then
    scratch="$(mktemp -d /var/tmp/zebkit-helper.XXXXXX)"
    chmod 755 "$scratch"
else
    scratch="$(mktemp -d "$TMPDIR/sandbox-helper.XXXXXX")"
fi
trap 'rm -rf "$scratch"' EXIT
if [[ -f "$input" ]]; then
    tar -xJf "$input" -C "$scratch" || die "could not unpack $input"
    package="$(find "$scratch" -mindepth 1 -maxdepth 1 -type d | head -1)"
elif [[ -f "$input/libzebkit.so.1" ]]; then
    # A directory is copied only for root, into root's directory.
    package="$input"
    if (( EUID == 0 )); then
        package="$scratch/package"
        cp -a "$input" "$package"
    fi
else
    die "no release archive or package at $input"
fi
[[ -n "$package" ]] || die "nothing unpacked from $input"
helper="$package/chrome-sandbox"
installer="$package/zebkit-sandbox-install"

[[ -f "$helper" && ! -L "$helper" ]] && r=0 || r=1; check $r "the package has chrome-sandbox"
[[ -x "$installer" ]] && r=0 || r=1; check $r "the package has zebkit-sandbox-install"
[[ "$(stat -c %a "$helper" 2> /dev/null)" != 4??? ]] && r=0 || r=1; check $r "chrome-sandbox is not set-user-ID as shipped" "$(stat -c %a "$helper" 2> /dev/null)"

if (( EUID != 0 )); then
    out="$("$installer" 2>&1)" && code=0 || code=$?
    [[ "$code" != 0 && "$out" == *"run as root"* ]] && r=0 || r=1; check $r "the installer refuses a user who is not root" "$code: $out"
    echo "skip setuid helper: needs root, which this does not ask for: run it as root on a test machine (the steps are in its header)"
    finish
fi

# Root from here: the documented install, then the engine as $user.
id "$user" > /dev/null 2>&1 || die "no user $user to run the engine as (ZEBKIT_TEST_USER)"
command -v runuser > /dev/null || die "no runuser (util-linux)"
out="$("$installer" 2>&1)" && code=0 || code=$?
[[ "$code" == 0 && "$out" == *"installed"* ]] && r=0 || r=1; check $r "zebkit-sandbox-install installs the helper" "$code: $out"
[[ "$(stat -c '%U %a' "$helper")" == "root 4755" ]] && r=0 || r=1; check $r "the helper is root's, 4755" "$(stat -c '%U %a' "$helper")"

build_against_package "$package" "$scratch/zebkit-run" "$ZEBKIT_ROOT/tools/zebkit-run/zebkit-run.c"
chmod 755 "$scratch/zebkit-run"
# run_engine <seconds>: the engine as $user, sandboxed only by the helper; sets
# engine_code (empty while it still runs), engine_err and renderer_nspid.
run_engine() {
    local profile err pid child
    profile="$(runuser -u "$user" -- mktemp -d)"
    err="$scratch/engine.err"
    runuser -u "$user" -- "$scratch/zebkit-run" --headless --disable-namespace-sandbox \
        --user-data-dir="$profile" about:blank > /dev/null 2> "$err" &
    pid=$!
    engine_code= renderer_nspid=
    sleep "$1"
    if kill -0 "$pid" 2> /dev/null; then
        for child in $(pgrep -u "$user" -f -- '--type=renderer' || true); do
            renderer_nspid="$(sed -n 's/^NSpid:[[:space:]]*//p' "/proc/$child/status" 2> /dev/null || true)"
            [[ -n "$renderer_nspid" ]] && break
        done
        kill "$pid" 2> /dev/null || true
        wait "$pid" 2> /dev/null || true
    else
        wait "$pid" && engine_code=0 || engine_code=$?
    fi
    engine_err="$(cat "$err")"
    rm -rf "$profile"
}
run_engine 6
[[ -z "$engine_code" ]] && r=0 || r=1; check $r "the engine starts with the helper and no user namespaces" "exit $engine_code: ${engine_err: -600}"
[[ "$engine_err" != *"no sandbox for the page processes"* && "$engine_err" != *"without the sandbox"* ]] && r=0 || r=1
check $r "and says nothing of running without a sandbox" "${engine_err: -600}"
[[ "$(wc -w <<< "$renderer_nspid")" -ge 2 ]] && r=0 || r=1; check $r "its renderer is in a PID namespace of its own (the helper's)" "NSpid: ${renderer_nspid:-none}"

chmod 755 "$helper"
run_engine 6
[[ -n "$engine_code" && "$engine_code" != 0 ]] && r=0 || r=1; check $r "with the helper not set-user-ID it refuses to start" "${engine_code:-still running}"
[[ "$engine_err" == *zebkit-sandbox-install* ]] && r=0 || r=1; check $r "and names zebkit-sandbox-install" "${engine_err: -600}"
finish
