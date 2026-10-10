# Expat: XML parsing for Fontconfig's configuration files.

DEP_VERSION=2.7.1
DEP_SOURCES=(
    "https://github.com/libexpat/libexpat/releases/download/R_2_7_1/expat-2.7.1.tar.xz 354552544b8f99012e5062f7d570ec77f14b412a3ff5c7d8d0dae62c0d217c30"
)

dep_build() {
    dep_autotools --without-docbook --without-examples --without-tests --without-xmlwf
}
