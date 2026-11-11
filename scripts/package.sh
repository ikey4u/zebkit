#!/usr/bin/env bash
# Assemble what a profile ships into one directory, out/blink/<profile>/package,
# with every binary stripped.
#
#   scripts/package.sh <profile>
#
# The engine is libzebkit.so.1, with zebkit-helper for its child processes,
# and the headers an application builds against in include/. For macOS
# (slim-macos), the engine is Zebkit.framework, with its helper, its data
# files and the libraries it loads inside, and include/ beside it. For Windows
# (slim-windows), zebkit.dll with its import library and zebkit-helper.exe, with
# the data files and libraries beside them. Its run-time
# dependencies, as GN lists them, include the content shell's test fonts,
# test data, mock Vulkan drivers, validation layers and a crash reporter;
# the list below is what the engine needs to run, and nothing else is
# copied.
#
# PACKAGE_STRIP=0 leaves the symbols in.
#
# Nothing is done when the package is up to date: when what it is made of (the
# build's files, by size and time, the profile's arguments, these scripts, the
# notices) and the package itself are as the last run left them
# (out/blink/<profile>/package.stamp). What the last run measured of it
# (package.info) is logged again.

source "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

profile="${1:-}"
require_profile "$profile"

build_dir="$SRC_DIR/out/$profile"
package="$OUT_DIR/$profile/package"
stamp="$OUT_DIR/$profile/package.stamp"
info="$OUT_DIR/$profile/package.info"

# ICU's data is built first (it has a stamp of its own): it is an input.
"$ZEBKIT_ROOT/scripts/build-icu-data.sh"

# listing <path...>: every file under them, with its size and time.
listing() {
    python3 -I - "$@" <<'PY'
import os, sys
for top in sys.argv[1:]:
    if os.path.isfile(top):
        paths = [top]
    else:
        paths = []
        for root, dirs, files in os.walk(top):
            dirs.sort()
            paths += [os.path.join(root, name) for name in sorted(files)]
    for path in paths:
        st = os.lstat(path)
        print(path, st.st_size, st.st_mtime_ns)
PY
}
# What the package is made of. The build directory's own files (not its
# object files), and what of gen/ and of the framework is copied.
inputs_key() {
    local tops=("$build_dir/args.gn" "$ZEBKIT_ROOT/src/engine/include" "$OUT_DIR/icu/icudtl.dat")
    local file
    for file in "$build_dir"/*; do
        [[ -f "$file" && "$file" != *.log && "$file" != */siso_* && "$file" != */.siso* ]] && tops+=("$file")
    done
    for file in "$build_dir/gen/zebkit/engine/include" "$build_dir/gen/components/components_resources.pak" \
        "$build_dir/gen/components/components_resources.pak.info" "$build_dir/Zebkit.framework" \
        "$SRC_DIR/out/$profile-sandbox/zebkit_sandbox.lib"; do
        [[ -e "$file" ]] && tops+=("$file")
    done
    {
        echo "$profile ${PACKAGE_STRIP:-1} $CHROMIUM_VERSION $ZEBKIT_HOST"
        listing "${tops[@]}"
        cat "$ZEBKIT_ROOT/VERSION" "$ZEBKIT_ROOT/LICENSE" "$ZEBKIT_ROOT/scripts/NOTICE.in" "$ZEBKIT_ROOT/scripts/package.sh" \
            "$ZEBKIT_ROOT/scripts/trim-pak.py" "$ZEBKIT_ROOT/scripts/third-party-notices.py" "$ZEBKIT_ROOT/scripts/zebkit-sandbox-install"
    } | sha256 | cut -d' ' -f1
}
package_key() { listing "$package" | sha256 | cut -d' ' -f1; }
if [[ -f "$stamp" && -d "$package" ]] && [[ "$(cat "$stamp")" == "$(inputs_key) $(package_key)" ]]; then
    while IFS= read -r line; do log "$line"; done < "$info"
    log "package is up to date: $package"
    exit 0
fi
rm -f "$stamp"
: > "$info"
# done: record the package as made from these inputs.
done_package() {
    echo "$(inputs_key) $(package_key)" > "$stamp"
    log "package: $package"
    exit 0
}

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
    python3 -I "$ZEBKIT_ROOT/scripts/trim-pak.py" "$build_dir/zebkit.pak" \
        "$build_dir/zebkit.pak.info" "$1/zebkit.pak" ${more[@]+"${more[@]}"} >&2
    "$ZEBKIT_ROOT/scripts/build-icu-data.sh"
    cp "$OUT_DIR/icu/icudtl.dat" "$1/icudtl.dat"
}

# What the package is under, and what it contains under theirs (docs/LICENSING.md):
# LICENSE, NOTICE, and THIRD_PARTY_NOTICES.txt from Chromium's own tool over the
# engine's dependencies (scripts/third-party-notices.py). release.sh appends the
# crates of the tools it adds.
copy_notices() {
    local target_os version
    target_os="$(profile_args "$profile" | sed -n 's/^target_os = "\(.*\)"/\1/p' | tail -1)"
    version="$(tr -d '[:space:]' < "$ZEBKIT_ROOT/VERSION")"
    cp "$ZEBKIT_ROOT/LICENSE" "$package/LICENSE"
    sed -e "s/@VERSION@/$version/g" -e "s/@CHROMIUM@/$CHROMIUM_VERSION/g" \
        "$ZEBKIT_ROOT/scripts/NOTICE.in" > "$package/NOTICE"
    (cd "$SRC_DIR" && python3 -I "$ZEBKIT_ROOT/scripts/third-party-notices.py" engine "out/$profile" \
        "${target_os:-linux}" "$package/THIRD_PARTY_NOTICES.txt") >&2 \
        || die "no third-party notices for $profile"
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
    copy_notices
    done_package
fi

if grep -q '^target_os = "win"' <<< "$(profile_args "$profile")"; then
    [[ -f "$build_dir/zebkit.dll" ]] || die "profile '$profile' is not built; run scripts/build.sh $profile"
    rm -rf "$package"
    mkdir -p "$package"
    for file in \
        zebkit.dll             `# the engine` \
        zebkit.dll.lib         `# its import library, to link an application with` \
        zebkit-helper.exe      `# its renderer, GPU and utility processes` \
        snapshot_blob.bin      `# V8's startup snapshot` \
        libEGL.dll libGLESv2.dll `# ANGLE: OpenGL ES on Direct3D` \
        vk_swiftshader.dll vk_swiftshader_icd.json vulkan-1.dll `# drawing without a GPU` \
        ; do
        [[ -f "$build_dir/$file" ]] || die "$file is not in $build_dir"
        cp "$build_dir/$file" "$package/"
    done
    copy_data "$package"
    copy_headers
    # For the application's executable (<zebkit/win_sandbox.h>).
    sandbox_lib="$SRC_DIR/out/$profile-sandbox/zebkit_sandbox.lib"
    [[ -f "$sandbox_lib" ]] || die "no zebkit_sandbox.lib; run scripts/build.sh $profile-sandbox zebkit/engine:zebkit_sandbox"
    cp "$sandbox_lib" "$package/"
    copy_notices
    done_package
fi

[[ -f "$build_dir/libzebkit.so.1" ]] || die "profile '$profile' is not built; run scripts/build.sh $profile"

files=(
    libzebkit.so.1             # the engine
    zebkit-helper              # its renderer, GPU and utility processes
    zebkit.pak          # resources: Blink's user-agent style sheets, strings, images
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
# The optional sandbox helper (docs/SANDBOX.md): not set-user-ID until installed,
# which only root can do and a package cannot.
cp "$build_dir/chrome_sandbox" "$package/chrome-sandbox"
cp "$ZEBKIT_ROOT/scripts/zebkit-sandbox-install" "$package/zebkit-sandbox-install"
copy_notices

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

printf '%s\n' "expects from the system: $needed" "oldest usable glibc: $glibc" > "$info"
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
done_package
