#!/usr/bin/env bash
# Check that an application can use the libraries inside the engine
# (docs/archive/webkit/VENDOR.md): FFmpeg, as zebkit::vendor::ffmpeg.
#
#   scripts/vendor.sh <profile>
#
# Builds tests/vendor/ against the profile's package, with the system's
# compiler and nothing but the engine to link against (libzebkit on Linux,
# Zebkit.framework on macOS), and runs them.

source "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

profile="${1:-}"
require_profile "$profile"
stage_profile "$profile"
"$ZEBKIT_ROOT/scripts/package.sh" "$profile" > /dev/null 2>&1 || die "packaging '$profile' failed"
package="$build_dir/package"
[[ -f "$package/include/zebkit/vendor/ffmpeg.h" ]] || die "profile '$profile' does not expose FFmpeg"

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
failures=0
check() { # check <ok?> <description>
    if [[ "$1" == yes ]]; then echo "  PASS  $2"; else echo "  FAIL  $2"; failures=$(( failures + 1 )); fi
}

# What differs between the systems: where the engine is and what it
# exports, how a program is linked against it, and what a program needs.
if [[ "$ZEBKIT_HOST" == macos ]]; then
    engine="$package/Zebkit.framework/Versions/A/Frameworks/WebKit.framework/Versions/A/WebKit"
    engine_name=Zebkit.framework
    exported="$(nm -gU "$engine" | awk '{ print $3 }' | sed 's/^_//')"
    link=(-I"$package/include" -F"$package" -framework Zebkit -Wl,-rpath,"$package"
          -Wl,-dyld_env,DYLD_FRAMEWORK_PATH="$package/Zebkit.framework/Versions/A/Frameworks")
    needed_by() { otool -L "$1" | awk 'NR > 1 { print $1 }' | paste -sd' ' -; }
else
    engine="$package/libzebkit.so.1"
    engine_name=libzebkit.so.1
    exported="$(nm -D --defined-only "$engine" | awk '{ print $3 }')"
    link=(-I"$package/include" -L"$package" -l:libzebkit.so.1 -Wl,-rpath,"$package" -Wl,--no-undefined)
    needed_by() { objdump -p "$1" | awk '/NEEDED/ { print $2 }' | paste -sd' ' -; }
fi
count="$(grep -c '^zebkit_vendor_ffmpeg_' <<< "$exported" || true)"
check "$( (( count == $(wc -l < "$OUT_DIR/deps/vendor/ffmpeg.functions") )) && echo yes || echo no)" "the engine exports all $count FFmpeg entry points"
check "$([[ -z "$(grep -E '^(av|swr|sws)[a-z0-9]*_' <<< "$exported" || true)" ]] && echo yes || echo no)" "and none of FFmpeg's own names"

log "C++, through zebkit::vendor::ffmpeg"
if c++ -std=c++17 -O1 -Wall -o "$work/ffmpeg" "$ZEBKIT_ROOT/tests/vendor/ffmpeg.cpp" "${link[@]}" 2> "$work/build.log"; then
    result="$("$work/ffmpeg" "$ZEBKIT_ROOT/tests/media/sample.mp3" 2>&1 || true)"
    echo "  $result"
    value() { tr ' ' '\n' <<< "$result" | sed -n "s/^$1=//p"; }
    check "$([[ "$(value decoder)" == mp3* && "$(value samples)" -gt 10000 ]] && echo yes || echo no)" "an MP3 file is decoded"
    check "$([[ -n "$(value samples)" && "$(value converted)" == "$(value samples)" ]] && echo yes || echo no)" "and converted to 16-bit samples"
    check "$([[ "$(value h264)" == 1 && "$(value aac_encoder)" == 1 ]] && echo yes || echo no)" "the H.264 decoder and the AAC encoder are there"
    needed="$(needed_by "$work/ffmpeg")"
    check "$([[ "$needed" == *"$engine_name"* && "$needed" != *libav* && "$needed" != *libsw* ]] && echo yes || echo no)" "the program needs the engine and no FFmpeg library ($needed)"
else
    cat "$work/build.log" >&2
    check no "the C++ test builds"
fi

log "files, scaling and encoding"
if c++ -std=c++17 -O1 -Wall -o "$work/ffmpeg-files" "$ZEBKIT_ROOT/tests/vendor/ffmpeg-files.cpp" "${link[@]}" 2> "$work/build.log"; then
    result="$("$work/ffmpeg-files" "$ZEBKIT_ROOT/tests/media/sample-h264-aac.mp4" "$work/tone.flac" 2>&1 || true)"
    sed 's/^/  /' <<< "$result"
    check "$([[ "$result" == *"read: format=mov"*"video=h264"*"audio=aac frames=5"* ]] && echo yes || echo no)" "an MP4 file is opened and its video decoded"
    check "$([[ "$result" =~ scaled=64x36\ sum=[1-9] ]] && echo yes || echo no)" "a frame is scaled and converted to RGB"
    check "$([[ "$result" == *"write: format=flac encoder=flac"* && "$(head -c 4 "$work/tone.flac" 2>/dev/null)" == fLaC ]] && echo yes || echo no)" "audio is encoded and written to a FLAC file"
    if command -v ffprobe > /dev/null; then
        duration="$(ffprobe -v error -show_entries format=duration -of csv=p=0 "$work/tone.flac" 2>/dev/null || true)"
        check "$([[ "$duration" == 1.0* ]] && echo yes || echo no)" "which the system's ffprobe reads as one second ($duration)"
    fi
    check "$([[ "$result" == *"encoders: aac=1 mjpeg=1 h264=0"* ]] && echo yes || echo no)" "the encoders are FFmpeg's own: AAC and Motion JPEG yes, H.264 no"
else
    cat "$work/build.log" >&2
    check no "the file test builds"
fi

log "C"
if cc -O1 -Wall -o "$work/ffmpeg-c" "$ZEBKIT_ROOT/tests/vendor/ffmpeg.c" "${link[@]}" 2> "$work/build.log"; then
    result="$("$work/ffmpeg-c" 2>&1 || true)"
    echo "  $result"
    check "$([[ "$result" == *"vp9=vp9"* && "$result" == *"log2(1024)=10"* ]] && echo yes || echo no)" "the C API works"
else
    cat "$work/build.log" >&2
    check no "the C test builds"
fi

(( failures == 0 )) || die "$failures vendor check(s) failed for profile '$profile'"
log "vendored libraries OK"
