# libjpeg-turbo: JPEG decoding. The assembly SIMD paths need nasm; without
# it the build falls back to plain C, which is smaller but slower.

DEP_VERSION=3.1.0
DEP_SOURCES=(
    "https://github.com/libjpeg-turbo/libjpeg-turbo/releases/download/3.1.0/libjpeg-turbo-3.1.0.tar.gz 9564c72b1dfd1d6fe6274c5f95a8d989b59854575d4bbee44ade7bc17aa9bc93"
)

dep_build() {
    dep_cmake -DENABLE_SHARED=OFF -DENABLE_STATIC=ON -DWITH_TURBOJPEG=OFF \
        -DWITH_JAVA=OFF -DWITH_TOOLS=OFF -DWITH_TESTS=OFF
}
