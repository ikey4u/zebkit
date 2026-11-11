#!/usr/bin/env bash
# Set up the Linux machine to build zebkit for macOS (profiles/slim-macos.gn),
# as Chromium's own builders compile for macOS on Linux.
#
#   scripts/macos-toolchain.sh <MacOSX.sdk.tar.xz> <Xcode version.plist>
#
# What only a Mac has is Apple's SDK, the headers and library stubs the
# engine compiles and links against; Chromium wants it inside a copy of
# Xcode, of which it reads the version. Make the two files on a Mac:
#
#   cd "$(xcrun --show-sdk-path)/.." &&
#       tar -cf - --exclude 'MacOSX.sdk/usr/share/man' MacOSX.sdk | xz -T0 > MacOSX.sdk.tar.xz
#   cp "$(xcode-select -p)/../version.plist" .
#
# The rest comes from where Chromium's DEPS says, at the versions it pins,
# for a checkout that builds macOS on Linux: the Mach interface generator
# for Linux, clang's runtime library for macOS, and ANGLE's Metal shaders.
# All of it goes under $SDK_DIR/macos (scripts/lib.sh) and is linked into the tree;
# scripts/fetch-upstream.sh links it again after a sync.

source "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

sdk_archive="$(caller_path "${1:-}")"
version_plist="$(caller_path "${2:-}")"
[[ -f "$sdk_archive" && -f "$version_plist" ]] \
    || die "usage: $(basename "$0") <MacOSX.sdk.tar.xz> <Xcode version.plist>"
[[ -f "$SRC_DIR/DEPS" ]] || die "no upstream tree; run scripts/fetch-upstream.sh first"

macos="$SDK_DIR/macos"
xcode="$macos/xcode"
mkdir -p "$macos"

# The SDK, under the name Chromium asks for (profiles/slim-macos.gn sets
# mac_sdk_official_version to the SDK's version).
sdk_version="$(tar -xJOf "$sdk_archive" MacOSX.sdk/SDKSettings.json \
    | python3 -I -c 'import json, sys; print(json.load(sys.stdin)["Version"])')"
sdks_in_xcode=Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs
# An SDK of that version, from the same Xcode, laid out already is kept: a
# build of another checkout may be using it. A new one is laid out beside it
# and then takes its place.
if [[ -f "$xcode/$sdks_in_xcode/MacOSX$sdk_version.sdk/SDKSettings.json" && -L "$xcode/$sdks_in_xcode/MacOSX.sdk" ]] \
    && cmp -s "$version_plist" "$xcode/Contents/version.plist"; then
    log "the macOS $sdk_version SDK is laid out already: $xcode"
else
    new="$macos/xcode.new"
    sdks="$new/$sdks_in_xcode"
    rm -rf "$new"
    mkdir -p "$sdks" "$new/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/bin"
    cp "$version_plist" "$new/Contents/version.plist"
    log "unpacking the macOS $sdk_version SDK"
    tar -xJf "$sdk_archive" -C "$sdks" --warning=no-unknown-keyword
    mv "$sdks/MacOSX.sdk" "$sdks/MacOSX$sdk_version.sdk"
    # Also under the name Xcode gives the current one, which Chromium's scripts
    # for FFmpeg look for.
    ln -s "MacOSX$sdk_version.sdk" "$sdks/MacOSX.sdk"
    rm -rf "$xcode"
    mv "$new" "$xcode"
fi
grep -q "mac_sdk_official_version = \"$sdk_version\"" "$PROFILES_DIR/slim-macos.gn" \
    || log "note: profiles/slim-macos.gn names another SDK version than $sdk_version"

# What DEPS pins, read from DEPS itself.
pins="$(cd "$SRC_DIR" && python3 -I -c '
g = {"Var": lambda name: "{%s}" % name, "Str": str}
exec(open("DEPS").read(), g)
deps = g["deps"]
for name, path in (("mig", "src/third_party/mig/bin"), ("angle-metal", "src/ui/gl/resources/angle-metal")):
    package = deps[path]["packages"][0]
    print("cipd", name, package["package"].replace("${{platform}}", "linux-amd64"), package["version"])
for item in deps["src/third_party/llvm-build/Release+Asserts"]["objects"]:
    if "mac-runtime-library" in item["object_name"]:
        print("gcs", item["object_name"], item["sha256sum"])
')"

ensure="$macos/cipd.ensure"
: > "$ensure"
while read -r kind a b c; do
    case "$kind" in
        cipd) printf '@Subdir %s\n%s %s\n' "$a" "$b" "$c" >> "$ensure" ;;
        gcs)
            # Fetched again only when DEPS names another object; unpacked beside,
            # then renamed.
            [[ "$(cat "$macos/runtime.object" 2> /dev/null)" == "$a $b" ]] && continue
            log "fetching $a"
            curl -sSfLo "$macos/runtime.tar.xz" "https://commondatastorage.googleapis.com/chromium-browser-clang/$a"
            [[ "$(sha256 "$macos/runtime.tar.xz" | cut -d' ' -f1)" == "$b" ]] || die "$a: wrong SHA-256"
            rm -rf "$macos/runtime.new"
            mkdir -p "$macos/runtime.new"
            tar -xJf "$macos/runtime.tar.xz" -C "$macos/runtime.new"
            rm -rf "$macos/runtime"
            mv "$macos/runtime.new" "$macos/runtime"
            echo "$a $b" > "$macos/runtime.object"
            rm "$macos/runtime.tar.xz"
            ;;
    esac
done <<< "$pins"
log "fetching the Mach interface generator and ANGLE's Metal shaders"
cipd ensure -root "$macos/cipd" -ensure-file "$ensure" >/dev/null

[[ -n "${ZEBKIT_SDK_NO_LINK:-}" ]] || "$ZEBKIT_ROOT/scripts/fetch-upstream.sh"
log "ready to build profiles/slim-macos.gn"
