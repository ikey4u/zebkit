#!/usr/bin/env bash
# Generate zebkit's configuration of FFmpeg into src/ffmpeg, which the
# engine builds with (zebkit_ffmpeg = true, patches/1017).
#
#   scripts/ffmpeg-config.sh [--no-build]
#
# --no-build takes FFmpeg's builds already in the tree, from a run before;
# FFMPEG_ONLY=<os> builds that system's and takes the others from a run before.
#
# Chromium's FFmpeg has what a browser plays. Applications need more of it
# (audio resampled for Whisper, frames scaled, files remuxed, VP8 and
# VP9 frames), so zebkit's adds, to Chrome's configuration:
#
#   libswresample and libswscale, the file protocol, the MP4, MOV, Matroska
#   and WebM muxers, and VP8 and VP9 decoding through the engine's libvpx
#   (FFmpeg's wrapper is 1.1 MiB smaller than its own VP9 decoder)
#
# The work is Chromium's own tools for FFmpeg (media/ffmpeg/scripts): FFmpeg's
# configure and make for each platform zebkit builds (Linux x86-64, macOS
# arm64 and Windows x64 from Linux), then generate_gn.py, which finds the sources of the
# objects built. That writes into the FFmpeg tree it reads, so it runs on a
# copy. Run again when Chromium's FFmpeg changes (a new CHROMIUM_VERSION).
#
# Windows needs the Windows SDK (scripts/windows-toolchain.sh). Without it
# Windows is not built: FFmpeg's Windows build from a run before is taken if
# the tree has one; if not, only the other systems' configurations are
# written, and the source list, which needs every system, stays as it is.

source "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

FLAGS=(
    --disable-unstable
    --enable-swresample
    --enable-swscale
    --enable-protocol=file
    --enable-muxer=mp4,mov,matroska,webm,ipod
    --enable-libvpx
    --enable-decoder=libvpx_vp8,libvpx_vp9
    "--extra-cflags=-I$SRC_DIR/third_party/libvpx/source/libvpx"
)
PLATFORMS=("linux x64" "mac arm64" "win x64")

ffmpeg="$SRC_DIR/third_party/ffmpeg"
scripts="$SRC_DIR/media/ffmpeg/scripts"
out="$ZEBKIT_ROOT/src/ffmpeg"
work="$CACHE_DIR/ffmpeg-gen"
[[ -d "$ffmpeg" ]] || die "no upstream tree; run scripts/fetch-upstream.sh first"
[[ -d "$SRC_DIR/build/mac_files/xcode_binaries" ]] || die "no macOS SDK; run scripts/macos-toolchain.sh first"
export PATH="$SRC_DIR/third_party/llvm-build/Release+Asserts/bin:$PATH"

# Whether FFmpeg can be built for Windows here, and whether a run before did.
windows_sdk=0
[[ -f "$SDK_DIR/windows/win_toolchain.json" ]] && windows_sdk=1
windows_built=0
[[ -d "$ffmpeg/build.x64.win/Chrome" ]] && windows_built=1
if (( ! windows_sdk )); then
    [[ "${FFMPEG_ONLY:-}" == win ]] && die "FFMPEG_ONLY=win needs the Windows SDK; run scripts/windows-toolchain.sh first"
    if (( windows_built )); then
        log "no Windows SDK: skipping FFmpeg's build for Windows, taking the one of a run before (scripts/windows-toolchain.sh sets the SDK up)"
    else
        log "no Windows SDK and no build of a run before: skipping Windows; src/ffmpeg/ffmpeg_generated.gni and its renamed files stay as they are, only the other systems' configurations are written (scripts/windows-toolchain.sh sets the SDK up)"
        PLATFORMS=("linux x64" "mac arm64")
    fi
fi

# configure checks that libvpx links, which the engine's does in the end:
# a library with the two entry points does for the check.
stub_libvpx() {
    local os="$1" dir="$CACHE_DIR/ffmpeg-vpx-stub/$1"
    local target=x86_64-unknown-linux-gnu
    [[ "$os" == mac ]] && target=arm64-apple-macos13
    [[ "$os" == win ]] && target=x86_64-pc-windows-msvc
    mkdir -p "$dir"
    printf 'void *vpx_codec_vp8_dx(void) { return 0; }\nvoid *vpx_codec_vp9_dx(void) { return 0; }\n' > "$dir/stub.c"
    clang --target="$target" -c "$dir/stub.c" -o "$dir/stub.o"
    rm -f "$dir/libvpx.a" "$dir/vpx.lib"
    llvm-ar rcs "$dir/libvpx.a" "$dir/stub.o"
    # MSVC's naming, for link.exe's -lvpx.
    [[ "$os" == win ]] && cp "$dir/libvpx.a" "$dir/vpx.lib"
    echo "$dir"
}

build=1
[[ "${1:-}" == --no-build ]] && build=0
for platform in "${PLATFORMS[@]}"; do
    (( build )) || break
    [[ -n "${FFMPEG_ONLY:-}" && "${platform%% *}" != "$FFMPEG_ONLY" ]] && continue
    [[ "${platform%% *}" == win ]] && (( ! windows_sdk )) && continue
    log "configuring and building FFmpeg for $platform"
    stub="$(stub_libvpx "${platform%% *}")"
    libpath="-L$stub"
    [[ "${platform%% *}" == win ]] && libpath="/LIBPATH:$stub"
    # shellcheck disable=SC2086
    (cd "$ffmpeg" && python3 "$scripts/build_ffmpeg.py" $platform --branding Chrome -- "${FLAGS[@]}" \
        "--extra-ldflags=$libpath")
done

# Only over every system's builds: a list of two would leave Windows without sources.
regenerate=1
(( windows_sdk || windows_built )) || regenerate=0
log "finding the sources of what was built"
# The copy is laid out as in Chromium's tree, src/third_party/ffmpeg, beside
# the license checker the script runs.
copy="$work/src/third_party/ffmpeg"
rm -rf "$work"
mkdir -p "$work/builds" "$copy"
(cd "$ffmpeg" && tar --exclude='./build.*' --exclude=./.git -cf - .) | tar -xf - -C "$copy"
ln -s "$SRC_DIR/third_party/devscripts" "$work/src/third_party/devscripts"
# The files it renames include their originals as
# "../../third_party/ffmpeg/...", which it then reads: from two directories
# down too.
ln -s .. "$copy/third_party/ffmpeg" 2>/dev/null || { mkdir -p "$copy/third_party"; ln -s .. "$copy/third_party/ffmpeg"; }
for platform in "${PLATFORMS[@]}"; do
    read -r os arch <<< "$platform"
    ln -s "$ffmpeg/build.$arch.$os" "$work/builds/build.$arch.$os"
done
# It writes the files it renames relative to where it runs: in the copy.
(( regenerate )) && (cd "$copy" && python3 -W ignore "$scripts/generate_gn.py" -s "$copy" -b "$work/builds")

# Into a copy, and over $out by content: a file that did not change keeps its
# time, and a build going on elsewhere does not see it change.
final="$out"
out="$work/out"
log "writing $final"
mkdir -p "$out"
cp -a "$final/ffmpeg_generated.gni" "$out/" 2>/dev/null || true
for platform in "${PLATFORMS[@]}"; do
    read -r os arch <<< "$platform"
    # What copy_config.sh copies for Chromium's configurations.
    from="$ffmpeg/build.$arch.$os/Chrome"
    to="$out/config/$os/$arch"
    mkdir -p "$to"
    for file in config.h config.asm config_components.h config_components.asm \
                libavutil/avconfig.h libavutil/ffversion.h libavcodec/bsf_list.c \
                libavcodec/codec_list.c libavcodec/parser_list.c libavformat/demuxer_list.c \
                libavformat/muxer_list.c libavformat/protocol_list.c; do
        [[ -f "$from/$file" ]] || continue
        mkdir -p "$to/$(dirname "$file")"
        # The configure line names the checkout, and the Windows SDK's place on
        # this machine; neither belongs in the repository.
        sed -E -e "s|$ZEBKIT_ROOT/||g" -e "s#(/winsysroot:?)[^ ']*#\1#g" "$from/$file" > "$to/$file"
    done
done
if (( regenerate )); then
cp "$copy/ffmpeg_generated.gni" "$out/ffmpeg_generated.gni"
# Files generate_gn.py made for sources whose objects would share a name.
(cd "$copy" && find . -name 'autorename_*' -newer "$work/builds" -type f) | while read -r file; do
    mkdir -p "$out/autorename/$(dirname "$file")"
    cp "$copy/$file" "$out/autorename/$file"
done
# The list names them as FFmpeg's own; they are zebkit's.
python3 -I - "$out" <<'PY'
import pathlib, sys
out = pathlib.Path(sys.argv[1])
gni = out / "ffmpeg_generated.gni"
text = gni.read_text()
for path in sorted((out / "autorename").rglob("autorename_*")):
    relative = path.relative_to(out / "autorename").as_posix()
    text = text.replace(f'"{relative}"', f'"//zebkit/ffmpeg/autorename/{relative}"')
# Imported from //zebkit/ffmpeg, where FFmpeg's options are not.
text = text.replace('import("ffmpeg_options.gni")', 'import("//third_party/ffmpeg/ffmpeg_options.gni")')
# generate_gn.py leaves out sdp.c for size, which nothing of Chromium's
# FFmpeg calls; the MOV and MP4 muxers do (its stubs, without RTP).
text += """
# zebkit: sdp.c, which the MOV and MP4 muxers call; Chromium's script leaves
# it out (scripts/ffmpeg-config.sh).
ffmpeg_c_sources += [ "libavformat/sdp.c" ]
"""
gni.write_text(text)
PY
else
    # The list as it is.
    rm -f "$out/ffmpeg_generated.gni"
fi
python3 -I - "$out" "$final" <<'PY'
import pathlib, shutil, sys
new, old = map(pathlib.Path, sys.argv[1:])
names = {p.relative_to(new) for p in new.rglob("*") if p.is_file()}
# A renamed file's first line is the time it was generated.
def text(path):
    return path.read_bytes().split(b"\n", 1)[-1] if "autorename_" in path.name else path.read_bytes()
for name in names:
    target = old / name
    if not target.exists() or text(new / name) != text(target):
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(new / name, target)
PY
log "done: $final"
