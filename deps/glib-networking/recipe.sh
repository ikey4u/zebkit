# glib-networking: GIO's TLS backend, loaded by libgio as a plugin.
# GnuTLS and Nettle are linked into the plugin statically, so it is the
# only TLS file that ships. Proxy auto-configuration backends are off.

DEP_VERSION=2.80.1
DEP_LINKAGE=shared
DEP_SOURCES=(
    "https://download.gnome.org/sources/glib-networking/2.80/glib-networking-2.80.1.tar.xz b80e2874157cd55071f1b6710fa0b911d5ac5de106a9ee2a4c9c7bee61782f8e"
)

dep_build() {
    LDFLAGS="-Wl,--gc-sections -Wl,--no-undefined" \
    dep_meson -Dgnutls=enabled -Dopenssl=disabled -Dlibproxy=disabled \
        -Dgnome_proxy=disabled -Denvironment_proxy=disabled \
        -Dinstalled_tests=false
}
