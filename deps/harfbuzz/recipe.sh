# HarfBuzz: text shaping. Built here mainly so that its ICU glue links our
# filtered ICU; the distro's copy would pull the full system ICU back in.

DEP_VERSION=10.2.0
DEP_SOURCES=(
    "https://github.com/harfbuzz/harfbuzz/releases/download/10.2.0/harfbuzz-10.2.0.tar.xz 620e3468faec2ea8685d32c46a58469b850ef63040b3565cde05959825b48227"
)

dep_build() {
    dep_cmake \
        -DHB_HAVE_FREETYPE=ON \
        -DHB_HAVE_ICU=ON \
        -DHB_HAVE_GLIB=OFF \
        -DHB_HAVE_GRAPHITE2=OFF \
        -DHB_BUILD_SUBSET=OFF \
        -DHB_BUILD_UTILS=OFF
}
