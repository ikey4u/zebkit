# libpng: PNG decoding, also used by FreeType for colour bitmap fonts.

DEP_VERSION=1.6.48
DEP_SOURCES=(
    "https://download.sourceforge.net/libpng/libpng-1.6.48.tar.xz 46fd06ff37db1db64c0dc288d78a3f5efd23ad9ac41561193f983e20937ece03"
)

dep_build() {
    dep_cmake -DPNG_SHARED=OFF -DPNG_STATIC=ON -DPNG_TESTS=OFF -DPNG_TOOLS=OFF
}
