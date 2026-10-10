# FreeType: font rasterization. Brotli stays on for WOFF2 web fonts; bzip2
# (compressed bitmap fonts) is off, and so is the optional HarfBuzz
# auto-hinter hookup, which would make the two libraries depend on each other.

DEP_VERSION=2.13.3
DEP_SOURCES=(
    "https://download.savannah.gnu.org/releases/freetype/freetype-2.13.3.tar.xz 0550350666d427c74daeb85d5ac7bb353acba5f76956395995311a9c6f063289"
)

dep_build() {
    dep_cmake -DFT_DISABLE_BZIP2=ON -DFT_DISABLE_HARFBUZZ=ON \
        -DFT_REQUIRE_ZLIB=ON -DFT_REQUIRE_PNG=ON -DFT_REQUIRE_BROTLI=ON
}
