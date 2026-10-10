# ICU: Unicode, text segmentation, collation and locale data.
#
# The stock data file is 30 MiB. filter.json keeps what WebKit reaches:
# every break rule and normalization table, the legacy converters WebKit
# asks ICU for by name, root collation, and locale data for the languages
# listed under localeFilter. See README.md in this directory.

DEP_VERSION=76.1
DEP_SOURCES=(
    "https://github.com/unicode-org/icu/releases/download/release-76-1/icu4c-76_1-src.tgz dfacb46bfe4747410472ce3e1144bf28a102feeaa4e3875bac9b4c6cf30f4f3e"
    "https://github.com/unicode-org/icu/releases/download/release-76-1/icu4c-76_1-data.zip 133ae58a67d68b46f7296822904cd3c30126a0b4b2f65f0f905e7f47c0ef9e47"
)

# The source tarball ships a prebuilt data file; filtering needs the data
# sources from the second archive in its place.
dep_extract() {
    tar -xf "$1" -C "$DEP_SRC" --strip-components=1 --exclude='icu/source/data'
    unzip -q "$2" -d "$DEP_SRC/source"
}

dep_build() {
    # ICU needs RTTI (it uses dynamic_cast) but not exceptions.
    ICU_DATA_FILTER_FILE="$DEP_DIR/filter.json" \
    CC="$DEP_CC" CXX="$DEP_CXX" CFLAGS="$DEP_CFLAGS" CXXFLAGS="$DEP_CXXFLAGS -fno-exceptions" \
    "$DEP_SRC/source/configure" \
        --prefix="$DEPS_PREFIX" \
        --enable-static --disable-shared \
        --with-data-packaging=static \
        --disable-tests --disable-samples --disable-extras \
        --disable-icuio --disable-layoutex
    make -j"$DEP_JOBS"
    make install
}
