# libxml2: XML parsing. WebKit only drives the push parser, so everything
# that needs another library or is a separate XML technology is off.

DEP_VERSION=2.13.8
DEP_SOURCES=(
    "https://download.gnome.org/sources/libxml2/2.13/libxml2-2.13.8.tar.xz 277294cb33119ab71b2bc81f2f445e9bc9435b893ad15bb2cd2b0e859a0ee84a"
)

dep_build() {
    local off=(PYTHON LZMA ZLIB ICONV ICU HTTP PROGRAMS TESTS DEBUG LEGACY MODULES
               CATALOG SCHEMAS SCHEMATRON VALID XINCLUDE XPTR C14N READER WRITER
               PATTERN REGEXPS XPATH HTML ISO8859X)
    local args=()
    for feature in "${off[@]}"; do
        args+=("-DLIBXML2_WITH_$feature=OFF")
    done
    dep_cmake "${args[@]}"
}
