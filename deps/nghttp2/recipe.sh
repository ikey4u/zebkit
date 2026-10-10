# nghttp2: HTTP/2 framing for libsoup. Library only.

DEP_VERSION=1.64.0
DEP_SOURCES=(
    "https://github.com/nghttp2/nghttp2/releases/download/v1.64.0/nghttp2-1.64.0.tar.xz 88bb94c9e4fd1c499967f83dece36a78122af7d5fb40da2019c56b9ccc6eb9dd"
)

dep_build() {
    dep_autotools --enable-lib-only --disable-doc
}
