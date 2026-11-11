#!/usr/bin/env bash
# Create, from nothing, the SDKs that build zebkit for macOS and Windows on
# Linux, in one directory every checkout can use.
#
#   zsdk-create.sh [windows|macos|all]      (default: all)
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
# What exists is reused: a macOS SDK of the same version and Xcode is kept, and
# the pieces Chromium's DEPS pins are fetched again only when it pins others,
# each laid out beside the old one and then put in its place. The SDKs stay on
# this machine: do not publish them.
set -euo pipefail

self="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
what="${1:-all}"
case "$what" in windows|macos|all) ;; *) echo "usage: $(basename "$0") [windows|macos|all]" >&2; exit 2 ;; esac

if [[ -z "${ZEBKIT_SDK_DIR:-}" ]]; then
    if [[ "$(basename "$0")" == create.sh ]]; then ZEBKIT_SDK_DIR="$self"; else ZEBKIT_SDK_DIR="$self/../.cache/sdk"; fi
fi
export ZEBKIT_SDK_DIR
mkdir -p "$ZEBKIT_SDK_DIR"

if [[ "$(uname)" == Darwin ]]; then
    [[ "$what" == windows ]] && { echo "error: the Windows SDK is laid out on Linux" >&2; exit 1; }
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

if [[ "$what" != macos ]]; then
    if [[ "${ZSDK_LINK:-}" == 1 ]]; then "$ZEBKIT_ROOT/scripts/windows-toolchain.sh"
    else "$ZEBKIT_ROOT/scripts/windows-toolchain.sh" --no-link; fi
fi
if [[ "$what" != windows ]]; then
    macos="$ZEBKIT_SDK_DIR/macos"
    if [[ ! -f "$macos/sdk.tar.xz" || ! -f "$macos/version.plist" ]]; then
        echo "error: no $macos/sdk.tar.xz and version.plist: run this script on a Mac and copy them here" >&2
        exit 1
    fi
    [[ "${ZSDK_LINK:-}" == 1 ]] || export ZEBKIT_SDK_NO_LINK=1
    "$ZEBKIT_ROOT/scripts/macos-toolchain.sh" "$macos/sdk.tar.xz" "$macos/version.plist"
fi
echo "SDKs ready in $ZEBKIT_SDK_DIR"
