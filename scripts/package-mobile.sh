#!/usr/bin/env bash
# Package libzebkit as Android and iOS build it, without the engine
# (docs/MOBILE.md), into out/blink/<profile>/package:
#
#   scripts/package-mobile.sh <slim-android | slim-ios | mobile-linux>
#
#   libzebkit.so                 Android (an APK's lib/<abi>/); mobile-linux
#   zebkit.aar                   Android: dev.zebkit.ZebkitViewer, libzebkit.so of
#                                every slim-android* profile packaged (arm64,
#                                x86-64) and ICU's data (assets/zebkit/icudtl.dat)
#   Zebkit.xcframework/          iOS, on a Mac: also copied to bindings/swift
#   Zebkit.framework/            iOS: the binary, Info.plist, Headers/,
#                                Modules/ (Swift's `import Zebkit`) and
#                                icudtl.dat
#   include/zebkit/              the headers it has, the vendored libraries' too
#   icudtl.dat                   ICU's data, for zebkit_icu_init_from_file()
#   LICENSE, NOTICE, THIRD_PARTY_NOTICES.txt
#
# Run after scripts/build.sh <profile> zebkit/engine. Desktop packages, with
# the engine, are scripts/package.sh's.

source "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

profile="${1:-}"
require_profile "$profile"
args="$(profile_args "$profile")"
grep -q '^zebkit_build_engine = false' <<< "$args" \
    || die "$profile builds the engine: package it with scripts/package.sh"
build_dir="$SRC_DIR/out/$profile"
package="$OUT_DIR/$profile/package"
target_os="$(sed -n 's/^target_os = "\(.*\)"/\1/p' <<< "$args" | tail -1)"
version="$(tr -d '[:space:]' < "$ZEBKIT_ROOT/VERSION")"

rm -rf "$package"
mkdir -p "$package/include/zebkit"

# The headers of what it has, and the vendored libraries'.
headers=(zebkit.h canvas.h text.h pdf.h services.h viewer.h)
[[ -f "$build_dir/obj/zebkit/engine/media_tools/media_tools.o" ]] && headers+=(media_tools.h)
for header in "${headers[@]}"; do
    cp "$ZEBKIT_ROOT/src/engine/include/zebkit/$header" "$package/include/zebkit/"
done
if [[ -d "$build_dir/gen/zebkit/engine/include/zebkit" ]]; then
    cp -R "$build_dir/gen/zebkit/engine/include/zebkit" "$package/include/"
fi

"$ZEBKIT_ROOT/scripts/build-icu-data.sh"
cp "$OUT_DIR/icu/icudtl.dat" "$package/icudtl.dat"

# zebkit.aar: dev.zebkit.ZebkitViewer (bindings/android) with libzebkit.so
# for the profile's processor and ICU's data, for an application's
# dependencies (implementation(files("zebkit.aar"))).
make_aar() {
    local jdk android_jar work other other_cpu abi
    jdk="$SRC_DIR/third_party/jdk/current/bin"
    android_jar="$(ls -d "$SRC_DIR"/third_party/android_sdk/public/platforms/android-*/android.jar | sort -V | tail -1)"
    [[ -x "$jdk/javac" && -f "$android_jar" ]] || die "no JDK or android.jar in the checkout (ZEBKIT_TARGET_OS=android scripts/fetch-upstream.sh)"
    work="$(mktemp -d)"
    mkdir -p "$work/classes" "$work/aar/assets/zebkit"
    "$jdk/javac" --release 11 -cp "$android_jar" -d "$work/classes" \
        $(find "$ZEBKIT_ROOT/bindings/android/src/main/java" -name '*.java')
    "$jdk/jar" cf "$work/aar/classes.jar" -C "$work/classes" .
    cp "$ZEBKIT_ROOT/bindings/android/AndroidManifest.xml" "$work/aar/"
    # This profile's library, and those of the other slim-android* profiles
    # packaged already (arm64 for devices, x86-64 for the emulator).
    for other in "$OUT_DIR"/slim-android*/package/libzebkit.so; do
        [[ -f "$other" ]] || continue
        other_cpu="$(profile_args "$(basename "$(dirname "$(dirname "$other")")")" | sed -n 's/^target_cpu = "\(.*\)"/\1/p' | tail -1)"
        case "$other_cpu" in
            arm64) abi=arm64-v8a ;;
            x64) abi=x86_64 ;;
            *) die "no Android ABI for target_cpu $other_cpu" ;;
        esac
        mkdir -p "$work/aar/jni/$abi"
        cp "$other" "$work/aar/jni/$abi/"
    done
    cp "$package/icudtl.dat" "$work/aar/assets/zebkit/icudtl.dat"
    : > "$work/aar/R.txt"
    rm -f "$package/zebkit.aar"
    (cd "$work/aar" && "$jdk/jar" cfM "$package/zebkit.aar" .)
    log "zebkit.aar: $(cd "$work/aar" && ls jni | paste -sd' ' -)"
    rm -rf "$work"
}

case "$target_os" in
    ios)
        binary="$build_dir/Zebkit"
        [[ -f "$binary" ]] || die "no $binary; build it first: scripts/build.sh $profile zebkit/engine"
        framework="$package/Zebkit.framework"
        mkdir -p "$framework/Headers" "$framework/Modules"
        cp "$binary" "$framework/Zebkit"
        cp -R "$package/include/zebkit/." "$framework/Headers/"
        # The headers include each other as "x.h" and <zebkit/x.h>: both
        # resolve inside the framework (zebkit/ is a link to Headers).
        ln -s . "$framework/Headers/zebkit"
        {
            echo "framework module Zebkit {"
            for header in "${headers[@]}"; do echo "    header \"$header\""; done
            echo "    export *"
            echo "}"
        } > "$framework/Modules/module.modulemap"
        cp "$package/icudtl.dat" "$framework/icudtl.dat"
        cat > "$framework/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleDevelopmentRegion</key><string>en</string>
    <key>CFBundleExecutable</key><string>Zebkit</string>
    <key>CFBundleIdentifier</key><string>dev.zebkit.Zebkit</string>
    <key>CFBundleInfoDictionaryVersion</key><string>6.0</string>
    <key>CFBundleName</key><string>Zebkit</string>
    <key>CFBundlePackageType</key><string>FMWK</string>
    <key>CFBundleShortVersionString</key><string>$version</string>
    <key>CFBundleVersion</key><string>$version</string>
    <key>CFBundleSupportedPlatforms</key><array><string>iPhoneOS</string></array>
    <key>MinimumOSVersion</key><string>15.0</string>
</dict>
</plist>
PLIST
        # Zebkit.xcframework for Swift packages (bindings/swift): on a Mac,
        # from this framework and any other slim-ios* package's (the
        # simulator's) already made.
        if command -v xcodebuild > /dev/null; then
            frameworks=()
            for other in "$OUT_DIR"/slim-ios*/package/Zebkit.framework; do
                frameworks+=(-framework "$other")
            done
            rm -rf "$package/Zebkit.xcframework"
            xcodebuild -create-xcframework "${frameworks[@]}" -output "$package/Zebkit.xcframework" >&2
            rm -rf "$ZEBKIT_ROOT/bindings/swift/Zebkit.xcframework"
            cp -R "$package/Zebkit.xcframework" "$ZEBKIT_ROOT/bindings/swift/"
        else
            log "no xcodebuild: Zebkit.framework only (Zebkit.xcframework is made on a Mac)"
        fi
        ;;
    *)
        library="$build_dir/libzebkit.so"
        [[ -f "$library" ]] || die "no $library; build it first: scripts/build.sh $profile zebkit/engine"
        cp "$library" "$package/libzebkit.so"
        # Without symbols: the build's are kept for crash reports.
        strip="$SRC_DIR/third_party/llvm-build/Release+Asserts/bin/llvm-strip"
        [[ -x "$strip" ]] && "$strip" --strip-unneeded "$package/libzebkit.so"
        [[ -f "$build_dir/zebkit_mobile_test" ]] && cp "$build_dir/zebkit_mobile_test" "$package/"
        if [[ "$target_os" == android ]]; then
            make_aar
        fi
        ;;
esac

cp "$ZEBKIT_ROOT/LICENSE" "$package/LICENSE"
sed -e "s/@VERSION@/$version/g" -e "s/@CHROMIUM@/$CHROMIUM_VERSION/g" \
    "$ZEBKIT_ROOT/scripts/NOTICE.in" > "$package/NOTICE"
(cd "$SRC_DIR" && python3 -I "$ZEBKIT_ROOT/scripts/third-party-notices.py" engine "out/$profile" \
    "${target_os:-linux}" "$package/THIRD_PARTY_NOTICES.txt") >&2 \
    || log "warning: no THIRD_PARTY_NOTICES.txt (scripts/third-party-notices.py failed)"

log "packaged $profile: $package"
du -sh "$package"/* >&2
