# SQLite: storage for IndexedDB, cookies and caches.
# Shared, because both the engine and libsoup use it and should share one copy.

DEP_VERSION=3.46.1
DEP_LINKAGE=shared
DEP_SOURCES=(
    "https://www.sqlite.org/2024/sqlite-autoconf-3460100.tar.gz 67d3fe6d268e6eaddcae3727fce58fcc8e9c53869bdd07a0c61e38ddf2965071"
)

dep_build() {
    # Full-text search, R-tree and session are not used by WebKit or libsoup.
    DEP_CFLAGS="$DEP_CFLAGS -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_DEFAULT_MEMSTATUS=0"
    # zlib is only wanted by the sqlite3 shell; keep the library free of it.
    ac_cv_search_deflate=no \
    dep_autotools --disable-readline --disable-fts3 --disable-fts4 --disable-fts5 \
        --disable-rtree --disable-session
}
