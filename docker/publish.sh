#!/usr/bin/env bash
# The release image's entry (docker/Dockerfile): build zebkit's release for
# Linux and macOS, test it, and publish it as the GitHub release v<VERSION>.
#
#   zebkit-publish               # build, test, publish
#   zebkit-publish --no-publish  # build and test only
#
# Publishing needs GH_TOKEN and a commit that names the archives'
# checksums: a first run builds and records them in engines.txt, then
# stops; commit that, and run again to publish.

set -euo pipefail
log() { printf '\033[1m==> %s\033[0m\n' "$*"; }
die() { printf 'error: %s\n' "$*" >&2; exit 1; }

publish=1
[[ "${1:-}" == --no-publish ]] && publish=0
[[ -f VERSION && -d scripts ]] || die "run in the zebkit repository: -v \"$PWD:$PWD\" -w \"$PWD\""
mise trust -q mise.toml
mise install -q

# Apple's SDK, once: the macOS build needs it and only a Mac has it.
if [[ ! -d .cache/macos/xcode ]]; then
    [[ -f /macos/MacOSX.sdk.tar.xz && -f /macos/version.plist ]] \
        || die "mount MacOSX.sdk.tar.xz and version.plist at /macos (scripts/macos-toolchain.sh says how to make them)"
    scripts/fetch-upstream.sh
    scripts/macos-toolchain.sh /macos/MacOSX.sdk.tar.xz /macos/version.plist
fi

log "release build for Linux and macOS"
mise run r
log "the engine's tests"
scripts/engine-test.sh slim
log "the engine on glibc 2.17"
scripts/glibc-test.sh slim

(( publish )) || { log "built and tested: $(cat VERSION)"; exit 0; }
[[ -n "${GH_TOKEN:-}" ]] || die "publishing needs GH_TOKEN"
if [[ -n "$(git status --porcelain)" ]]; then
    git status --short
    die "commit these (the archives' checksums in engines.txt among them) and run again to publish"
fi
mise run r:github
