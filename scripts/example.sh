#!/usr/bin/env bash
# Build one of the applications in examples/ against a zebkit in dist/ and
# run it.
#
#   scripts/example.sh linux-basic [URL]          # examples/window.c: a small browser window
#   scripts/example.sh macos-basic [URL]          # examples/window.c on macOS
#   scripts/example.sh ffmpeg [IN.mp3 [OUT.wav]]  # examples/ffmpeg.cpp: decode with the engine's FFmpeg
#
# On Linux it uses dist/release, which scripts/dist.sh release makes (it is
# built first if it is not there); EXAMPLE_DIST=debug uses dist/debug. The
# executable goes to dist/example/ on Linux. The macOS C embedding demo goes into
# dist/example/Zebkit Embedded Browser.app with a signed framework copy.
# EXAMPLE_BUILD_ONLY=1 builds without launching. The embedding demo prefers the
# native package in dist/<kind>, then the native/cross-built profile's
# package (docs/MACOS.md); ZEBKIT_DIR overrides all of these.

source "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

example="${1:-}"
[[ $# -gt 0 ]] && shift
case "$example" in
    linux-basic|macos-basic|ffmpeg) ;;
    *) die "usage: $(basename "$0") linux-basic [URL] | macos-basic [URL] | ffmpeg [IN.mp3 [OUT.wav]]" ;;
esac
case "$example-$ZEBKIT_HOST" in
    linux-basic-linux|macos-basic-macos|ffmpeg-*) ;;
    *) die "the example '$example' is not for this system ($ZEBKIT_HOST)" ;;
esac

kind="${EXAMPLE_DIST:-release}"
[[ "$kind" == release || "$kind" == debug ]] || die "EXAMPLE_DIST is release or debug"
# The engine: dist/<kind>, which scripts/dist.sh makes on either platform,
# or a native/cross-built macOS profile's package, or ZEBKIT_DIR.
if [[ "$ZEBKIT_HOST" == macos ]]; then
    dist="${ZEBKIT_DIR:-$ZEBKIT_ROOT/dist/$kind}"
    if [[ -z "${ZEBKIT_DIR:-}" && ! -e "$dist/Zebkit.framework" ]]; then
        native_profile=slim-macos-native
        [[ "$kind" == debug ]] && native_profile=slim-macos-native-dev
        dist="$OUT_DIR/$native_profile/package"
        [[ -e "$dist/Zebkit.framework" ]] || dist="$OUT_DIR/slim-macos/package"
    fi
    [[ -e "$dist/Zebkit.framework" ]] || die "no Zebkit.framework in $dist: run scripts/dist.sh $kind on this Mac, or set ZEBKIT_DIR to a package"
else
    dist="$ZEBKIT_ROOT/dist/$kind"
    [[ -e "$dist/libzebkit.so.1" ]] || "$ZEBKIT_ROOT/scripts/dist.sh" "$kind"
fi

out="$ZEBKIT_ROOT/dist/example"
binary="$out/$example"
mkdir -p "$out"
if [[ "$ZEBKIT_HOST" == macos ]]; then
    link=(-F"$dist" -framework Zebkit -Wl,-rpath,"$dist")
else
    link=(-L"$dist" -l:libzebkit.so.1 -Wl,-rpath,"\$ORIGIN/../$kind")
fi

# Keep the sandboxed helper and resources in a standard application bundle.
# This also avoids the helper startup crashes observed with the released
# framework linked from outside a bare executable's bundle.
if [[ "$example" == macos-basic ]]; then
    app="$out/Zebkit Embedded Browser.app"
    contents="$app/Contents"
    framework="$contents/Frameworks/Zebkit.framework"
    mkdir -p "$contents/MacOS" "$framework"
    rsync -a --delete "$dist/Zebkit.framework/" "$framework/"
    python3 - "$contents/Info.plist" <<'PY'
import plistlib
import sys

with open(sys.argv[1], "wb") as output:
    plistlib.dump({
        "CFBundleExecutable": "Zebkit Embedded Browser",
        "CFBundleIdentifier": "dev.zebkit.browser.embedded",
        "CFBundleName": "Zebkit Embedded Browser",
        "CFBundleDisplayName": "Zebkit Embedded Browser",
        "CFBundlePackageType": "APPL",
        "CFBundleInfoDictionaryVersion": "6.0",
        "NSHighResolutionCapable": True,
        "NSSupportsAutomaticGraphicsSwitching": True,
    }, output)
PY
    binary="$contents/MacOS/Zebkit Embedded Browser"
    link=(-F"$contents/Frameworks" -framework Zebkit -Wl,-rpath,@executable_path/../Frameworks)
fi

case "$example" in
    linux-basic|macos-basic)
        log "building examples/window.c against $dist"
        cc -std=c11 -O1 -Wall -Wextra -o "$binary" "$ZEBKIT_ROOT/examples/window.c" -I"$dist/include" "${link[@]}"
        ;;
    ffmpeg)
        [[ -f "$dist/include/zebkit/vendor/ffmpeg.h" ]] || die "$dist does not expose FFmpeg"
        log "building examples/ffmpeg.cpp against $dist"
        c++ -std=c++17 -O1 -Wall -o "$binary" "$ZEBKIT_ROOT/examples/ffmpeg.cpp" -I"$dist/include" "${link[@]}"
        # With nothing named, the sample beside the example.
        (( $# )) || set -- "$ZEBKIT_ROOT/examples/sample.mp3"
        (( $# > 1 )) || set -- "$1" "$out/$(basename "${1%.*}").wav"
        ;;
esac

if [[ "$example" == macos-basic ]]; then
    # The cross-built archive carries linker signatures, without resource
    # seals. Sign the copied bundles from the inside out using the Mac's
    # codesign; leave the source engine package alone.
    codesign --force --sign - "$framework/Versions/A/Helpers/Zebkit Helper.app"
    codesign --force --sign - "$framework"
    codesign --force --sign - "$app"
    codesign --verify --deep --strict "$app"
    log "application: $app"
fi
[[ "${EXAMPLE_BUILD_ONLY:-0}" == 1 ]] && exit 0

# Chromium's sandbox needs unprivileged user namespaces; where the system
# gives none, the example runs without it.
if [[ "$example" != ffmpeg && "$ZEBKIT_HOST" == linux ]] && ! unshare --user true 2> /dev/null; then
    log "no user namespaces here: running with --no-sandbox"
    set -- --no-sandbox "$@"
fi
log "running $binary $*"
exec "$binary" "$@"
