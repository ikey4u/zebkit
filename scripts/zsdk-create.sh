#!/usr/bin/env bash
# Create, from nothing, the SDKs that build zebkit for macOS and Windows on
# Linux, in one directory every checkout can use.
#
#   zsdk-create.sh [windows|macos|macos-intel|all]      (default: all)
#
#   ZEBKIT_SDK_DIR    where they go: this script's directory when it is run as
#                     <dir>/create.sh, otherwise the checkout's .cache/sdk
#   ZEBKIT_ROOT       the zebkit checkout, which has Chromium's DEPS to pin from
#                     (default: this script's checkout)
#   ZSDK_LINK=1       also link them into that checkout's Chromium tree
#
# windows: everything is downloaded, by SHA-256; running this accepts Microsoft's
#   licence terms for Visual Studio and the Windows SDK.
# macos: Apple gives its SDK only with Xcode, so a Mac has to pack it once. On a
#   Mac this script packs it into macos/ and stops; copy that to the Linux machine.
#   On Linux it uses macos/sdk.tar.xz and macos/version.plist, and fetches the rest.
#
# macos-intel: macOS on Intel Macs (profile slim-macos-intel). Apple's SDK and
#   the compiler's runtime for macOS have both processors' libraries, so it is
#   the macos SDK, checked to have x86-64's and linked as macos-intel; made
#   after macos (all does both).
#
# What exists is reused: a macOS SDK of the same version and Xcode is kept, and
# the pieces Chromium's DEPS pins are fetched again only when it pins others,
# each laid out beside the old one and then put in its place. The SDKs stay on
# this machine: do not publish them.
set -euo pipefail

self="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
what="${1:-all}"
case "$what" in windows|macos|macos-intel|all) ;; *) echo "usage: $(basename "$0") [windows|macos|macos-intel|all]" >&2; exit 2 ;; esac

if [[ -z "${ZEBKIT_SDK_DIR:-}" ]]; then
    if [[ "$(basename "$0")" == create.sh ]]; then ZEBKIT_SDK_DIR="$self"; else ZEBKIT_SDK_DIR="$self/../.cache/sdk"; fi
fi
export ZEBKIT_SDK_DIR
mkdir -p "$ZEBKIT_SDK_DIR"

if [[ "$(uname)" == Darwin ]]; then
    [[ "$what" == windows || "$what" == macos-intel ]] && { echo "error: the Windows SDK is laid out on Linux" >&2; exit 1; }
    sdk="$(xcrun --show-sdk-path)"
    mkdir -p "$ZEBKIT_SDK_DIR/macos"
    tar -C "$sdk/.." -cf - --exclude 'MacOSX.sdk/usr/share/man' MacOSX.sdk | xz -T0 > "$ZEBKIT_SDK_DIR/macos/sdk.tar.xz"
    cp "$(xcode-select -p)/../version.plist" "$ZEBKIT_SDK_DIR/macos/version.plist"
    echo "packed $ZEBKIT_SDK_DIR/macos: copy sdk.tar.xz and version.plist to macos/ on the Linux machine and run this there"
    exit 0
fi

if [[ -z "${ZEBKIT_ROOT:-}" ]]; then
    if [[ -f "$self/lib.sh" ]]; then ZEBKIT_ROOT="$(dirname "$self")"; else ZEBKIT_ROOT="${self}"; fi
fi
[[ -f "$ZEBKIT_ROOT/scripts/lib.sh" ]] \
    || { echo "error: no zebkit checkout in $ZEBKIT_ROOT; set ZEBKIT_ROOT" >&2; exit 1; }

if [[ "$what" == windows || "$what" == all ]]; then
    if [[ "${ZSDK_LINK:-}" == 1 ]]; then "$ZEBKIT_ROOT/scripts/windows-toolchain.sh"
    else "$ZEBKIT_ROOT/scripts/windows-toolchain.sh" --no-link; fi
fi
if [[ "$what" == macos || "$what" == all ]]; then
    macos="$ZEBKIT_SDK_DIR/macos"
    if [[ ! -f "$macos/sdk.tar.xz" || ! -f "$macos/version.plist" ]]; then
        echo "error: no $macos/sdk.tar.xz and version.plist: run this script on a Mac and copy them here" >&2
        exit 1
    fi
    [[ "${ZSDK_LINK:-}" == 1 ]] || export ZEBKIT_SDK_NO_LINK=1
    "$ZEBKIT_ROOT/scripts/macos-toolchain.sh" "$macos/sdk.tar.xz" "$macos/version.plist"
fi
if [[ "$what" == macos-intel || "$what" == all ]]; then
    macos="$ZEBKIT_SDK_DIR/macos"
    [[ -d "$macos/xcode" && -d "$macos/runtime" ]] \
        || { echo "error: no macOS SDK laid out in $macos: run this script with macos first" >&2; exit 1; }
    llvm="$ZEBKIT_ROOT/upstream/chromium/src/third_party/llvm-build/Release+Asserts/bin"
    # x86-64's libraries in the SDK (libSystem's stubs) and in the compiler's runtime.
    system_tbd="$macos/xcode/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk/usr/lib/libSystem.B.tbd"
    runtime="$(ls "$macos"/runtime/lib/clang/*/lib/darwin/libclang_rt.osx.a 2> /dev/null | head -1)"
    [[ -f "$system_tbd" ]] && grep -q 'x86_64-macos' "$system_tbd" \
        || { echo "error: the macOS SDK in $macos has no x86-64 libraries" >&2; exit 1; }
    if [[ -x "$llvm/llvm-lipo" ]]; then
        "$llvm/llvm-lipo" -archs "$runtime" | grep -qw x86_64 \
            || { echo "error: $runtime has no x86-64 slice" >&2; exit 1; }
    fi
    if [[ ! -e "$ZEBKIT_SDK_DIR/macos-intel" ]]; then
        ln -s macos "$ZEBKIT_SDK_DIR/macos-intel"
    fi
    [[ "$(readlink -f "$ZEBKIT_SDK_DIR/macos-intel")" == "$(readlink -f "$macos")" ]] \
        || { echo "error: $ZEBKIT_SDK_DIR/macos-intel is not the macos SDK" >&2; exit 1; }
    echo "macOS for Intel: the macos SDK, which has x86-64's libraries: $ZEBKIT_SDK_DIR/macos-intel"
fi
echo "SDKs ready in $ZEBKIT_SDK_DIR"
