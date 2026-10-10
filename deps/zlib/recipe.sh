# zlib: deflate, used by nearly everything else here.

DEP_VERSION=1.3.1
DEP_SOURCES=(
    "https://github.com/madler/zlib/releases/download/v1.3.1/zlib-1.3.1.tar.xz 38ef96b8dfe510d42707d9c781877914792541133e1870841463bfa73f883e32"
)

dep_build() {
    # zlib has its own configure script, not autoconf's.
    CC="$DEP_CC" CFLAGS="$DEP_CFLAGS" "$DEP_SRC/configure" --static --prefix="$DEPS_PREFIX" || return
    make -j"$DEP_JOBS" || return
    make install
}
