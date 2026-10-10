#!/usr/bin/env bash
# Build third-party libraries from source, size-optimized and static, into
# out/deps/prefix. Each library is a recipe in deps/<name>/recipe.sh.
#
#   scripts/build-deps.sh            # everything in deps/order, in order
#   scripts/build-deps.sh icu        # just the named ones
#   DEP_FORCE=1 scripts/build-deps.sh libsoup   # rebuild even if up to date
#
# A recipe sets DEP_VERSION and DEP_SOURCES ("url sha256" per entry) and
# defines dep_build, which runs in an empty build directory. It may set
# DEP_LINKAGE=shared (the default is static) and may define
# dep_extract <archive...> when the default "untar, strip one level" is wrong.
# A dependency is rebuilt when its recipe directory or the flags below change.

source "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

DEPS_DIR="$ZEBKIT_ROOT/deps"
DEPS_OUT="$OUT_DIR/deps"
DEPS_PREFIX="$DEPS_OUT/prefix"

# Everything is compiled with the pinned cross toolchain, so no library can
# pull in a symbol newer than the glibc floor in toolchain.lock.
[[ -x "$TOOLCHAIN_DIR/bin/zebkit-cc" ]] || "$ZEBKIT_ROOT/scripts/fetch-toolchain.sh"
DEP_CC="$TOOLCHAIN_DIR/bin/zebkit-cc"
DEP_CXX="$TOOLCHAIN_DIR/bin/zebkit-c++"
DEP_JOBS="${DEP_JOBS:-$(nproc)}"
# Static, position-independent, one section per function so the engine link
# can drop what it does not reference, and no unwind tables.
# ("-Xclang -Oz -fomit-frame-pointer": the toolchain's driver would otherwise
# turn -Oz into -Os and keep frame pointers.)
BASE_CFLAGS="-Oz -Xclang -Oz -fomit-frame-pointer -DNDEBUG -fPIC -ffunction-sections -fdata-sections -fno-asynchronous-unwind-tables -fno-unwind-tables"

export PKG_CONFIG_PATH="$DEPS_PREFIX/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"

# The helpers below run where "set -e" is suspended (the caller tests the
# recipe's exit status), so each step returns its failure explicitly.

# dep_cmake [args...]: configure, build and install a CMake project from
# $DEP_SRC with the shared flags. For recipes whose library uses CMake.
# (CMAKE_LINK_DEPENDS_USE_LINKER is off because the toolchain's linker
# crashes on the dependency-file option CMake would otherwise pass.)
dep_cmake() {
    local shared=OFF
    [[ "$DEP_LINKAGE" == shared ]] && shared=ON
    cmake -S "$DEP_SRC" -B . -G Ninja \
        -DCMAKE_BUILD_TYPE=MinSizeRel \
        -DCMAKE_C_COMPILER="$DEP_CC" -DCMAKE_CXX_COMPILER="$DEP_CXX" \
        -DCMAKE_C_FLAGS="$DEP_CFLAGS" -DCMAKE_CXX_FLAGS="$DEP_CXXFLAGS" \
        -DCMAKE_C_FLAGS_MINSIZEREL= -DCMAKE_CXX_FLAGS_MINSIZEREL= \
        -DCMAKE_INSTALL_PREFIX="$DEPS_PREFIX" -DCMAKE_INSTALL_LIBDIR=lib \
        -DCMAKE_PREFIX_PATH="$DEPS_PREFIX" \
        -DBUILD_SHARED_LIBS="$shared" -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
        -DCMAKE_LINK_DEPENDS_USE_LINKER=OFF \
        "$@" || return
    ninja -j"$DEP_JOBS" || return
    ninja install
}

# dep_autotools [args...]: the same for a ./configure project.
dep_autotools() {
    local linkage=(--enable-static --disable-shared)
    [[ "$DEP_LINKAGE" == shared ]] && linkage=(--enable-shared --disable-static)
    CC="$DEP_CC" CXX="$DEP_CXX" CFLAGS="$DEP_CFLAGS" CXXFLAGS="$DEP_CXXFLAGS" \
    "$DEP_SRC/configure" --prefix="$DEPS_PREFIX" --libdir="$DEPS_PREFIX/lib" \
        "${linkage[@]}" --with-pic "$@" || return
    make -j"$DEP_JOBS" || return
    make install
}

# dep_meson [args...]: the same for a Meson project.
dep_meson() {
    CC="$DEP_CC" CXX="$DEP_CXX" CFLAGS="$DEP_CFLAGS" CXXFLAGS="$DEP_CXXFLAGS" \
    meson setup . "$DEP_SRC" --prefix="$DEPS_PREFIX" --libdir=lib \
        --buildtype=minsize -Ddefault_library="$DEP_LINKAGE" -Db_ndebug=true \
        --pkg-config-path="$DEPS_PREFIX/lib/pkgconfig" --cmake-prefix-path="$DEPS_PREFIX" \
        "$@" || return
    ninja -j"$DEP_JOBS" || return
    ninja install
}

# A static library cannot be linked without the libraries it uses itself, but
# pkg-config files list those as private, which consumers only read when they
# ask for static linking, and most build systems here do not. For the .pc
# files a static recipe just installed, fold the private entries into the
# public ones.
publish_private_dependencies() {
    local marker="$1" pc
    [[ -d "$DEPS_PREFIX/lib/pkgconfig" ]] || return 0
    while IFS= read -r pc; do
        python3 - "$pc" <<'PY'
import re, sys
path = sys.argv[1]
fields, order, rest = {}, [], []
for line in open(path).read().splitlines():
    m = re.match(r"^(Requires|Requires\.private|Libs|Libs\.private):\s*(.*)$", line)
    if m:
        fields[m.group(1)] = m.group(2).split("#")[0].strip()
        if m.group(1) in ("Requires", "Libs"):
            order.append(m.group(1))
            rest.append(None)
    else:
        rest.append(line)
for public, private, joiner in (("Requires", "Requires.private", ", "), ("Libs", "Libs.private", " ")):
    extra = fields.pop(private, "")
    if extra:
        fields[public] = joiner.join(x for x in (fields.get(public, ""), extra) if x)
        if public not in order:
            order.append(public)
            rest.append(None)
names = iter(order)
out = []
for line in rest:
    if line is None:
        name = next(names)
        out.append(f"{name}: {fields[name]}")
    else:
        out.append(line)
open(path, "w").write("\n".join(out) + "\n")
PY
    done < <(find "$DEPS_PREFIX/lib/pkgconfig" -name '*.pc' -newer "$marker")
}

build_one() {
    local name="$1"
    DEP_DIR="$DEPS_DIR/$name"
    [[ -f "$DEP_DIR/recipe.sh" ]] || die "no recipe for '$name' in $DEPS_DIR"

    unset -f dep_extract dep_build
    unset DEP_VERSION DEP_SOURCES
    DEP_LINKAGE=static
    DEP_CFLAGS="$BASE_CFLAGS"
    DEP_CXXFLAGS="$BASE_CFLAGS"
    # shellcheck disable=SC1091
    source "$DEP_DIR/recipe.sh"

    DEP_SRC="$DEPS_OUT/src/$name-$DEP_VERSION"
    local build="$DEPS_OUT/build/$name-$DEP_VERSION"
    local stamp="$DEPS_OUT/stamps/$name"
    local want
    want="$(cat "$DEP_DIR"/* | sha256sum | cut -d' ' -f1) $TOOLCHAIN_SHA256 $TOOLCHAIN_TARGET $BASE_CFLAGS"

    if [[ -z "${DEP_FORCE:-}" && -f "$stamp" && "$(cat "$stamp")" == "$want" ]]; then
        log "$name $DEP_VERSION is up to date"
        return
    fi

    local archives=() entry url sha file
    mkdir -p "$CACHE_DIR/deps"
    for entry in ${DEP_SOURCES[@]+"${DEP_SOURCES[@]}"}; do
        read -r url sha <<< "$entry"
        file="$CACHE_DIR/deps/$(basename "$url")"
        if [[ ! -f "$file" ]] || ! echo "$sha  $file" | sha256sum -c --status; then
            log "downloading $url"
            curl -fL --retry 3 -sS -o "$file.part" "$url"
            mv "$file.part" "$file"
        fi
        echo "$sha  $file" | sha256sum -c --status || die "checksum mismatch for $file"
        archives+=("$file")
    done

    log "extracting $name $DEP_VERSION"
    rm -rf "$DEP_SRC" "$build"
    mkdir -p "$DEP_SRC" "$build" "$DEPS_PREFIX" "$DEPS_OUT/stamps"
    if declare -F dep_extract >/dev/null; then
        dep_extract "${archives[@]}"
    elif (( ${#archives[@]} )); then
        tar -xf "${archives[0]}" -C "$DEP_SRC" --strip-components=1
    fi

    log "building $name $DEP_VERSION (log: $build.log)"
    local started="$DEPS_OUT/stamps/.started"
    # Dated slightly in the past: some installers round file times down to
    # the second, and a file from this build must still count as newer.
    touch -d "@$(( $(date +%s) - 2 ))" "$started"
    ( cd "$build" && dep_build ) > "$build.log" 2>&1 \
        || { tail -n 30 "$build.log" >&2; die "$name failed to build"; }
    [[ "$DEP_LINKAGE" == static ]] && publish_private_dependencies "$started"

    echo "$want" > "$stamp"
    log "installed $name $DEP_VERSION into $DEPS_PREFIX"
}

if (( $# )); then
    names=("$@")
else
    mapfile -t names < <(sed -e 's/#.*//' -e '/^\s*$/d' "$DEPS_DIR/order")
fi

for name in "${names[@]}"; do
    build_one "$name"
done
