#!/usr/bin/env bash
# Assemble what a profile ships on macOS into one relocatable directory:
# out/<profile>/package/. This is what scripts/package.sh runs on a Mac.
#
#   scripts/package.sh <profile>
#
# The directory holds the engine's frameworks and libraries, the services
# they start (the web, network and GPU processes), and include/ with
# zebkit's own headers:
#
#   WebKit.framework  WebCore.framework  JavaScriptCore.framework  ...
#   libwebrtc.dylib  libANGLE-shared.dylib
#   com.apple.WebKit.WebContent.xpc  ...
#   include/
#
# It works wherever it is copied. The services look for the frameworks in
# the directory they are in themselves, which WebKit's build arranges. A
# program that uses the engine has to be told the same, or it gets the
# system's WebKit; it is linked with
#
#   -F<directory> -framework WebKit -Wl,-dyld_env,DYLD_FRAMEWORK_PATH=@executable_path/<path to the directory>
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
rm -rf "$package"
mkdir -p "$package/include"

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
cp -R "$products_dir/WebKit.framework" "$package/"
added=1
while (( added )); do
    added=0
    while IFS= read -r binary; do
        while IFS= read -r dependency; do
            name="$(is_ours "$dependency")"
            [[ -n "$name" && ! -e "$package/$name" ]] || continue
            cp -R "$products_dir/$name" "$package/"
            added=1
        done < <(otool -L "$binary" | awk 'NR > 1 { print $1 }')
    done < <(find "$package" -type f \( -name '*.dylib' -o -path '*.framework/Versions/A/*' \) ! -path '*/Resources/*' ! -name '*.plist' -perm +111)
done
# The services are started by name, not linked.
for service in "$products_dir"/com.apple.WebKit.*.xpc; do
    cp -R "$service" "$package/"
done

# Headers are for building against the engine. WebKit's are the API, with
# JavaScriptCore's, which they include; the other frameworks' are WebKit's
# own business.
find "$package" -type d \( -name PrivateHeaders -o -name '*.dSYM' \) -prune -exec rm -rf {} +
for framework in "$package"/*.framework; do
    case "$(basename "$framework")" in WebKit.framework|JavaScriptCore.framework) continue ;; esac
    rm -rf "$framework/Versions/A/Headers" "$framework/Headers" "$framework/Versions/A/Modules" "$framework/Modules"
done
# Links to what was just taken out.
find "$package" -type l ! -exec test -e {} \; -delete
engine="$package/WebKit.framework/Versions/A/WebKit"
exported="$(nm -gU "$engine")"
[[ "$exported" == *" _zebkit_cdp_listen"* ]] && cp "$ZEBKIT_ROOT/src/cdp/zebkit-cdp.h" "$package/include/"
[[ "$exported" == *" _zebkit_view_new"* ]] && cp "$ZEBKIT_ROOT/src/view/zebkit-view.h" "$package/include/"
if [[ "$exported" == *" _zebkit_vendor_"* ]]; then
    cp -R "$OUT_DIR/deps/vendor/include/zebkit" "$package/include/"
fi

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
for item in "$package"/*.dylib "$package"/*.framework; do
    codesign --force --sign - "$item" 2> /dev/null || die "cannot sign $item"
done
for service in "$package"/*.xpc; do
    codesign --force --sign - --preserve-metadata=entitlements "$service" 2> /dev/null || die "cannot sign $service"
done

# The size is what runs: headers are for building against it.
size="$(find "$package" -type f ! -path '*/Headers/*' ! -path '*/Modules/*' ! -path "$package/include/*" -exec stat -f %z {} + | awk '{ total += $1 } END { printf "%.1f MiB", total / 1048576 }')"
log "package: $package ($size, ${#binaries[@]} binaries)"
log "expects from the system: macOS $(otool -l "$engine" | awk '/LC_BUILD_VERSION/ { found = 1 } found && /minos/ { print $2; exit }') or newer"
