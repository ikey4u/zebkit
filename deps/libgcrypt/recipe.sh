# libgcrypt: cryptographic primitives behind WebCrypto and hashing.
#
# libgcrypt registers every enabled algorithm in static tables, so the
# linker cannot drop unused ones; the lists below are what WebKit calls
# (AES, SHA-1/2, HMAC, PBKDF2, RSA, and ECC for ECDSA/ECDH/Ed25519/X25519).
# SHA-3 and BLAKE2 are there because libgcrypt's own KDF and digest code
# does not link without them. Extend the lists if WebCrypto gains an algorithm.

DEP_VERSION=1.11.0
DEP_SOURCES=(
    "https://gnupg.org/ftp/gcrypt/libgcrypt/libgcrypt-1.11.0.tar.bz2 09120c9867ce7f2081d6aaa1775386b98c2f2f246135761aae47d81f58685b9c"
)

dep_build() {
    # libgcrypt compiles its jitter entropy source with -O0 and refuses to
    # build if any other level reaches it, which "-Xclang -Oz" would.
    DEP_CFLAGS="${DEP_CFLAGS/-Xclang -Oz /}"
    dep_autotools --disable-doc \
        --with-libgpg-error-prefix="$DEPS_PREFIX" \
        --enable-ciphers=aes \
        --enable-pubkey-ciphers=rsa,ecc \
        --enable-digests=sha1,sha256,sha512,sha3,blake2 \
        --enable-kdfs=pkdf2
}
