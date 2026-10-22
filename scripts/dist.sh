#!/usr/bin/env bash
# Build zebkit for the machine this runs on and put what an application
# ships into dist/<kind>/.
#
#   scripts/dist.sh release    # the slim profile, stripped        -> dist/release
#   scripts/dist.sh debug      # slim-dev, with debug information  -> dist/debug
#
# Runs the whole pipeline, each step of which does nothing when it is up to
# date: Chromium's source with the patches, the engine, the package. The
# first build takes hours; later ones minutes.
#
# "debug" is not an unoptimized build: it is slim with debug information
# and without the link-time optimization pass (profiles/slim-dev.gn), which
# is what a debugger and a profiler need and what relinks in minutes.
#
# The macOS engine is not built on a Mac: it is cross-built on Linux
# (docs/MACOS.md).

source "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

kind="${1:-}"
case "$kind" in
    release) profile=slim; strip=1 ;;
    debug) profile=slim-dev; strip=0 ;;
    *) die "usage: $(basename "$0") release|debug" ;;
esac

host="$(uname -s)-$(uname -m)"
case "$host" in
    Linux-x86_64) engine=libzebkit.so.1 ;;
    Darwin-*) die "the macOS engine is built on Linux: scripts/build.sh slim-macos, then scripts/package.sh slim-macos (docs/MACOS.md)" ;;
    *) die "zebkit builds on Linux x86_64 so far; this is $host" ;;
esac

"$ZEBKIT_ROOT/scripts/fetch-upstream.sh"
"$ZEBKIT_ROOT/scripts/build.sh" "$profile"
PACKAGE_STRIP="$strip" "$ZEBKIT_ROOT/scripts/package.sh" "$profile"

dist="$ZEBKIT_ROOT/dist/$kind"
package="$OUT_DIR/$profile/package"
[[ -e "$package/$engine" ]] || die "packaging left no engine in $package"
mkdir -p "$dist"
# Only what the package holds: files of an earlier build must not linger.
find "$dist" -mindepth 1 -delete
cp -a "$package/." "$dist/"
log "$kind build of zebkit for $host: $dist ($(du -sm "$dist" | cut -f1) MiB)"
