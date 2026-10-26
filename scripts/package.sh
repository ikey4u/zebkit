#!/usr/bin/env bash
# Assemble what a profile ships into one directory, out/blink/<profile>/package,
# with every binary stripped.
#
#   scripts/package.sh <profile>
#
# The engine is libzebkit.so.1, with zebkit-helper for its child processes,
# and the headers an application builds against in include/. For macOS
# (slim-macos), the engine is Zebkit.framework, with its helper, its data
# files and the libraries it loads inside, and include/ beside it. Its run-time
# dependencies, as GN lists them, include the content shell's test fonts,
# test data, mock Vulkan drivers, validation layers and a crash reporter;
# the list below is what the engine needs to run, and nothing else is
# copied.
#
# PACKAGE_STRIP=0 leaves the symbols in.

source "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

profile="${1:-}"
require_profile "$profile"

build_dir="$SRC_DIR/out/$profile"
package="$OUT_DIR/$profile/package"

# The API: <zebkit/zebkit.h> and what the build generated beside it.
copy_headers() {
    mkdir -p "$package/include"
    cp -R "$ZEBKIT_ROOT/src/engine/include/zebkit" "$package/include/"
    if [[ -d "$build_dir/gen/zebkit/engine/include/zebkit" ]]; then
        cp -R "$build_dir/gen/zebkit/engine/include/zebkit" "$package/include/"
    fi
}

# The resource pack less the DevTools front end and the browser's internal
# pages (scripts/trim-pak.py), and ICU's data for the languages zebkit
# supports (scripts/build-icu-data.sh) in place of Chromium's for all of them.
copy_data() {
    local components="$build_dir/gen/components/components_resources.pak"
    local more=()
    [[ -f "$components" ]] && more=("$components" "$components.info")
    python3 -I "$ZEBKIT_ROOT/scripts/trim-pak.py" "$build_dir/content_shell.pak" \
        "$build_dir/content_shell.pak.info" "$1/content_shell.pak" ${more[@]+"${more[@]}"} >&2
    "$ZEBKIT_ROOT/scripts/build-icu-data.sh"
    cp "$OUT_DIR/icu/icudtl.dat" "$1/icudtl.dat"
}

if grep -q '^target_os = "mac"' <<< "$(profile_args "$profile")"; then
    framework="$build_dir/Zebkit.framework"
    [[ -f "$framework/Versions/A/Zebkit" ]] || die "profile '$profile' is not built; run scripts/build.sh $profile"
    rm -rf "$package"
    mkdir -p "$package"
    # As built, with its links (Zebkit -> Versions/Current/Zebkit, ...).
    cp -R "$framework" "$package/"
    copy_data "$package/Zebkit.framework/Versions/A/Resources"
    copy_headers
    if [[ "$ZEBKIT_HOST" == macos ]]; then
        # Native iteration leaves linker stripping off. Strip the copied
        # package instead, without rebuilding host tools and their generated
        # dependencies just to change how the binaries are shipped.
        root="$package/Zebkit.framework/Versions/A"
        helper="$root/Helpers/Zebkit Helper.app"
        strip_tool="$SRC_DIR/third_party/llvm-build/Release+Asserts/bin/llvm-strip"
        if [[ "${PACKAGE_STRIP:-1}" != 0 ]]; then
            while IFS= read -r -d '' binary; do
                # Mach-O arm64, including the helper and ANGLE's libraries.
                magic="$(od -An -tx1 -N4 "$binary" | tr -d ' \n')"
                if [[ "$magic" == cffaedfe ]]; then
                    "$strip_tool" -x -S "$binary"
                    codesign --force --sign - "$binary"
                fi
            done < <(find "$root" -type f -print0)
        fi
        # Resource trimming also changes the bundle. Seal from the inside out.
        codesign --force --sign - "$helper"
        codesign --force --sign - "$package/Zebkit.framework"
        codesign --verify --deep --strict "$package/Zebkit.framework"
    fi
    log "package: $package"
    exit 0
fi

[[ -f "$build_dir/libzebkit.so.1" ]] || die "profile '$profile' is not built; run scripts/build.sh $profile"

files=(
    libzebkit.so.1             # the engine
    zebkit-helper              # its renderer, GPU and utility processes
    content_shell.pak          # resources: Blink's user-agent style sheets, strings, images
    icudtl.dat                 # ICU's data
    snapshot_blob.bin          # V8's startup snapshot
    v8_context_snapshot.bin    # the same with Blink's bindings
    libEGL.so libGLESv2.so     # ANGLE: OpenGL ES for the compositor and WebGL
    libvulkan.so.1             # the Vulkan loader ANGLE and SwiftShader go through
    libvk_swiftshader.so       # SwiftShader: WebGL and compositing without a GPU
    vk_swiftshader_icd.json
)

rm -rf "$package"
mkdir -p "$package"
for file in "${files[@]}"; do
    [[ -f "$build_dir/$file" ]] || die "$file is not in $build_dir"
    cp "$build_dir/$file" "$package/"
done

copy_data "$package"
copy_headers

is_elf() { [[ "$(od -An -tx1 -N4 "$1" | tr -d ' ')" == 7f454c46 ]]; }

strip_tool="$SRC_DIR/third_party/llvm-build/Release+Asserts/bin/llvm-strip"
if [[ "${PACKAGE_STRIP:-1}" != 0 ]]; then
    for file in "$package"/*; do
        if [[ -f "$file" ]] && is_elf "$file"; then "$strip_tool" --strip-all "$file"; fi
    done
fi

# What the package expects from the system, and the oldest glibc that has
# every symbol version it uses.
needed="$(for file in "$package"/*; do
    if [[ -f "$file" ]] && is_elf "$file"; then readelf -d "$file" | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p'; fi
done | sort -u | grep -vxF -f <(ls "$package") | paste -sd' ' -)"
glibc="$(for file in "$package"/*; do
    if [[ -f "$file" ]] && is_elf "$file"; then readelf -V "$file" | grep -o 'GLIBC_[0-9.]*' || true; fi
done | sed 's/GLIBC_//' | sort -uV | tail -1)"

log "expects from the system: $needed"
log "oldest usable glibc: $glibc"
# The oldest glibc the engine runs on; a symbol from a newer one, weak or
# not, keeps glibc 2.17's dynamic linker from loading the package.
glibc_floor=2.17
if [[ -n "$glibc" && "$(printf '%s\n' "$glibc_floor" "$glibc" | sort -V | tail -1)" != "$glibc_floor" ]]; then
    die "the package needs glibc $glibc, newer than $glibc_floor: $(for file in "$package"/*; do
        if [[ -f "$file" ]] && is_elf "$file"; then objdump -T "$file" | awk -v f="${file##*/}" -v v="GLIBC_$glibc" '$0 ~ v { print f ": " $NF }'; fi
    done | sort -u | paste -sd' ' -)"
fi
log "package: $package"
