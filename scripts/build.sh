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
# //zebkit/VERSION, which fetch-upstream.sh links in too: a tree synced before
# it did has it from here.
[[ -L "$SRC_DIR/zebkit/VERSION" ]] || ln -sfn "$ZEBKIT_ROOT/VERSION" "$SRC_DIR/zebkit/VERSION"

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
# iOS on Linux: the SDK from zsdk (scripts/zsdk.sh), given
# here, since Chromium would ask Xcode for it; and no dSYMs (no dsymutil).
if [[ "$ZEBKIT_HOST" == linux ]] && grep -q '^target_os = "ios"' <<< "$args"; then
    "$ZEBKIT_ROOT/scripts/zsdk.sh" ios
    developer="$IOS_SDK/xcode/Contents/Developer"
    ios_version() { sed -n "s/^$1 //p" "$IOS_SDK/xcode/versions"; }
    if grep -q '^target_environment = "simulator"' <<< "$args"; then
        platform=iPhoneSimulator sdk_name=iphonesimulator
    else
        platform=iPhoneOS sdk_name=iphoneos
    fi
    platform_dir="$developer/Platforms/$platform.platform"
    toolchain="$developer/Toolchains/XcodeDefault.xctoolchain"
    args+="
# The iOS SDK from zsdk (scripts/build.sh)
use_system_xcode = true
enable_dsyms = false
ios_sdk_name = \"$sdk_name\"
ios_sdk_platform = \"$platform\"
ios_sdk_platform_path = \"$platform_dir\"
ios_sdk_path = \"$platform_dir/Developer/SDKs/$platform$(ios_version sdk_version).sdk\"
ios_sdk_version = \"$(ios_version sdk_version)\"
ios_sdk_build = \"$(ios_version sdk_build)\"
ios_toolchains_path = \"$toolchain\"
ios_bin_path = \"$toolchain/usr/bin/\"
xcode_version = \"$(ios_version xcode_version_int)\"
xcode_version_int = $(ios_version xcode_version_int)
xcode_version_verbatim = \"$(ios_version xcode_version_verbatim)\"
xcode_build = \"$(ios_version xcode_build)\""
fi
if [[ ! -f "$build_dir/build.ninja" || ! -f "$build_dir/args.gn" || "$(cat "$build_dir/args.gn")" != "$args" ]]; then
    printf '%s\n' "$args" > "$build_dir/args.gn"
    log "configuring $profile"
    # For macOS and iOS configure only the engine and its dependencies.
    roots=()
    if grep -q '^target_os = "\(mac\|ios\)"' <<< "$args"; then roots=(--root-pattern=//:zebkit); fi
    (cd "$SRC_DIR" && gn gen "${roots[@]}" "out/$profile")
fi
(( configure_only )) && exit 0

# A siso output directory that plain ninja has built in (third_party/ninja,
# not autoninja) has its .ninja_deps: autoninja then takes ninja, refuses
# ("Run gn clean before switching from siso to ninja"), and what ninja
# rebuilt makes siso rebuild much of Chromium after it. Single files are
# built here too (scripts/build.sh <profile> obj/.../file.o), with siso;
# ninja's records go, siso's stay.
if [[ -f "$build_dir/.siso_deps" && -f "$build_dir/.ninja_deps" ]]; then
    log "warning: out/$profile was built with plain ninja over siso: its .ninja_deps goes (build single files with scripts/build.sh <profile> <target>)"
    rm -f "$build_dir/.ninja_deps"
fi

# Under load the compiler now and then crashes (clang's "frontend command
# failed due to signal"), or reports nonsense errors, on a file it compiles
# fine the next time; each run keeps what it compiled. A failure is retried;
# one that repeats on the same step is a real error. Crashes on ever other
# files are retried longer: a full rebuild of Chromium meets several.
failed_before=
for attempt in 1 2 3 4 5 6 7 8; do
    log "building ${targets[*]} for $profile"
    if (cd "$SRC_DIR" && heavy autoninja -C "out/$profile" -j "$jobs" "${targets[@]}" 2>&1 \
            | tee "$build_dir/zebkit-build.log"); then
        exit 0
    fi
    failed="$(grep -ao '^FAILED: [^ ]* "[^"]*"' "$build_dir/siso_output" 2>/dev/null | cut -d' ' -f3 | sort)"
    [[ -n "$failed" && "$failed" != "$failed_before" ]] || exit 1
    # Past three, only the compiler's crashes are tried again.
    if (( attempt >= 3 )) && ! grep -q "frontend command failed due to signal" "$build_dir/zebkit-build.log"; then
        exit 1
    fi
    failed_before="$failed"
    log "failed on $(echo $failed) (attempt $attempt); building again"
done
exit 1
