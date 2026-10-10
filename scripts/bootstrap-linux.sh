#!/usr/bin/env bash
# Install the toolchain and library headers needed to build every profile on
# Debian/Ubuntu. The baseline profile builds upstream defaults, so this list
# is deliberately wider than what the slim profile links against.

source "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

TOOLCHAIN=(
    build-essential g++ clang lld llvm
    cmake ninja-build meson ccache pkg-config
    ruby perl python3 gperf unifdef gettext nasm m4 flex bison
    patch xz-utils unzip curl binutils
)

# Linked by every profile.
CORE_LIBS=(
    libicu-dev libglib2.0-dev zlib1g-dev
    libharfbuzz-dev libfreetype-dev libfontconfig-dev
    libjpeg-dev libpng-dev libwebp-dev
    libxml2-dev libsqlite3-dev
    libsoup-3.0-dev libgcrypt20-dev libtasn1-6-dev
    libepoxy-dev libegl1-mesa-dev libgles-dev libdrm-dev libgbm-dev
    libwpe-1.0-dev libwpebackend-fdo-1.0-dev
    libxkbcommon-dev libwayland-dev wayland-protocols
    libinput-dev libudev-dev libasound2-dev
)

# Only needed for the stock-feature baseline profile.
BASELINE_LIBS=(
    libxslt1-dev libenchant-2-dev libwoff-dev liblcms2-dev libavif-dev libjxl-dev libhyphen-dev
    libatk1.0-dev libatk-bridge2.0-dev libatspi2.0-dev libsystemd-dev libmanette-0.2-dev
    libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev
    libgstreamer-plugins-bad1.0-dev
)

SUDO=""
[[ $EUID -eq 0 ]] || SUDO="sudo"

log "installing ${#TOOLCHAIN[@]} toolchain and $(( ${#CORE_LIBS[@]} + ${#BASELINE_LIBS[@]} )) library packages"
$SUDO apt-get update

# A -dev package must match its runtime library exactly. On hosts where the
# runtime came from <codename>-backports, take the matching -dev from there.
codename="$(. /etc/os-release && echo "${VERSION_CODENAME:-}")"
packages=()
for pkg in "${TOOLCHAIN[@]}" "${CORE_LIBS[@]}" "${BASELINE_LIBS[@]}"; do
    if [[ $pkg == lib*-dev ]]; then
        runtime="$(apt-cache depends "$pkg" | awk '/Depends: lib/ {print $2; exit}')"
        installed="$(dpkg-query -W -f '${Version}\n' "$runtime" 2>/dev/null || true)"
        [[ $installed == *~bpo* ]] && pkg="$pkg/$codename-backports"
    fi
    packages+=("$pkg")
done

# APT_OPTS passes extra apt options through, e.g. to relocate the download cache.
# shellcheck disable=SC2086
$SUDO env DEBIAN_FRONTEND=noninteractive apt-get ${APT_OPTS:-} install -y --no-install-recommends "${packages[@]}"
