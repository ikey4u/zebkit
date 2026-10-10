# GStreamer: the media framework behind <video> and <audio>.
#
# Built from the monorepo as one static library, libgstreamer-full, holding
# the core, the helper libraries WebKit uses and a fixed list of plugins
# registered at start-up. There is no plugin directory and no registry on
# disk. Everything not named below is off, so no bundled subproject is
# downloaded; FFmpeg and GLib come from deps/.
#
# What the plugin list gives:
#   containers  MP4/MOV/M4A (isomp4), WebM/Matroska, WAV, MP3/AAC/FLAC streams
#   decoding    FFmpeg (libav): H.264, VP8, VP9, AAC, MP3, Opus, Vorbis, FLAC
#   audio out   ALSA, the one sound interface every Linux system has
#   also        WebVTT subtitle encoding and the null video sink WebKit uses
# Debug logging is compiled out; it is a large share of GStreamer's size.

DEP_VERSION=1.28.8
DEP_SOURCES=(
    "https://gitlab.freedesktop.org/gstreamer/gstreamer/-/archive/1.28.8/gstreamer-1.28.8.tar.gz fd146dc03ea55ccfe78c1a91d35b71f20089209e173163f98237aad9f5ec7540"
)

dep_build() {
    # libgstreamer-full registers every plugin that is enabled, so the lists
    # below are the complete set.
    local base=(app playback typefind audioconvert audioresample volume videoconvertscale alsa)
    local good=(isomp4 matroska audioparsers id3demux wavparse audiofx interleave autodetect)
    local bad=(videoparsers subenc debugutils)
    local args=()
    local name
    for name in "${base[@]}"; do args+=("-Dgst-plugins-base:$name=enabled"); done
    for name in "${good[@]}"; do args+=("-Dgst-plugins-good:$name=enabled"); done
    for name in "${bad[@]}"; do args+=("-Dgst-plugins-bad:$name=enabled"); done

    dep_meson --auto-features=disabled --wrap-mode=nodownload \
        -Dgst-full=enabled -Dgst-full-target-type=static_library \
        -Dgst-full-libraries=gstreamer-app-1.0,gstreamer-audio-1.0,gstreamer-video-1.0,gstreamer-pbutils-1.0,gstreamer-tag-1.0,gstreamer-fft-1.0,gstreamer-allocators-1.0 \
        -Dbase=enabled -Dgood=enabled -Dbad=enabled -Dlibav=enabled \
        -Dugly=disabled -Ddevtools=disabled -Dges=disabled -Drtsp_server=disabled \
        -Dpython=disabled -Dtls=disabled -Dlibnice=disabled -Dgst-examples=disabled \
        -Dtools=disabled -Dtests=disabled -Dexamples=disabled -Dbenchmarks=disabled \
        -Dintrospection=disabled -Dnls=disabled -Dorc=disabled -Dwebrtc=disabled \
        -Dgstreamer:gst_debug=false -Dgstreamer:registry=false -Dgstreamer:check=disabled \
        -Dgstreamer:coretracers=disabled -Dgstreamer:ptp-helper=disabled \
        "${args[@]}" || return

    # libgstreamer-full's pkg-config file names every plugin as a requirement,
    # but the plugin .pc files live outside the search path, and it asks for
    # libatomic, which the toolchain's runtime already covers. WebKit only
    # needs to find the library itself; profiles/_common.cmake adds the
    # plugin archives to the link.
    local pc="$DEPS_PREFIX/lib/pkgconfig/gstreamer-full-1.0.pc"
    sed -i -e '/^Requires\(\.private\)\?:/d' -e 's/ -latomic//' "$pc" || return
    echo 'Requires: glib-2.0, gobject-2.0, gmodule-no-export-2.0, gio-2.0' >> "$pc"
}
