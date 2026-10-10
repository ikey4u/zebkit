# GLib, GObject and GIO: the base libraries under WebKit's GLib API, libsoup
# and the TLS plugin.
# Shared, because the embedder talks to the engine through GObject types and
# must use the same copy. Built without SELinux, libmount and extended
# attribute support, which drops those libraries from the package.
# GIO looks for plugins (our TLS backend) under the install prefix.

DEP_VERSION=2.84.4
DEP_LINKAGE=shared
DEP_SOURCES=(
    "https://download.gnome.org/sources/glib/2.84/glib-2.84.4.tar.xz 8a9ea10943c36fc117e253f80c91e477b673525ae45762942858aef57631bb90"
)

dep_build() {
    LDFLAGS="-Wl,--gc-sections" \
    dep_meson -Dselinux=disabled -Dlibmount=disabled -Dxattr=false \
        -Dnls=disabled -Dglib_debug=disabled \
        -Dtests=false -Dinstalled_tests=false \
        -Dintrospection=disabled -Ddocumentation=false -Dman-pages=disabled \
        -Dsysprof=disabled -Ddtrace=disabled -Dsystemtap=disabled -Dlibelf=disabled
}
