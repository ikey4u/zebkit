# libpsl: public-suffix list lookups for cookie scoping in libsoup.
# Built with the list compiled in and no IDN library, which is what keeps
# libidn2 and the 1.9 MiB libunistring out of the package.

DEP_VERSION=0.21.5
DEP_SOURCES=(
    "https://github.com/rockdaboot/libpsl/releases/download/0.21.5/libpsl-0.21.5.tar.gz 1dcc9ceae8b128f3c0b3f654decd0e1e891afc6ff81098f227ef260449dae208"
)

dep_build() {
    dep_autotools --disable-runtime --enable-builtin --disable-man --disable-nls
}
