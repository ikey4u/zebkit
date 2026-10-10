# PCRE2: regular expressions for GLib (GRegex). Linked into libglib
# statically. Its JIT is off; nothing on the engine's hot paths uses GRegex.

DEP_VERSION=10.45
DEP_SOURCES=(
    "https://github.com/PCRE2Project/pcre2/releases/download/pcre2-10.45/pcre2-10.45.tar.bz2 21547f3516120c75597e5b30a992e27a592a31950b5140e7b8bfde3f192033c4"
)

dep_build() {
    dep_cmake -DPCRE2_BUILD_PCRE2_8=ON -DPCRE2_BUILD_PCRE2_16=OFF -DPCRE2_BUILD_PCRE2_32=OFF \
        -DPCRE2_SUPPORT_JIT=OFF -DPCRE2_SUPPORT_UNICODE=ON \
        -DPCRE2_BUILD_PCRE2GREP=OFF -DPCRE2_BUILD_TESTS=OFF
}
