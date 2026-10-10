# Brotli: decompression for WOFF2 fonts and "Content-Encoding: br".

DEP_VERSION=1.1.0
DEP_SOURCES=(
    "https://github.com/google/brotli/archive/refs/tags/v1.1.0.tar.gz e720a6ca29428b803f4ad165371771f5398faba397edf6778837a18599ea13ff"
)

dep_build() {
    dep_cmake -DBROTLI_DISABLE_TESTS=ON
}
