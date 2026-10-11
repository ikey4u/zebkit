#!/usr/bin/env bash
# Build one of the applications in examples/ against a zebkit in dist/ and
# run it.
#
#   scripts/example.sh linux-basic [URL]          # examples/window.c: a small browser window
#   scripts/example.sh ffmpeg [IN.mp3 [OUT.wav]]  # examples/ffmpeg.cpp: decode with the engine's FFmpeg
#
# Uses dist/release, which scripts/dist.sh release makes (it is built first
# if it is not there). EXAMPLE_DIST=debug uses dist/debug. The executable
# goes to dist/example/ and finds the engine through a relative run path,
# the way a shipped application finds the one beside it.

source "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

example="${1:-}"
[[ $# -gt 0 ]] && shift
case "$example" in
    linux-basic|ffmpeg) ;;
    *) die "usage: $(basename "$0") linux-basic [URL] | ffmpeg [IN.mp3 [OUT.wav]]" ;;
esac

kind="${EXAMPLE_DIST:-release}"
case "$kind" in
    release) profile=slim ;;
    debug) profile=slim-dev ;;
    *) die "EXAMPLE_DIST is release or debug" ;;
esac
dist="$ZEBKIT_ROOT/dist/$kind"
[[ -f "$dist/libzebkit.so.1" ]] || "$ZEBKIT_ROOT/scripts/dist.sh" "$kind"

out="$ZEBKIT_ROOT/dist/example"
binary="$out/$example"
mkdir -p "$out"
link=(-L"$dist" -l:libzebkit.so.1 -Wl,-rpath,"\$ORIGIN/../$kind")

case "$example" in
    linux-basic)
        # WebKit's headers are not part of the package yet; they are in the
        # build's stage.
        stage="$OUT_DIR/$profile/stage"
        pc_path="$stage/lib/pkgconfig:$OUT_DIR/deps/prefix/lib/pkgconfig"
        [[ -f "$stage/lib/pkgconfig/wpe-webkit-2.0.pc" ]] || die "no headers in $stage; run scripts/dist.sh $kind"
        log "building examples/window.c against dist/$kind"
        # shellcheck disable=SC2046
        cc -O1 -Wall -Wno-unused-parameter -o "$binary" "$ZEBKIT_ROOT/examples/window.c" \
            $(PKG_CONFIG_PATH="$pc_path" pkg-config --cflags wpe-webkit-2.0) \
            "${link[@]}" -l:libgobject-2.0.so.0 -l:libglib-2.0.so.0
        ;;
    ffmpeg)
        [[ -f "$dist/include/zebkit/vendor/ffmpeg.h" ]] || die "dist/$kind does not expose FFmpeg; run scripts/dist.sh $kind"
        log "building examples/ffmpeg.cpp against dist/$kind"
        c++ -std=c++17 -O1 -Wall -o "$binary" "$ZEBKIT_ROOT/examples/ffmpeg.cpp" -I"$dist/include" "${link[@]}"
        # With nothing named, the sample the media test plays.
        (( $# )) || set -- "$ZEBKIT_ROOT/tests/media/sample.mp3"
        (( $# > 1 )) || set -- "$1" "$out/$(basename "${1%.*}").wav"
        ;;
esac

log "running $binary $*"
exec "$binary" "$@"
