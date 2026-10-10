# libtasn1: ASN.1 parsing for key import and export in WebCrypto.

DEP_VERSION=4.20.0
DEP_SOURCES=(
    "https://ftp.gnu.org/gnu/libtasn1/libtasn1-4.20.0.tar.gz 92e0e3bd4c02d4aeee76036b2ddd83f0c732ba4cda5cb71d583272b23587a76c"
)

dep_build() {
    dep_autotools --disable-doc --disable-gtk-doc --disable-valgrind-tests
}
