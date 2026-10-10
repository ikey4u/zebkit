# FFmpeg: the audio and video decoders behind media playback, reached through
# GStreamer's libav plugin.
#
# Only decoders and their parsers are built: H.264, VP8, VP9, AAC, MP3, Opus,
# Vorbis, FLAC and PCM. Containers are GStreamer's job, and there is no
# network, device or encoding code, and libavfilter is built empty (the
# GStreamer plugin insists on linking it). Assembly stays on: it is what
# makes software decoding fast enough.
#
# H.264 and AAC are covered by patents in some countries; shipping this
# decoder set may need a licence there.

DEP_VERSION=8.1.3
DEP_SOURCES=(
    "https://ffmpeg.org/releases/ffmpeg-8.1.3.tar.xz 7138d28c96d9d3e3af4ee3d8cad72741f8ffb40da90c1112235dea3ecd3178a3"
)

dep_build() {
    "$DEP_SRC/configure" \
        --prefix="$DEPS_PREFIX" --libdir="$DEPS_PREFIX/lib" \
        --cc="$DEP_CC" --cxx="$DEP_CXX" --ar=ar --ranlib=ranlib --nm=nm --strip=strip \
        --extra-cflags="$DEP_CFLAGS" \
        --enable-static --disable-shared --enable-pic \
        --disable-everything --disable-autodetect \
        --disable-programs --disable-doc --disable-debug \
        --disable-network --disable-avdevice \
        --disable-swscale --disable-iconv \
        --enable-decoder=h264,vp8,vp9,aac,aac_latm,mp3,mp3float,opus,vorbis,flac,pcm_s16le,pcm_s16be,pcm_f32le,pcm_u8 \
        --enable-parser=h264,vp8,vp9,aac,aac_latm,mpegaudio,opus,vorbis,flac || return
    # glibc 2.17 still exports sysctl(), so configure enables it, but the
    # toolchain's headers (shared by all glibc versions) no longer declare it.
    sed -i 's/^#define HAVE_SYSCTL 1$/#define HAVE_SYSCTL 0/' config.h || return
    make -j"$DEP_JOBS" || return
    make install
}
