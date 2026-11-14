#!/usr/bin/env bash
# The release's iOS package, made on a Mac (Chromium builds iOS only on macOS):
# dist/release-ios, which scripts/release.sh on the Linux machine packs as
# zebkit-<version>-ios.tar.xz beside the other systems' archives. Copy it
# there (dist/release-ios) before running mise run r.
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

[[ "$ZEBKIT_HOST" == macos ]] || die "iOS is built on a Mac: run this there, then copy dist/release-ios to the Linux machine"
command -v xcodebuild > /dev/null || die "no xcodebuild: install Xcode"

ZEBKIT_TARGET_OS=ios "$ZEBKIT_ROOT/scripts/fetch-upstream.sh"
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
[[ "$(/usr/libexec/PlistBuddy -c 'Print :CFBundleShortVersionString' "$plist")" == "$version" ]] \
    || die "$plist is not zebkit $version"
log "iOS package of zebkit $version: $ios ($(du -sm "$ios" | cut -f1) MiB); copy it to dist/release-ios on the Linux machine"
