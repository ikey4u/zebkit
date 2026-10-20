#!/usr/bin/env bash
# Measure Electron, the size zebkit has to beat, and write
# docs/reports/electron.md.
#
#   scripts/electron-baseline.sh             # the baseline, pinned in upstream.lock
#   scripts/electron-baseline.sh 45.0.0-beta.5   # another release beside it
#
# The application is examples/electron-hello, a window with one page,
# packaged the way electron-packager does it: Electron's prebuilt runtime
# (ELECTRON_VERSION in upstream.lock) with the application in
# resources/app. It is run once under a virtual display to show that it
# works, and then counted the way scripts/measure.sh counts a zebkit
# package: every file in the directory, uncompressed.
#
# Two numbers: Electron as it comes, and Electron trimmed as far as it goes
# without rebuilding it, with one locale (en-US) and no license file. Each
# also compressed, as a download takes it.

source "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

version="${1:-$ELECTRON_VERSION}"
zip_name="electron-v$version-linux-x64.zip"
zip="$CACHE_DIR/electron/$zip_name"
release_url="https://github.com/electron/electron/releases/download/v$version"
if [[ "$version" == "$ELECTRON_VERSION" ]]; then
    package="$OUT_DIR/electron/package"
    report="$REPORTS_DIR/electron.md"
    want_sha256="$ELECTRON_LINUX_X64_SHA256"
else
    # Not pinned: checked against the checksums published with the release.
    package="$OUT_DIR/electron/$version/package"
    report="$REPORTS_DIR/electron-$version.md"
    want_sha256="$(curl -fsSL --retry 5 --retry-all-errors "$release_url/SHASUMS256.txt" | awk -v f="*$zip_name" '$2 == f || $2 == substr(f, 2) { print $1 }')"
    [[ -n "$want_sha256" ]] || die "no checksum for $zip_name in the release's SHASUMS256.txt"
fi

if [[ ! -f "$zip" ]] || ! sha256 "$zip" | grep -q "^$want_sha256 "; then
    log "downloading Electron $version"
    mkdir -p "$(dirname "$zip")"
    curl -fsSL --retry 5 --retry-all-errors -C - -o "$zip.part" "$release_url/$zip_name"
    sha256 "$zip.part" | grep -q "^$want_sha256 " || die "$zip_name does not match its SHA-256"
    mv "$zip.part" "$zip"
fi

rm -rf "$package"
mkdir -p "$package"
unzip -q "$zip" -d "$package"
# Electron's own sample application, which electron-packager removes.
rm "$package/resources/default_app.asar"
mkdir -p "$package/resources/app"
cp "$ZEBKIT_ROOT"/examples/electron-hello/{package.json,main.js,index.html} "$package/resources/app/"

# The check runs without the sandbox, whose helper has to be set-user-ID
# root to work; it is in the package and counted all the same.
log "running the application"
output="$(xvfb-run -a "$package/electron" --no-sandbox "$package/resources/app" --check 2>/dev/null)" || true
grep -qx 'hello: Hello, world' <<< "$output" || die "the application did not show its page; it printed: $output"

mib() { awk -v b="$1" 'BEGIN { printf "%.1f", b / 1048576 }'; }
bytes() { find "$@" -type f -printf '%s\n' | awk '{ s += $1 } END { print s + 0 }'; }

total=$(bytes "$package")
locales=$(bytes "$package/locales")
en_us=$(stat -c %s "$package/locales/en-US.pak")
licenses=$(stat -c %s "$package/LICENSES.chromium.html")
trimmed=$(( total - locales + en_us - licenses ))
locale_count=$(find "$package/locales" -name '*.pak' | wc -l)

# Compressed, as shipped and trimmed.
log "compressing"
trimmed_dir="$(dirname "$package")/trimmed"
rm -rf "$trimmed_dir"
cp -a "$package" "$trimmed_dir"
find "$trimmed_dir/locales" -name '*.pak' ! -name en-US.pak -delete
rm "$trimmed_dir/LICENSES.chromium.html"
(( $(bytes "$trimmed_dir") == trimmed )) || die "the trimmed copy is not the size counted for it"
total_xz=$(compressed_size xz "$package") total_zip=$(compressed_size zip "$package")
trimmed_xz=$(compressed_size xz "$trimmed_dir") trimmed_zip=$(compressed_size zip "$trimmed_dir")

{
    echo "# Size report: Electron"
    echo
    echo "- Electron $version (Chromium $(strings "$package/electron" | grep -m1 -oE '^Chrome/[0-9.]+' | cut -d/ -f2), Node.js included), Linux x86-64"
    echo "- Application: \`examples/electron-hello\`, a window with one page, in \`resources/app\`"
    echo "- Measured: $(date -u +%Y-%m-%d) by \`scripts/electron-baseline.sh\`, every file in the package, uncompressed"
    echo "- **As Electron ships: $(mib "$total") MiB**"
    echo "- **Trimmed without rebuilding (only \`en-US.pak\` of $locale_count locales, no \`LICENSES.chromium.html\`): $(mib "$trimmed") MiB**"
    echo
    echo "| | Uncompressed | \`.tar.xz\` (xz -6) | \`.zip\` (deflate -9) |"
    echo "|---|---:|---:|---:|"
    echo "| As Electron ships | $(mib "$total") | $(mib "$total_xz") | $(mib "$total_zip") |"
    echo "| Trimmed | $(mib "$trimmed") | $(mib "$trimmed_xz") | $(mib "$trimmed_zip") |"
    echo
    echo "In MiB. The \`.tar.xz\` is compressed as zebkit's releases are (\`scripts/release.sh\`)."
    echo
    echo "| File | MiB |"
    echo "|---|---:|"
    echo "| \`locales/\` ($locale_count files) | $(mib "$locales") |"
    find "$package" -path "$package/locales" -prune -o -type f -printf '%s|%P\n' | sort -t'|' -k1,1nr \
        | awk -F'|' '$1 >= 104858 { printf "| `%s` | %.1f |\n", $2, $1 / 1048576 } $1 < 104858 { rest += $1; n++ }
                     END { if (n) printf "| %d smaller files | %.1f |\n", n, rest / 1048576 }'
} > "$report"

log "Electron $version: $(mib "$total") MiB as shipped, $(mib "$trimmed") MiB trimmed, $(mib "$trimmed_xz") MiB trimmed as .tar.xz"
log "wrote $report"
