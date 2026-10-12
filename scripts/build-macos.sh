#!/usr/bin/env bash
# Build one profile of the macOS engine into out/<profile>/. This is what
# scripts/build.sh runs on a Mac.
#
#   scripts/build.sh <profile> [xcodebuild args...]
#
# The engine is WebKit's Mac port and its build is Xcode's: the workspace in
# the upstream tree, in release mode, with the profile's settings
# (profiles/macos/<profile>.xcconfig) laid over those of every target.
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

# The profile's settings go on the command line, one by one, where they
# count for more than what the projects set. Handed over as a file
# (-xcconfig) they do the same for a first build, but Xcode does not look
# at the file again, and a later change to the profile builds nothing.
settings=()
while IFS= read -r line; do
    line="${line%%//*}"
    [[ "$line" =~ ^[[:space:]]*([A-Za-z_][A-Za-z0-9_]*)[[:space:]]*=[[:space:]]*(.*)$ ]] || continue
    value="${BASH_REMATCH[2]%;*}"
    settings+=("${BASH_REMATCH[1]}=${value%"${value##*[![:space:]]}"}")
done < "$PROFILES_DIR/$profile.xcconfig"

# A profile that links what scripts/build-deps.sh builds needs it built,
# and has to link again when it changed.
if [[ "${settings[*]}" == *'$(ZEBKIT_ROOT)/out/deps'* ]]; then
    vendor="$OUT_DIR/deps/vendor/lib/libzebkit-vendor.a"
    [[ -f "$vendor" ]] || die "profile '$profile' links the libraries in out/deps; run scripts/build-deps.sh first"
    framework="$build_dir/Release/WebKit.framework/Versions/A/WebKit"
    if [[ -f "$framework" && "$vendor" -nt "$framework" ]]; then
        rm -f "$framework"
    fi
fi

# Disk. A build tree is about 40 GB. Xcode would add a cache of every
# compilation it ever did, which is as much again for each change of
# settings and buys nothing here, so that is off; and the cache link-time
# optimization keeps is thrown away when the settings change, since what
# is in it then is never used again.
settings+=("COMPILATION_CACHE_ENABLE_CACHING=NO")
# For the profile to name files of this checkout.
settings+=("ZEBKIT_ROOT=$ZEBKIT_ROOT")
if [[ "$(cat "$build_dir/.settings" 2>/dev/null)" != "${settings[*]}" ]]; then
    rm -rf "$build_dir/LTOCache"
    printf '%s\n' "${settings[*]}" > "$build_dir/.settings"
fi

log "building profile '$profile' (log: $build_dir/build.log)"
status=0
xcodebuild -workspace "$SRC_DIR/WebKit.xcworkspace" -scheme "${ZEBKIT_XCODE_SCHEME:-Everything up to MiniBrowser}" \
    -configuration Release -destination "platform=macOS,arch=$(uname -m)" \
    -derivedDataPath "$build_dir/DerivedData" \
    -UseSanitizedBuildSystemEnvironment=YES -ShowBuildOperationDuration=YES \
    SYMROOT="$build_dir" OBJROOT="$build_dir" SDKROOT=macosx MODULE_CACHE_DIR="$build_dir/ModuleCache" \
    ${settings[@]+"${settings[@]}"} "$@" > "$build_dir/build.log" 2>&1 || status=$?

if (( status != 0 )); then
    grep -E '(^|: )error:' "$build_dir/build.log" | sed "s#$ZEBKIT_ROOT/##g" | sort | uniq -c | sort -rn | head -40 >&2 || true
    die "build of profile '$profile' failed; the errors above are from $build_dir/build.log"
fi
log "built: $build_dir/Release"
