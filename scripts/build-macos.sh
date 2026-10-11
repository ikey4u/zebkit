#!/usr/bin/env bash
# Build one profile of the macOS engine into out/<profile>/. This is what
# scripts/build.sh runs on a Mac.
#
#   scripts/build.sh <profile> [xcodebuild args...]
#
# The engine is WebKit's Mac port and its build is Xcode's: the workspace in
# the upstream tree, in release mode, with the profile
# (profiles/macos/<profile>.xcconfig) laid over the settings of every target.
# The frameworks and helper processes end up in out/<profile>/Release, and
# everything Xcode caches stays inside out/<profile> as well.

source "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

profile="${1:-}"
require_profile "$profile"
shift

[[ -d "$SRC_DIR/.git" ]] || die "no upstream tree; run scripts/fetch-upstream.sh first"
xcodebuild -version >/dev/null 2>&1 || die "Xcode is not installed, or xcode-select points somewhere else"
# WebKit's graphics layer compiles Metal shaders, and the compiler for them
# is a separate download.
xcodebuild -showComponent MetalToolchain 2>/dev/null | grep -q 'Status: installed' \
    || die "the Metal Toolchain is missing; run: xcodebuild -downloadComponent MetalToolchain"

build_dir="$OUT_DIR/$profile"
mkdir -p "$build_dir"

# The build's own tools (Perl, Python, Ruby) are the system's, whatever the
# shell that started this has on its path.
export PATH=/usr/bin:/bin:/usr/sbin:/sbin

log "building profile '$profile' (log: $build_dir/build.log)"
status=0
xcodebuild -workspace "$SRC_DIR/WebKit.xcworkspace" -scheme "${ZEBKIT_XCODE_SCHEME:-Everything up to MiniBrowser}" \
    -configuration Release -destination "platform=macOS,arch=$(uname -m)" \
    -xcconfig "$PROFILES_DIR/$profile.xcconfig" \
    -derivedDataPath "$build_dir/DerivedData" \
    -UseSanitizedBuildSystemEnvironment=YES -ShowBuildOperationDuration=YES \
    SYMROOT="$build_dir" OBJROOT="$build_dir" SDKROOT=macosx MODULE_CACHE_DIR="$build_dir/ModuleCache" \
    "$@" > "$build_dir/build.log" 2>&1 || status=$?

if (( status != 0 )); then
    grep -E '(^|: )error:' "$build_dir/build.log" | sed "s#$ZEBKIT_ROOT/##g" | sort | uniq -c | sort -rn | head -40 >&2 || true
    die "build of profile '$profile' failed; the errors above are from $build_dir/build.log"
fi
log "built: $build_dir/Release"
