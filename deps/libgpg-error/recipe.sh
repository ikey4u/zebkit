# libgpg-error: error codes and runtime helpers required by libgcrypt.

DEP_VERSION=1.51
DEP_SOURCES=(
    "https://gnupg.org/ftp/gcrypt/libgpg-error/libgpg-error-1.51.tar.bz2 be0f1b2db6b93eed55369cdf79f19f72750c8c7c39fc20b577e724545427e6b2"
)

dep_build() {
    dep_autotools --disable-doc --disable-tests --disable-nls --disable-languages \
        --enable-install-gpg-error-config
}
