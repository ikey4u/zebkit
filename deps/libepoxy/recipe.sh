# libepoxy: OpenGL function dispatch. EGL only; no GLX or X11.

DEP_VERSION=1.5.10
DEP_SOURCES=(
    "https://github.com/anholt/libepoxy/archive/refs/tags/1.5.10.tar.gz a7ced37f4102b745ac86d6a70a9da399cc139ff168ba6b8002b4d8d43c900c15"
)

dep_build() {
    dep_meson -Dglx=no -Dx11=false -Degl=yes -Dtests=false -Ddocs=false
}
