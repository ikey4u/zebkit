#!/usr/bin/env bash
# Set up the Linux machine to build zebkit for macOS (profiles/slim-macos.gn,
# slim-macos-intel.gn), as Chromium's own builders compile for macOS on Linux.
#
#   scripts/macos-toolchain.sh
#
# What only a Mac has is Apple's SDK, the headers and library stubs the
# engine compiles and links against, inside a copy of Xcode, of which
# Chromium reads the version. It comes from zsdk (scripts/zsdk.sh), at the
# version upstream.lock pins; it has both processors' libraries, for Apple
# silicon and Intel.
#
# The rest comes from where Chromium's DEPS says, at the versions it pins,
# for a checkout that builds macOS on Linux: the Mach interface generator
# for Linux, clang's runtime library for macOS, and ANGLE's Metal shaders.
# They go under $SDK_DIR/macos (scripts/lib.sh). All of it is linked into the
# tree; scripts/fetch-upstream.sh links it again after a sync.

source "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

[[ -f "$SRC_DIR/DEPS" ]] || die "no upstream tree; run scripts/fetch-upstream.sh first"
"$ZEBKIT_ROOT/scripts/zsdk.sh" macos
grep -q "mac_sdk_official_version = \"$ZSDK_MACOS_VERSION\"" "$PROFILES_DIR/slim-macos.gn" \
    || log "note: profiles/slim-macos.gn names another SDK version than upstream.lock's $ZSDK_MACOS_VERSION"

macos="$SDK_DIR/macos"
mkdir -p "$macos"

# What DEPS pins, read from DEPS itself.
pins="$(cd "$SRC_DIR" && python3 -I -c '
g = {"Var": lambda name: "{%s}" % name, "Str": str}
exec(open("DEPS").read(), g)
deps = g["deps"]
for name, path in (("mig", "src/third_party/mig/bin"), ("angle-metal", "src/ui/gl/resources/angle-metal")):
    package = deps[path]["packages"][0]
    print("cipd", name, package["package"].replace("${{platform}}", "linux-amd64"), package["version"])
for item in deps["src/third_party/llvm-build/Release+Asserts"]["objects"]:
    if "mac-runtime-library" in item["object_name"]:
        print("gcs", item["object_name"], item["sha256sum"])
')"

ensure="$macos/cipd.ensure"
: > "$ensure"
while read -r kind a b c; do
    case "$kind" in
        cipd) printf '@Subdir %s\n%s %s\n' "$a" "$b" "$c" >> "$ensure" ;;
        gcs)
            # Fetched again only when DEPS names another object; unpacked beside,
            # then renamed.
            [[ "$(cat "$macos/runtime.object" 2> /dev/null)" == "$a $b" ]] && continue
            log "fetching $a"
            curl -sSfLo "$macos/runtime.tar.xz" "https://commondatastorage.googleapis.com/chromium-browser-clang/$a"
            [[ "$(sha256 "$macos/runtime.tar.xz" | cut -d' ' -f1)" == "$b" ]] || die "$a: wrong SHA-256"
            rm -rf "$macos/runtime.new"
            mkdir -p "$macos/runtime.new"
            tar -xJf "$macos/runtime.tar.xz" -C "$macos/runtime.new"
            rm -rf "$macos/runtime"
            mv "$macos/runtime.new" "$macos/runtime"
            echo "$a $b" > "$macos/runtime.object"
            rm "$macos/runtime.tar.xz"
            ;;
    esac
done <<< "$pins"
log "fetching the Mach interface generator and ANGLE's Metal shaders"
cipd ensure -root "$macos/cipd" -ensure-file "$ensure" >/dev/null

[[ -n "${ZEBKIT_SDK_NO_LINK:-}" ]] || "$ZEBKIT_ROOT/scripts/fetch-upstream.sh"
log "ready to build profiles/slim-macos.gn"
