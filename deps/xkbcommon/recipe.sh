# libxkbcommon: keyboard layouts and key-to-character translation.
# Static, so the engine does not depend on the system's copy. The keyboard
# layout data itself (xkeyboard-config, under /usr/share/X11/xkb) is still
# read from the system at run time.

DEP_VERSION=1.7.0
DEP_SOURCES=(
    "https://github.com/xkbcommon/libxkbcommon/archive/refs/tags/xkbcommon-1.7.0.tar.gz 20d5e40dabd927f7a7f4342bebb1e8c7a59241283c978b800ae3bf60394eabc4"
)

dep_build() {
    dep_meson -Denable-x11=false -Denable-wayland=false -Denable-tools=false \
        -Denable-docs=false -Denable-xkbregistry=false -Denable-bash-completion=false \
        -Dxkb-config-root=/usr/share/X11/xkb -Dx-locale-root=/usr/share/X11/locale
}
