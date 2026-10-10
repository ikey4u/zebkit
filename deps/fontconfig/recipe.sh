# Fontconfig: system font discovery and matching. Built so that it links
# our FreeType; the distro's copy would load a second FreeType.

DEP_VERSION=2.15.0
DEP_SOURCES=(
    "https://www.freedesktop.org/software/fontconfig/release/fontconfig-2.15.0.tar.xz 63a0658d0e06e0fa886106452b58ef04f21f58202ea02a94c39de0d3335d7c0e"
)

dep_build() {
    # Keep reading the host's configuration and caches, not ones under our prefix.
    dep_meson -Ddoc=disabled -Dtests=disabled -Dtools=disabled -Dnls=disabled \
        -Dcache-build=disabled \
        --sysconfdir=/etc --localstatedir=/var
}
