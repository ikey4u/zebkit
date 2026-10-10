# libsoup: the HTTP stack.
# Shared, because WebKit's GLib API hands libsoup objects to the embedder.
# Kerberos and NTLM authentication are off, which drops the krb5 libraries.

DEP_VERSION=3.6.5
DEP_LINKAGE=shared
DEP_SOURCES=(
    "https://download.gnome.org/sources/libsoup/3.6/libsoup-3.6.5.tar.xz 6891765aac3e949017945c3eaebd8cc8216df772456dc9f460976fbdb7ada234"
)

dep_build() {
    dep_meson \
        -Dgssapi=disabled -Dntlm=disabled -Dbrotli=enabled \
        -Dtls_check=false -Dsysprof=disabled \
        -Dintrospection=disabled -Dvapi=disabled -Ddocs=disabled \
        -Dtests=false -Dautobahn=disabled -Dpkcs11_tests=disabled
}
