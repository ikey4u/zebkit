#!/usr/bin/env bash
# Assemble what a profile ships on macOS into one relocatable directory:
# out/<profile>/package/. This is what scripts/package.sh runs on a Mac.
#
#   scripts/package.sh <profile>
#
# The directory holds one framework, which is what a program links, and
# include/ with the headers that are the same on every system:
#
#   Zebkit.framework/
#       Zebkit                       re-exports the engine; nothing else is in it
#       Headers/                     the API: <Zebkit/Zebkit.h>
#       Frameworks/                  the engine, as WebKit's build names it:
#           WebKit.framework  WebCore.framework  JavaScriptCore.framework  ...
#           libwebrtc.dylib  libANGLE-shared.dylib
#           com.apple.WebKit.WebContent.xpc  ...
#   include/
#
# What is inside keeps WebKit's names because it is found by them. macOS
# has a WebKit of its own, which its own libraries use; the loader, told a
# directory, takes WebKit.framework, WebCore.framework and
# JavaScriptCore.framework from there in the system's place, for the
# program and for those libraries, so that a process has one engine and
# not two. Under other names the system's would be loaded beside ours.
#
# It works wherever it is copied. The services look for the frameworks in
# the directory they are in themselves, which WebKit's build arranges. A
# program is linked with
#
#   -F<directory> -framework Zebkit -Wl,-rpath,@executable_path/<path to the directory>
#   -Wl,-dyld_env,DYLD_FRAMEWORK_PATH=@executable_path/<path to the directory>/Zebkit.framework/Versions/A/Frameworks
#
# and without the last of these does not start.
#
# PACKAGE_STRIP=0 leaves local symbols in.
#
# Everything is signed ad hoc, as Xcode leaves it: enough to run on the
# machine that built it and on others once they allow it, not what a
# distributed application needs (docs/MACOS.md).

source "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

profile="${1:-}"
require_profile "$profile"
stage_profile "$profile"

package="$build_dir/package"
umbrella="$package/Zebkit.framework"
version_dir="$umbrella/Versions/A"
# The engine, under WebKit's names.
frameworks="$version_dir/Frameworks"
rm -rf "$package"
mkdir -p "$package/include" "$frameworks" "$version_dir/Headers" "$version_dir/Resources"

# is_ours <path of a dependency>: prints what it is called in the build's
# products if the build made it, and nothing for what the system supplies.
is_ours() {
    local name
    case "$1" in
        *.framework/*) name="$(sed -E 's#.*/([^/]+\.framework)/.*#\1#' <<< "$1")" ;;
        *) name="$(basename "$1")" ;;
    esac
    # The inspector's interface is 18 MiB of resources for a window zebkit
    # does not have; WebKit loads it only when asked to show one.
    [[ "$name" == WebInspectorUI.framework ]] && return 0
    [[ -e "$products_dir/$name" ]] && echo "$name"
    return 0
}

# Start from the framework applications link against, and add what the
# packaged binaries themselves ask for until nothing is missing.
cp -R "$products_dir/WebKit.framework" "$frameworks/"
added=1
while (( added )); do
    added=0
    while IFS= read -r binary; do
        while IFS= read -r dependency; do
            name="$(is_ours "$dependency")"
            [[ -n "$name" && ! -e "$frameworks/$name" ]] || continue
            cp -R "$products_dir/$name" "$frameworks/"
            added=1
        done < <(otool -L "$binary" | awk 'NR > 1 { print $1 }')
    done < <(find "$frameworks" -type f \( -name '*.dylib' -o -path '*.framework/Versions/A/*' \) ! -path '*/Resources/*' ! -name '*.plist' -perm +111)
done
# The services are started by name, not linked.
for service in "$products_dir"/com.apple.WebKit.*.xpc; do
    cp -R "$service" "$frameworks/"
done

engine="$frameworks/WebKit.framework/Versions/A/WebKit"
minimum_system="$(otool -l "$engine" | awk '/LC_BUILD_VERSION/ { found = 1 } found && /minos/ { print $2; exit }')"
exported="$(nm -gU "$engine")"

# The API is WebKit's public headers under the framework's name, so that a
# program includes <Zebkit/Zebkit.h> and nothing of the system's WebKit,
# with zebkit's own beside them. Everything else the build made headers
# for is the engine's own business.
for header in "$frameworks/WebKit.framework/Versions/A/Headers"/*.h; do
    sed 's#<WebKit/#<Zebkit/#g' "$header" > "$version_dir/Headers/$(basename "$header")"
done
mv "$version_dir/Headers/WebKit.h" "$version_dir/Headers/Zebkit.h"
own_headers=()
[[ "$exported" == *" _zebkit_view_new"* ]] && own_headers+=("$ZEBKIT_ROOT/src/view/zebkit-view.h")
[[ "$exported" == *" _zebkit_cdp_listen"* ]] && own_headers+=("$ZEBKIT_ROOT/src/cdp/zebkit-cdp.h")
for header in ${own_headers[@]+"${own_headers[@]}"}; do
    cp "$header" "$version_dir/Headers/"
    printf '#import <Zebkit/%s>\n' "$(basename "$header")" >> "$version_dir/Headers/Zebkit.h"
done
find "$frameworks" -type d \( -name Headers -o -name PrivateHeaders -o -name Modules -o -name '*.dSYM' \) -prune -exec rm -rf {} +
# Links to what was just taken out.
find "$frameworks" -type l ! -exec test -e {} \; -delete

# include/ is what the Linux package has there, for the programs that are
# one source for both systems.
[[ "$exported" == *" _zebkit_cdp_listen"* ]] && cp "$ZEBKIT_ROOT/src/cdp/zebkit-cdp.h" "$package/include/"
if [[ "$exported" == *" _zebkit_vendor_"* ]]; then
    cp -R "$OUT_DIR/deps/vendor/include/zebkit" "$package/include/"
fi

# The framework a program links. Its binary has no code: it hands on
# everything WebKit.framework exports, zebkit's own entry points among
# them, and names that framework by where it is from here, which is how
# the linker of a program finds the engine's symbols and not those of the
# system's WebKit.
system_name=/System/Library/Frameworks/WebKit.framework/Versions/A/WebKit
clang -dynamiclib -x c /dev/null -o "$version_dir/Zebkit" -arch "$(uname -m)" -mmacosx-version-min="$minimum_system" \
    -install_name @rpath/Zebkit.framework/Versions/A/Zebkit \
    -F"$frameworks" -Wl,-reexport_framework,WebKit || die "cannot link Zebkit.framework"
install_name_tool -change "$system_name" @loader_path/Frameworks/WebKit.framework/Versions/A/WebKit "$version_dir/Zebkit" 2> /dev/null
otool -L "$version_dir/Zebkit" | grep -q '@loader_path/Frameworks/WebKit.framework' || die "Zebkit.framework does not name the engine inside it"
sed -e "s/@VERSION@/$(tr -d '[:space:]' < "$ZEBKIT_ROOT/VERSION")/" -e "s/@MINIMUM_SYSTEM@/$minimum_system/" \
    "$ZEBKIT_ROOT/src/framework/Info.plist" > "$version_dir/Resources/Info.plist"
ln -s A "$umbrella/Versions/Current"
for name in Zebkit Headers Resources Frameworks; do
    ln -s "Versions/Current/$name" "$umbrella/$name"
done

# Strip, then sign again: a changed file no longer matches its signature,
# and macOS does not run unsigned code. The services keep what they are
# entitled to (the JIT, among other things).
binaries=()
while IFS= read -r binary; do
    binaries+=("$binary")
done < <(find "$package" -type f -perm +111 ! -path '*/Resources/*' -exec sh -c 'file -b "$1" | grep -q "^Mach-O"' _ {} \; -print)
if [[ "${PACKAGE_STRIP:-1}" != 0 ]]; then
    for binary in "${binaries[@]}"; do
        strip -x "$binary" 2> /dev/null
    done
fi
# From the inside out: what holds signed code is signed after it.
while IFS= read -r helper; do
    codesign --force --sign - "$helper" 2> /dev/null || die "cannot sign $helper"
done < <(find "$frameworks" -type f -path '*/Helpers/*' -perm +111)
for item in "$frameworks"/*.dylib "$frameworks"/*.framework; do
    codesign --force --sign - "$item" 2> /dev/null || die "cannot sign $item"
done
for service in "$frameworks"/*.xpc; do
    codesign --force --sign - --preserve-metadata=entitlements "$service" 2> /dev/null || die "cannot sign $service"
done
codesign --force --sign - "$umbrella" 2> /dev/null || die "cannot sign $umbrella"
codesign --verify --deep --strict "$umbrella" 2> /dev/null || die "the signature of $umbrella does not hold"

# The size is what runs: headers are for building against it.
size="$(find "$package" -type f ! -path '*/Headers/*' ! -path '*/Modules/*' ! -path "$package/include/*" -exec stat -f %z {} + | awk '{ total += $1 } END { printf "%.1f MiB", total / 1048576 }')"
log "package: $package ($size, ${#binaries[@]} binaries)"
log "expects from the system: macOS $minimum_system or newer"
