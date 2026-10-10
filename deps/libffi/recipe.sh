# libffi: foreign-function calls for GObject's signal marshalling.

DEP_VERSION=3.4.8
DEP_SOURCES=(
    "https://github.com/libffi/libffi/releases/download/v3.4.8/libffi-3.4.8.tar.gz bc9842a18898bfacb0ed1252c4febcc7e78fa139fd27fdc7a3e30d9d9356119b"
)

dep_build() {
    dep_autotools --disable-docs --disable-multi-os-directory
}
