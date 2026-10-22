#!/usr/bin/env bash
# Build one of the applications in examples/ against a zebkit in dist/ and
# run it.
#
#   scripts/example.sh linux-basic [URL]          # examples/window.c: a small browser window
#   scripts/example.sh macos-basic [URL]          # the same program on macOS
#   scripts/example.sh ffmpeg [IN.mp3 [OUT.wav]]  # examples/ffmpeg.cpp: decode with the engine's FFmpeg
#
# On Linux it uses dist/release, which scripts/dist.sh release makes (it is
# built first if it is not there); EXAMPLE_DIST=debug uses dist/debug. The
# executable goes to dist/example/ and finds the engine through a relative
# run path, the way a shipped application finds the one beside it. On a
# Mac it uses the macOS package built on Linux (docs/MACOS.md), in
# out/blink/slim-macos/package or ZEBKIT_DIR.

source "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

example="${1:-}"
[[ $# -gt 0 ]] && shift
case "$example" in
    linux-basic|macos-basic|ffmpeg) ;;
    *) die "usage: $(basename "$0") linux-basic [URL] | macos-basic [URL] | ffmpeg [IN.mp3 [OUT.wav]]" ;;
esac
case "$example-$ZEBKIT_HOST" in
    linux-basic-linux|macos-basic-macos|ffmpeg-*) ;;
    *) die "the example '$example' is not for this system ($ZEBKIT_HOST)" ;;
esac

kind="${EXAMPLE_DIST:-release}"
[[ "$kind" == release || "$kind" == debug ]] || die "EXAMPLE_DIST is release or debug"
# The engine: dist/<kind>, which scripts/dist.sh makes on Linux. On a Mac,
# the macOS package built on Linux (docs/MACOS.md), copied to the same
# place, or ZEBKIT_DIR.
if [[ "$ZEBKIT_HOST" == macos ]]; then
    dist="${ZEBKIT_DIR:-$OUT_DIR/slim-macos/package}"
    [[ -e "$dist/Zebkit.framework" ]] || die "no Zebkit.framework in $dist: build it on Linux (scripts/build.sh slim-macos, scripts/package.sh slim-macos) and copy the package here, or set ZEBKIT_DIR"
else
    dist="$ZEBKIT_ROOT/dist/$kind"
    [[ -e "$dist/libzebkit.so.1" ]] || "$ZEBKIT_ROOT/scripts/dist.sh" "$kind"
fi

out="$ZEBKIT_ROOT/dist/example"
binary="$out/$example"
mkdir -p "$out"
if [[ "$ZEBKIT_HOST" == macos ]]; then
    link=(-F"$dist" -framework Zebkit -Wl,-rpath,"$dist")
else
    link=(-L"$dist" -l:libzebkit.so.1 -Wl,-rpath,"\$ORIGIN/../$kind")
fi

case "$example" in
    linux-basic|macos-basic)
        log "building examples/window.c against $dist"
        cc -std=c11 -O1 -Wall -Wextra -o "$binary" "$ZEBKIT_ROOT/examples/window.c" -I"$dist/include" "${link[@]}"
        ;;
    ffmpeg)
        [[ -f "$dist/include/zebkit/vendor/ffmpeg.h" ]] || die "$dist does not expose FFmpeg"
        log "building examples/ffmpeg.cpp against $dist"
        c++ -std=c++17 -O1 -Wall -o "$binary" "$ZEBKIT_ROOT/examples/ffmpeg.cpp" -I"$dist/include" "${link[@]}"
        # With nothing named, the sample the media test plays.
        (( $# )) || set -- "$ZEBKIT_ROOT/tests/media/sample.mp3"
        (( $# > 1 )) || set -- "$1" "$out/$(basename "${1%.*}").wav"
        ;;
esac

# Chromium's sandbox needs unprivileged user namespaces; where the system
# gives none, the example runs without it.
if [[ "$example" != ffmpeg && "$ZEBKIT_HOST" == linux ]] && ! unshare --user true 2> /dev/null; then
    log "no user namespaces here: running with --no-sandbox"
    set -- --no-sandbox "$@"
fi
log "running $binary $*"
exec "$binary" "$@"
