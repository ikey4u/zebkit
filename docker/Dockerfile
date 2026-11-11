# A machine that builds zebkit's release for Linux, macOS and Windows and
# publishes it, after scripts/check.sh: the engine's tests, the engine on
# glibc 2.17, the web pages' limits (docker/publish.sh says what it cannot do).
#
#   docker build -t zebkit-release --build-arg UID="$(id -u)" docker
#   docker run --rm -it -v "$PWD:$PWD" -w "$PWD" -v zebkit-mise:/home/builder/.local/share/mise \
#       -v "$HOME/MacOSX:/macos:ro" -e GH_TOKEN zebkit-release   # build, test, publish
#   docker run ... zebkit-release --no-publish            # build and test only
#
# The repository is mounted, not copied, and at the same path as outside:
# the Chromium tree (upstream/), the build (out/), the caches (.cache/) and
# dist/ stay in it from run to run, so a second build compiles only what
# changed, and the tree's links and the build's records, which hold
# absolute paths, stay right inside and outside the container. The
# zebkit-mise volume keeps the tools mise installs. Allow about 150 GB for
# the repository and as much memory as there is.
#
# The image is Debian, not a glibc 2.17 system: what the build runs on does
# not decide what the engine needs. Chromium compiles with its own clang
# against its own sysroot; scripts/package.sh refuses a package that needs
# a glibc newer than 2.17, and scripts/glibc-test.sh runs the engine on
# CentOS 7's glibc 2.17 and libraries before anything is published.
#
# /macos holds Apple's SDK for the macOS build, made once on a Mac
# (scripts/macos-toolchain.sh says how): MacOSX.sdk.tar.xz and version.plist,
# copied into .cache/sdk/macos by the first run (scripts/zsdk-create.sh).

FROM debian:13

# What Chromium's tools and zebkit's scripts call from the system; the
# compiler, sysroot, Rust, Node and ninja come with Chromium's checkout.
# The libraries are what the engine loads, for the tests on this machine;
# then what the tests drive it with: a private X server, a Wayland compositor,
# a session bus with dbus-python, X11's headers, glib's, xdotool and xev
# (x11-utils), wtype (a keyboard for the compositor's seat) and OpenSSL.
# Node.js and Go come from mise (mise.toml).
RUN apt-get update && apt-get install -y --no-install-recommends \
        bison bzip2 ca-certificates curl file flex git gperf binutils \
        build-essential patch perl pkg-config python3 unzip xz-utils zip \
        libasound2t64 libatk-bridge2.0-0t64 libatk1.0-0t64 libatspi2.0-0t64 \
        libdbus-1-3 libexpat1 libgbm1 libglib2.0-0t64 libnspr4 libnss3 \
        libudev1 libx11-6 libxcb1 libxcomposite1 libxdamage1 libxext6 \
        libxfixes3 libxkbcommon0 libxrandr2 rustup \
        xvfb labwc dbus dbus-x11 python3-dbus python3-gi libx11-dev \
        libglib2.0-dev xdotool wtype openssl x11-utils \
    && rm -rf /var/lib/apt/lists/*

# mise, which brings gh and xz at the versions mise.toml pins.
ARG MISE_VERSION=v2026.10.4
RUN curl -fsSL "https://github.com/jdx/mise/releases/download/$MISE_VERSION/mise-$MISE_VERSION-linux-x64" \
        -o /usr/local/bin/mise \
    && chmod +x /usr/local/bin/mise

# The same user id as the owner of the mounted repository, so what the
# build writes there stays theirs. Chromium's tools do not run as root.
ARG UID=1000
RUN useradd --create-home --uid "$UID" builder
USER builder
ENV MISE_YES=1 PATH=/home/builder/.local/share/mise/shims:/home/builder/.cargo/bin:$PATH
# Rust with the cross-built systems' targets, for the archives' zebkit-mcp.
RUN rustup default stable && rustup target add aarch64-apple-darwin x86_64-pc-windows-msvc
RUN git config --global --add safe.directory "*" \
    && mkdir -p /home/builder/.local/share/mise

COPY --chmod=755 publish.sh /usr/local/bin/zebkit-publish
ENTRYPOINT ["zebkit-publish"]
