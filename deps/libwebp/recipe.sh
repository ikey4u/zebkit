# libwebp: WebP decoding and encoding (canvas.toBlob), plus the demuxer
# for animated images.

DEP_VERSION=1.5.0
DEP_SOURCES=(
    "https://storage.googleapis.com/downloads.webmproject.org/releases/webp/libwebp-1.5.0.tar.gz 7d6fab70cf844bf6769077bd5d7a74893f8ffd4dfb42861745750c63c2a5c92c"
)

dep_build() {
    local off=(ANIM_UTILS CWEBP DWEBP GIF2WEBP IMG2WEBP VWEBP WEBPINFO WEBPMUX EXTRAS)
    local args=()
    for tool in "${off[@]}"; do
        args+=("-DWEBP_BUILD_$tool=OFF")
    done
    dep_cmake "${args[@]}"
}
