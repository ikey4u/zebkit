#!/usr/bin/env bash
# Set up the Linux machine to build zebkit for iOS (profiles/slim-ios.gn,
# slim-ios-simulator.gn), as scripts/macos-toolchain.sh does for macOS.
#
#   scripts/ios-toolchain.sh <directory>
#
# Chromium compiles and links iOS with its own clang and ld64.lld, on any
# host; what only a Mac has is Apple's iPhone SDKs, and the Swift libraries
# the link searches. Make the directory's files on a Mac (about 40 MB):
#
#   p="$(xcode-select -p)/Platforms"
#   for s in iPhoneOS iPhoneSimulator; do
#       (cd "$p/$s.platform/Developer/SDKs" &&
#           tar -cf - --exclude "$s.sdk/usr/share/man" "$s.sdk") | xz -T0 > "$s.sdk.tar.xz"
#       cp "$p/$s.platform/Info.plist" "$s.platform.Info.plist"
#   done
#   (cd "$(xcode-select -p)/Toolchains/XcodeDefault.xctoolchain/usr/lib" &&
#       tar -cf - --exclude '*.swiftmodule' swift/iphoneos swift/iphonesimulator) | xz -T0 > swift-ios-libs.tar.xz
#   cp "$(xcode-select -p)/../version.plist" .
#
# They are laid out as a copy of Xcode under $SDK_DIR/ios/xcode, and
# scripts/build.sh gives an iOS profile's build on Linux the SDK's paths and
# versions (ios-sdk.gn there): Chromium asks Xcode for them otherwise
# (build/config/apple/sdk_info.py). patches/1091 and 1092 make Chromium's iOS
# build take them so, and build without dSYMs (no dsymutil on Linux).

source "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

[[ "$ZEBKIT_HOST" == linux ]] || die "this sets up iOS builds on Linux; a Mac builds iOS with its Xcode"
from="$(caller_path "${1:-}")"
for f in iPhoneOS.sdk.tar.xz iPhoneSimulator.sdk.tar.xz iPhoneOS.platform.Info.plist \
        iPhoneSimulator.platform.Info.plist swift-ios-libs.tar.xz version.plist; do
    [[ -f "$from/$f" ]] || die "usage: $(basename "$0") <directory with $f and the rest, made on a Mac>"
done

ios="$SDK_DIR/ios"
new="$ios/xcode.new"
developer="$new/Contents/Developer"
rm -rf "$new"
mkdir -p "$developer/Toolchains/XcodeDefault.xctoolchain/usr/lib"
cp "$from/version.plist" "$new/Contents/version.plist"
sdk_version=""
for s in iPhoneOS iPhoneSimulator; do
    sdks="$developer/Platforms/$s.platform/Developer/SDKs"
    mkdir -p "$sdks"
    log "unpacking the $s SDK"
    tar -xJf "$from/$s.sdk.tar.xz" -C "$sdks" --warning=no-unknown-keyword
    version="$(python3 -I -c 'import json, sys; print(json.load(open(sys.argv[1]))["Version"])' "$sdks/$s.sdk/SDKSettings.json")"
    [[ -z "$sdk_version" || "$sdk_version" == "$version" ]] || die "the SDKs' versions differ: $sdk_version, $version"
    sdk_version="$version"
    # Under its versioned name, as in Xcode, and the plain one linking to it.
    mv "$sdks/$s.sdk" "$sdks/$s$version.sdk"
    ln -s "$s$version.sdk" "$sdks/$s.sdk"
    cp "$from/$s.platform.Info.plist" "$developer/Platforms/$s.platform/Info.plist"
done
tar -xJf "$from/swift-ios-libs.tar.xz" -C "$developer/Toolchains/XcodeDefault.xctoolchain/usr/lib" \
    --warning=no-unknown-keyword

# The versions Chromium reads from Xcode otherwise: the SDK's build (in the
# Info.plists it writes) and Xcode's.
plist_value() {
    python3 -I -c 'import plistlib, sys; print(plistlib.load(open(sys.argv[1], "rb"))[sys.argv[2]])' "$1" "$2"
}
sdk_build="$(plist_value "$developer/Platforms/iPhoneOS.platform/Developer/SDKs/iPhoneOS$sdk_version.sdk/System/Library/CoreServices/SystemVersion.plist" ProductBuildVersion)"
xcode_verbatim="$(plist_value "$new/Contents/version.plist" CFBundleShortVersionString)"
xcode_build="$(plist_value "$new/Contents/version.plist" ProductBuildVersion)"
# 27.0 → 2700, 16.4.1 → 1641, as sdk_info.py makes it.
xcode_int="$(python3 -I -c 'import sys; p = (sys.argv[1].split(".") + ["0", "0"])[:3]; print(int(p[0]) * 100 + int(p[1]) * 10 + int(p[2]))' "$xcode_verbatim")"
cat > "$new/versions" <<EOF
sdk_version $sdk_version
sdk_build $sdk_build
xcode_version_verbatim $xcode_verbatim
xcode_version_int $xcode_int
xcode_build $xcode_build
EOF
rm -rf "$ios/xcode"
mv "$new" "$ios/xcode"
log "iOS SDK $sdk_version ($sdk_build), from Xcode $xcode_verbatim ($xcode_build): $ios/xcode"
