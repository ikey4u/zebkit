#!/usr/bin/env bash
# The release's iOS package: dist/release-ios, which scripts/release.sh packs
# as zebkit-<version>-ios.tar.xz beside the other systems' archives. Made on
# the Linux machine with the iPhone SDK from zsdk
# (scripts/zsdk.sh), or on a Mac with Xcode, then copied to the
# Linux machine's dist/release-ios before mise run r.
#
#   scripts/release-ios.sh
#
# What an iOS application takes from it (docs/MOBILE.md, docs/VIEWER.md):
#
#   Package.swift, Sources/, Tests/  the Swift package: ZebkitViewer over
#                                    WKWebView, and libzebkit's C API as Zebkit
#   Zebkit.xcframework/              libzebkit without its engine, for devices
#                                    (arm64) and the simulator (arm64)
#   include/zebkit/                  the C headers
#   icudtl.dat                       ICU's data (also inside each framework)
#   LICENSE, NOTICE, THIRD_PARTY_NOTICES.txt
#
# The version is VERSION's, compiled in: bump first, as for the rest of the
# release.

source "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

if [[ "$ZEBKIT_HOST" == macos ]]; then
    command -v xcodebuild > /dev/null || die "no xcodebuild: install Xcode"
else
    # On Linux, with the iPhone SDK from zsdk.
    "$ZEBKIT_ROOT/scripts/zsdk.sh" ios
fi

if [[ "$ZEBKIT_HOST" == macos ]]; then
    ZEBKIT_TARGET_OS=ios "$ZEBKIT_ROOT/scripts/fetch-upstream.sh"
else
    # The Linux checkout as it is: the engine-less iOS library needs none of
    # what a checkout for iOS adds (iOS's tests and tools, Mac-only packages).
    "$ZEBKIT_ROOT/scripts/fetch-upstream.sh"
fi
for profile in slim-ios-simulator slim-ios; do
    "$ZEBKIT_ROOT/scripts/build.sh" "$profile" zebkit/engine
    # slim-ios last: its package gathers every slim-ios* framework into
    # Zebkit.xcframework, and copies that into bindings/swift.
    "$ZEBKIT_ROOT/scripts/package-mobile.sh" "$profile"
done

package="$OUT_DIR/slim-ios/package"
[[ -d "$package/Zebkit.xcframework" ]] || die "packaging left no Zebkit.xcframework"
ios="$ZEBKIT_ROOT/dist/release-ios"
rm -rf "$ios"
mkdir -p "$ios"
cp -R "$package/Zebkit.xcframework" "$package/include" "$package/icudtl.dat" \
    "$package/LICENSE" "$package/NOTICE" "$package/THIRD_PARTY_NOTICES.txt" "$ios/"
cp "$ZEBKIT_ROOT/bindings/swift/Package.swift" "$ios/"
cp -R "$ZEBKIT_ROOT/bindings/swift/Sources" "$ZEBKIT_ROOT/bindings/swift/Tests" "$ios/"

# The version the release checks against VERSION.
version="$(tr -d '[:space:]' < "$ZEBKIT_ROOT/VERSION")"
plist="$ios/Zebkit.xcframework/ios-arm64/Zebkit.framework/Info.plist"
[[ "$(python3 -I -c 'import plistlib, sys; print(plistlib.load(open(sys.argv[1], "rb"))["CFBundleShortVersionString"])' "$plist")" == "$version" ]] \
    || die "$plist is not zebkit $version"
log "iOS package of zebkit $version: $ios ($(du -sm "$ios" | cut -f1) MiB)"
