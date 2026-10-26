#!/usr/bin/env bash
# Get the pinned Chromium release into upstream/chromium/src, with what it
# depends on (V8, Skia, the compiler and sysroot it builds with), and apply
# patches/series on top. Re-running is a no-op unless the pin or the series
# changed.
#
# The tree is fetched without history: about 30 GB instead of over 100.
# gclient syncs only when the pinned version changes. A sync of a tree that
# is already there fetches every branch's history, so a change of patches
# undoes the previous ones file by file instead.

source "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

checkout_os=linux
checkout_pgo=True
if [[ "$ZEBKIT_HOST" == macos ]]; then
    checkout_os=mac
    checkout_pgo=False
    # Use the installed full Xcode without changing xcode-select globally.
    if [[ -z "${DEVELOPER_DIR:-}" && -d /Applications/Xcode.app/Contents/Developer ]]; then
        export DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer
    fi
fi

stamp="$GCLIENT_DIR/.zebkit-stamp"
synced="$GCLIENT_DIR/.zebkit-synced"
applied="$GCLIENT_DIR/.zebkit-applied"
# The patches applied, in order, each with its SHA-256: when the series
# only grows at its end, the new patches are applied on top, without
# undoing the others (which would give every patched file a new time, and
# the build hours of work).
applied_series="$GCLIENT_DIR/.zebkit-applied-series"
series="$PATCHES_DIR/series"

patch_list() { [[ -f "$series" ]] && sed -e 's/#.*//' -e '/^[[:space:]]*$/d' "$series" || true; }
series_now() { patch_list | while read -r p; do echo "$p $(sha256 "$PATCHES_DIR/$p" | cut -d' ' -f1)"; done; }
patches_hash="$(patch_list | while read -r p; do sha256 "$PATCHES_DIR/$p"; done | sha256 | cut -d' ' -f1)"
want="$CHROMIUM_VERSION $DEPOT_TOOLS_COMMIT $patches_hash $checkout_os"
# Before native Mac builds the stamp had no OS field; those checkouts
# always fetched Linux dependencies. Preserve their incremental path.
recorded_os="$(cut -s -d' ' -f4 "$stamp" 2>/dev/null || true)"
recorded_os="${recorded_os:-linux}"

# zebkit's own engine code is not a patch: src/engine is linked into the
# tree as //zebkit/engine, so that what is built is what is in the
# repository.
link_engine() {
    mkdir -p "$SRC_DIR/zebkit"
    ln -sfn "$ZEBKIT_ROOT/src/engine" "$SRC_DIR/zebkit/engine"
    # zebkit's configuration of FFmpeg (scripts/ffmpeg-config.sh).
    ln -sfn "$ZEBKIT_ROOT/src/ffmpeg" "$SRC_DIR/zebkit/ffmpeg"
    # What scripts/macos-toolchain.sh set up for building macOS on Linux:
    # Apple's SDK in a copy of Xcode, and what DEPS fetches for a macOS
    # checkout.
    local macos="$CACHE_DIR/macos"
    if [[ "$ZEBKIT_HOST" == linux && -d "$macos/xcode" ]]; then
        mkdir -p "$SRC_DIR/build/mac_files"
        ln -sfn "$macos/xcode" "$SRC_DIR/build/mac_files/xcode_binaries"
        ln -sfn "$macos/cipd/mig" "$SRC_DIR/third_party/mig/bin"
        ln -sfn "$macos/cipd/angle-metal" "$SRC_DIR/ui/gl/resources/angle-metal"
        # With their times, and only what is not there yet: a compiler's
        # runtime with new times is a new toolchain to the build, which
        # then compiles the whole macOS engine again.
        cp -a -u "$macos/runtime/." "$SRC_DIR/third_party/llvm-build/Release+Asserts/"
    fi
}

apply_patch() {
    log "applying $1"
    patch -d "$SRC_DIR" -p1 --no-backup-if-mismatch < "$PATCHES_DIR/$1"
    awk '/^--- / { created = ($2 == "/dev/null"); next }
         /^\+\+\+ / { file = $2; sub(/^b\//, "", file); print (created ? "new " : "changed ") file }' \
        "$PATCHES_DIR/$1" >> "$applied"
    echo "$1 $(sha256 "$PATCHES_DIR/$1" | cut -d' ' -f1)" >> "$applied_series"
}

if [[ -f "$stamp" && "$(cat "$stamp")" == "$want" ]]; then
    # A tree patched before the record of the series was kept.
    [[ -f "$applied_series" ]] || series_now > "$applied_series"
    link_engine
    log "upstream tree is up to date: $SRC_DIR"
    exit 0
fi

# The same Chromium and depot_tools, and the series applied so far is the
# start of the series now: only the patches after it are applied.
if [[ -f "$stamp" && -f "$applied_series" && -f "$applied" ]] \
    && [[ "$(cut -d' ' -f1-2 "$stamp")" == "$CHROMIUM_VERSION $DEPOT_TOOLS_COMMIT" ]] \
    && [[ "$recorded_os" == "$checkout_os" ]]; then
    have="$(cat "$applied_series")"
    now="$(series_now)"
    count="$(printf '%s\n' "$have" | grep -c .)"
    if [[ -n "$have" && "$(printf '%s\n' "$now" | head -n "$count")" == "$have" ]]; then
        rm -f "$stamp"
        printf '%s\n' "$now" | tail -n +"$((count + 1))" | while read -r p _; do apply_patch "$p"; done
        link_engine
        echo "$want" > "$stamp"
        log "upstream tree ready, the new patches applied on top: $SRC_DIR"
        exit 0
    fi
fi
rm -f "$stamp"

# depot_tools, at its pin. It bootstraps its own Python and CIPD on first use.
if [[ ! -d "$DEPOT_TOOLS_DIR/.git" ]]; then
    log "cloning depot_tools"
    git clone -q "$DEPOT_TOOLS_URL" "$DEPOT_TOOLS_DIR"
fi
if [[ "$(git -C "$DEPOT_TOOLS_DIR" rev-parse HEAD)" != "$DEPOT_TOOLS_COMMIT" ]]; then
    git -C "$DEPOT_TOOLS_DIR" cat-file -e "$DEPOT_TOOLS_COMMIT^{commit}" 2>/dev/null \
        || git -C "$DEPOT_TOOLS_DIR" fetch -q origin
    git -C "$DEPOT_TOOLS_DIR" checkout -q "$DEPOT_TOOLS_COMMIT"
fi
"$DEPOT_TOOLS_DIR/ensure_bootstrap"

mkdir -p "$GCLIENT_DIR"
# Only the operating systems built here. NaCl is gone upstream; the PGO
# profiles are measured against size before a profile uses them.
cat > "$GCLIENT_DIR/.gclient" <<EOF
solutions = [{
    "name": "src",
    "url": "$CHROMIUM_URL@refs/tags/$CHROMIUM_VERSION",
    "managed": False,
    "custom_deps": {},
    # Pre-builds the Python environment of Chromium's test runners (numpy,
    # pandas, opencv), which the build does not use. Its installer also
    # unpacks into /tmp whatever TMPDIR says.
    "custom_hooks": [{"name": "vpython3_common"}],
    "custom_vars": {
        "checkout_nacl": False,
        "checkout_pgo_profiles": $checkout_pgo,
    },
}]
target_os = ["$checkout_os"]
EOF

# Undo the previous series: files it created are removed, files it changed
# are restored from the repository they belong to (src/ or one inside it).
if [[ -f "$applied" ]]; then
    while read -r kind file; do
        if [[ "$kind" == new ]]; then
            rm -f "$SRC_DIR/$file"
        else
            git -C "$(dirname "$SRC_DIR/$file")" checkout -q HEAD -- "$(basename "$file")"
        fi
    done < "$applied"
    rm -f "$applied"
fi
rm -f "$applied_series"

recorded_sync="$(cat "$synced" 2>/dev/null || true)"
if [[ "$recorded_sync" != "$CHROMIUM_VERSION $checkout_os" ]] \
    && ! [[ "$checkout_os" == linux && "$recorded_sync" == "$CHROMIUM_VERSION" ]]; then
    log "syncing Chromium $CHROMIUM_VERSION into $SRC_DIR"
    (cd "$GCLIENT_DIR" && gclient sync --no-history --nohooks --jobs 16 \
        --revision "src@refs/tags/$CHROMIUM_VERSION")
fi
echo "$CHROMIUM_VERSION $checkout_os" > "$synced"
log "running Chromium's hooks"
(cd "$GCLIENT_DIR" && gclient runhooks --jobs 16)

# Patch paths are relative to src/ and may reach into its other
# repositories (v8/, third_party/skia/ and the like).
patch_list | while read -r p; do apply_patch "$p"; done

link_engine
echo "$want" > "$stamp"
log "upstream tree ready: $SRC_DIR"
