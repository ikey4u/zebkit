#!/usr/bin/env bash
# Run zebkit_mobile_test, libzebkit without the engine at work
# (docs/MOBILE.md), from a profile's package: on the device adb sees for
# slim-android (a phone, or an emulator for target_cpu = "x64"), here for
# mobile-linux.
#
#   scripts/mobile-test.sh <slim-android | mobile-linux>

source "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

profile="${1:-mobile-linux}"
package="$OUT_DIR/$profile/package"
[[ -f "$package/zebkit_mobile_test" ]] \
    || die "no $package/zebkit_mobile_test: scripts/build.sh $profile zebkit/engine zebkit/engine:zebkit_mobile_test, then scripts/package-mobile.sh $profile"

case "$profile" in
    *android*)
        command -v adb > /dev/null || die "no adb on PATH"
        dir=/data/local/tmp/zebkit
        adb shell "rm -rf $dir && mkdir -p $dir"
        adb push "$package/libzebkit.so" "$package/zebkit_mobile_test" "$package/icudtl.dat" "$dir/" > /dev/null
        adb shell "cd $dir && chmod 755 zebkit_mobile_test && LD_LIBRARY_PATH=$dir ./zebkit_mobile_test $dir/icudtl.dat"
        ;;
    *)
        LD_LIBRARY_PATH="$package" "$package/zebkit_mobile_test" "$package/icudtl.dat"
        ;;
esac
