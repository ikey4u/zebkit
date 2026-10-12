#!/usr/bin/env bash
# Assemble what a profile ships into one flat, relocatable directory:
# out/<profile>/package/. Everything we built is stripped. The engine finds
# its helper processes, injected bundle and GIO plugins next to itself
# (patches/2001), and the engine and executables carry an $ORIGIN run path,
# so the directory works wherever it is copied; an embedder only has to load
# libzebkit from it.
#
#   scripts/package.sh <profile>
#
# On macOS the engine is frameworks and the package is laid out
# differently: scripts/package-macos.sh.
#
# PACKAGE_STRIP=0 leaves symbols and debug information in, for a package
# that is going to be debugged and not shipped.
#
# Libraries come from scripts/build-deps.sh when the profile links them.
# Whatever else the engine needs that a base Linux system does not provide is
# copied from the host as is, so that the directory, and the size
# scripts/measure.sh reports for it, is always the whole thing.

source "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

[[ "$ZEBKIT_HOST" == macos ]] && exec "$ZEBKIT_ROOT/scripts/package-macos.sh" "$@"

profile="${1:-}"
require_profile "$profile"
stage_profile "$profile"

# Libraries every desktop Linux already provides: the C library, the
# GL/windowing stack with its drivers, and ALSA. These are never bundled.
SYSTEM_LIBS='^(linux-vdso|ld-linux[a-z0-9_-]*|libc|libm|libdl|librt|libpthread|libresolv|libgcc_s|libstdc\+\+|libasound|libEGL|libGL|libGLESv2|libGLdispatch|libGLX|libOpenGL|libgbm|libdrm|libwayland-[a-z]+|libxkbcommon|libudev|libinput|libX[a-z0-9]*|libxcb[a-z0-9-]*|libsystemd|libcap|libselinux|libmount|libblkid)\.so'

package="$build_dir/package"
deps_lib="$OUT_DIR/deps/prefix/lib"
uses_deps=0
[[ "${run_env[*]}" == *"$deps_lib"* ]] && uses_deps=1
search_path="$stage_dir/lib"
(( uses_deps )) && search_path+=":$deps_lib"

rm -rf "$package"
mkdir -p "$package/gio/modules"

# add <file> [name]: copy a real file under its run-time name; strip it if we built it.
add() {
    local source name
    source="$(readlink -f "$1")"
    name="${2:-$(basename "$1")}"
    cp "$source" "$package/$name"
    [[ "${PACKAGE_STRIP:-1}" != 0 && "$source" == "$OUT_DIR"/* ]] && strip --strip-unneeded "$package/$name"
    return 0
}

engine="$(find "$stage_dir/lib" -maxdepth 1 -name 'libzebkit.so.[0-9]*' ! -name '*.so.*.*' | head -1)"
[[ -n "$engine" ]] || die "no engine library in $stage_dir/lib"
add "$engine"
for helper in "$stage_dir"/libexec/zebkit-*; do
    add "$helper"
done
add "$(find "$stage_dir/lib" -name 'libzebkit-bundle.so' | head -1)"

# HTTPS needs GIO's TLS backend, which libgio loads as a plugin at run time.
tls_plugin="$deps_lib/gio/modules/libgiognutls.so"
(( uses_deps )) && [[ -f "$tls_plugin" ]] \
    || tls_plugin="$(pkg-config --variable=giomoduledir gio-2.0 2>/dev/null)/libgiognutls.so"
if [[ -f "$tls_plugin" ]]; then
    add "$tls_plugin" "gio/modules/libgiognutls.so"
fi

# Add what the packaged files themselves ask for (their NEEDED entries),
# minus what the system provides, and repeat for whatever that brought in.
# Libraries that only system libraries need are the system's business.
added=1
while (( added )); do
    added=0
    for file in "$package"/*.so* "$package"/zebkit-* "$package"/gio/modules/*.so; do
        [[ -f "$file" ]] || continue
        resolved="$(LD_LIBRARY_PATH="$search_path" ldd "$file" 2>/dev/null | awk '$2 == "=>" && $3 ~ /^\// { print $1, $3 }')"
        while read -r soname; do
            [[ "$soname" =~ $SYSTEM_LIBS || -e "$package/$soname" ]] && continue
            path="$(awk -v n="$soname" '$1 == n { print $2; exit }' <<< "$resolved")"
            [[ -n "$path" ]] || die "cannot find $soname, needed by $(basename "$file")"
            add "$path" "$soname"
            added=1
        done < <(objdump -p "$file" | awk '/NEEDED/ { print $2 }')
    done
done

mapfile -t binaries < <(find "$package" -type f)

# What an application needs to compile against the endpoint in src/cdp, when
# the profile has it. The rest of the API is upstream's, in the stage.
if [[ "$(nm -D --defined-only "$package/$(basename "$engine")")" == *" zebkit_cdp_listen"* ]]; then
    mkdir -p "$package/include"
    cp "$ZEBKIT_ROOT/src/cdp/zebkit-cdp.h" "$package/include/"
fi
# And to use the libraries inside the engine (scripts/gen-vendor.py).
if [[ "$(nm -D --defined-only "$package/$(basename "$engine")")" == *" zebkit_vendor_"* ]]; then
    mkdir -p "$package/include"
    cp -r "$OUT_DIR/deps/vendor/include/zebkit" "$package/include/"
fi
system_libs="$(objdump -p "${binaries[@]}" | awk '/NEEDED/ { print $2 }' | sort -u \
    | while read -r soname; do [[ -e "$package/$soname" ]] || echo "$soname"; done | paste -sd' ')"
glibc_floor="$(objdump -T "${binaries[@]}" 2>/dev/null | grep -oE 'GLIBC_[0-9]+\.[0-9]+' | sort -uV | tail -1)"

# The size is what runs: headers in include/ are for building against it.
log "package: $package ($(du -sb --exclude=include "$package" | awk '{ printf "%.1f MiB", $1 / 1048576 }'), ${#binaries[@]} files)"
log "expects from the system: $system_libs"
log "oldest usable glibc: ${glibc_floor#GLIBC_}"
