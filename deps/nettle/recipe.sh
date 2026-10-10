# Nettle: the cryptographic primitives under GnuTLS. Built with its
# bundled mini-gmp, which removes the separate 0.5 MiB GMP library at the
# cost of slower RSA and Diffie-Hellman handshakes.

DEP_VERSION=3.10.1
DEP_SOURCES=(
    "https://ftp.gnu.org/gnu/nettle/nettle-3.10.1.tar.gz b0fcdd7fc0cdea6e80dcf1dd85ba794af0d5b4a57e26397eee3bc193272d9132"
)

dep_build() {
    dep_autotools --enable-mini-gmp --disable-documentation --disable-openssl
}
