#!/usr/bin/env bash
# The release image's entry (docker/Dockerfile): build zebkit's release for
# Linux, macOS and Windows, check it, and publish it as the GitHub release
# v<VERSION>.
#
#   zebkit-publish               # build, check, publish
#   zebkit-publish --no-publish  # build and check only
#
# The release's order (docs/BUILD.md, "Releases"): build, measure the reports
# on that build, commit, check, push, publish. A first run builds and records
# the archives' checksums in engines.txt; while docs/reports has reports
# measured before (scripts/check-reports.py), it stops there: measure them on
# this build, commit them with engines.txt and the release notes, push, and
# run again. That run builds nothing new (the build is up to date), checks,
# and publishes what was built (release.sh --publish-only), with GH_TOKEN. A
# run whose archives or checks are not those of the committed sources builds
# and checks again instead. The tags are pushed with gh's credentials when git
# cannot reach origin here (no ssh client in the image; release.sh).
#
# Limits: the SDKs are laid out by scripts/zsdk-create.sh into .cache/sdk
# (or ZEBKIT_SDK_DIR): Windows' is downloaded, which accepts Microsoft's
# licence terms; Apple's needs MacOSX.sdk.tar.xz and version.plist mounted at
# /macos the first time. The checks are scripts/check.sh's, in strict mode: a
# test this image cannot run (no GPU, no person at a screen) fails it rather
# than being skipped unseen; ENGINE_TEST_STRICT=0 accepts the skips, which
# are listed. A Mac or Windows machine is not here: their engines are built
# and packed, and are tested on those machines (docs/MACOS.md, docs/WINDOWS.md).

set -euo pipefail
log() { printf '\033[1m==> %s\033[0m\n' "$*"; }
die() { printf 'error: %s\n' "$*" >&2; exit 1; }

publish=1
[[ "${1:-}" == --no-publish ]] && publish=0
[[ -f VERSION && -d scripts ]] || die "run in the zebkit repository: -v \"$PWD:$PWD\" -w \"$PWD\""
mise trust -q mise.toml
mise install -q

# The SDKs, once: Apple's from /macos, Microsoft's downloaded.
sdk="${ZEBKIT_SDK_DIR:-.cache/sdk}"
if [[ ! -d "$sdk/macos/xcode" || ! -f "$sdk/windows/win_toolchain.json" ]]; then
    scripts/fetch-upstream.sh
    if [[ ! -f "$sdk/macos/sdk.tar.xz" ]]; then
        [[ -f /macos/MacOSX.sdk.tar.xz && -f /macos/version.plist ]] \
            || die "mount MacOSX.sdk.tar.xz and version.plist at /macos (scripts/macos-toolchain.sh says how to make them)"
        mkdir -p "$sdk/macos"
        cp /macos/MacOSX.sdk.tar.xz "$sdk/macos/sdk.tar.xz"
        cp /macos/version.plist "$sdk/macos/version.plist"
    fi
    ZSDK_LINK=1 scripts/zsdk-create.sh all
fi

# A run after the commit of what a first run built and checked: publish it,
# when nothing changed since. That is: a clean tree, every archive engines.txt
# names for this version in dist/ with those bytes, and the checks passed on
# these sources (scripts/check.sh's record). Otherwise (sources changed after
# the build, archives left from another) it builds and checks again.
version="$(tr -d '[:space:]' < VERSION)"
built_and_checked() {
    local target sum file count=0 key
    [[ -z "$(git status --porcelain)" ]] || return 1
    while read -r _ target sum; do
        file="dist/zebkit-$version-$target.tar.xz"
        [[ -f "$file" && "$(sha256sum "$file" | cut -d' ' -f1)" == "$sum" ]] || return 1
        count=$(( count + 1 ))
    done < <(grep -E "^$version " bindings/rust/zebkit-rs/engines.txt || true)
    (( count )) || return 1
    key="$(bash -c 'source scripts/lib.sh && source_tree_key')" || return 1
    grep -qx 'strict 1' "out/blink/check/passed-$key" 2> /dev/null
}
if (( publish )) && built_and_checked; then
    [[ -n "${GH_TOKEN:-}" ]] || die "publishing needs GH_TOKEN"
    exec scripts/release.sh --publish-only
fi
log "release build for Linux, macOS and Windows"
mise run r
# The reports are measured on the release build, and are part of the sources
# the checks are recorded for: before the checks.
if ! python3 -I -B scripts/check-reports.py docs/reports profiles \
    "$(sed -n 's/^CHROMIUM_VERSION=//p' upstream.lock)" "$(sed -n 's/^ELECTRON_VERSION=//p' upstream.lock)"; then
    die "built; now measure the reports above on this build (docs/BUILD.md, \"Releases\"), commit them with engines.txt and the release notes, push, and run again"
fi
log "the checks"
mise run check

(( publish )) || { log "built and checked: $(cat VERSION)"; exit 0; }
[[ -n "${GH_TOKEN:-}" ]] || die "publishing needs GH_TOKEN"
if [[ -n "$(git status --porcelain)" ]]; then
    git status --short
    die "commit and push these (the archives' checksums in engines.txt among them) and run again to publish"
fi
scripts/release.sh --publish-only
