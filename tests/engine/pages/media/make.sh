#!/usr/bin/env bash
# The media of tests/engine/media.c, made with FFmpeg's command line (any
# recent one with libx264, libvpx and libopus); the files are in the
# repository, so the tests do not need it. 8 seconds of a test pattern
# and a tone, small, a keyframe every second:
#
#   clip.mp4         H.264 (with B-frames) and AAC, for <video src>
#   clip.webm        VP9 and Opus, for <video src>
#   clip-video.webm  its video alone, appended through Media Source
#                    Extensions after the DASH stream's (a codec change)
#   tone.webm        Opus alone, for <audio src>
#   dash/            the MP4 as a DASH stream: an init segment and 1 s
#                    segments for video and for audio, which mse.html
#                    appends through Media Source Extensions
#   cenc/            the same video encrypted (CENC, Clear Key), which a
#                    capture refuses
set -euo pipefail
cd "$(dirname "$0")"
src=(-f lavfi -i "testsrc2=size=320x180:rate=24:duration=8"
     -f lavfi -i "sine=frequency=440:sample_rate=48000:duration=8")
ffmpeg -y -v error "${src[@]}" -c:v libx264 -preset veryfast -profile:v high -bf 2 \
    -g 24 -keyint_min 24 -sc_threshold 0 -b:v 120k -pix_fmt yuv420p \
    -c:a aac -b:a 48k -ac 2 -movflags +faststart clip.mp4
ffmpeg -y -v error "${src[@]}" -c:v libvpx-vp9 -b:v 120k -g 24 -keyint_min 24 -deadline realtime \
    -cpu-used 8 -c:a libopus -b:a 48k -ac 2 clip.webm
ffmpeg -y -v error -i clip.webm -map 0:v -c copy clip-video.webm
ffmpeg -y -v error -f lavfi -i "sine=frequency=330:sample_rate=48000:duration=8" \
    -c:a libopus -b:a 32k tone.webm
rm -rf dash cenc
mkdir -p dash cenc
ffmpeg -y -v error -i clip.mp4 -map 0:v -map 0:a -c copy -f dash -seg_duration 1 \
    -use_template 1 -use_timeline 0 -init_seg_name 'init-$RepresentationID$.m4s' \
    -media_seg_name 'chunk-$RepresentationID$-$Number%03d$.m4s' dash/stream.mpd
key=00112233445566778899aabbccddeeff kid=0123456789abcdef0123456789abcdef
ffmpeg -y -v error -i clip.mp4 -map 0:v -c copy -encryption_scheme cenc-aes-ctr \
    -encryption_key "$key" -encryption_kid "$kid" \
    -movflags frag_keyframe+empty_moov+default_base_moof cenc/video.mp4
