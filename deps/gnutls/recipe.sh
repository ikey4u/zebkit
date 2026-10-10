# GnuTLS: TLS for HTTPS, used through glib-networking.
#
# Left out: PKCS#11 (p11-kit, 1.6 MiB), IDN (libidn2 + libunistring,
# 2.1 MiB), certificate compression, TPM, DANE, SSL 3, SRP, anonymous
# and GOST suites, the C++ binding and the command-line tools.
#
# Without p11-kit, system CA certificates are read from compiled-in paths.
# Two are set, and a missing one is skipped: the bundle file Debian, Ubuntu
# and Arch use, and the /etc/ssl/certs directory, which Fedora, RHEL and
# openSUSE also populate.

DEP_VERSION=3.8.9
DEP_SOURCES=(
    "https://www.gnupg.org/ftp/gcrypt/gnutls/v3.8/gnutls-3.8.9.tar.xz 69e113d802d1670c4d5ac1b99040b1f2d5c7c05daec5003813c049b5184820ed"
)

dep_build() {
    dep_autotools \
        --with-nettle-mini \
        --with-included-libtasn1 --with-included-unistring \
        --without-p11-kit --without-idn --without-brotli --without-zstd --without-zlib \
        --without-tpm --without-tpm2 \
        --with-default-trust-store-file=/etc/ssl/certs/ca-certificates.crt \
        --with-default-trust-store-dir=/etc/ssl/certs \
        --disable-doc --disable-tests --disable-tools --disable-cxx --disable-nls \
        --disable-libdane --disable-ssl3-support --disable-ssl2-support \
        --disable-srp-authentication --disable-anon-authentication --disable-gost
}
