#!/usr/bin/env bash
# Measure what a profile ships and write docs/reports/<profile>.md.
#
# The number that counts is the size of the package directory that
# scripts/package.sh assembles, against Electron's trimmed size, the baseline
# (docs/reports/electron.md, from scripts/electron-baseline.sh); its size
# compressed, as a download takes it, is reported beside it. The report
# also lists what the package still expects from the system, since a small
# package that needs a long list of system libraries would not be the same
# achievement.

source "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

# Electron trimmed, the baseline, as scripts/electron-baseline.sh measured the
# release upstream.lock pins (the 150 MiB limit gave way to speed on 2026-10-21).
electron_report="$REPORTS_DIR/electron.md"
grep -q "^- Electron $ELECTRON_VERSION " "$electron_report" 2> /dev/null \
    || die "$electron_report is not of Electron $ELECTRON_VERSION: run scripts/electron-baseline.sh"
goal_mib="$(sed -n 's/^- \*\*Trimmed .*: \([0-9.]*\) MiB\*\*$/\1/p' "$electron_report")"
[[ -n "$goal_mib" ]] || die "no trimmed size in $electron_report"
GOAL_BYTES="$(awk -v m="$goal_mib" 'BEGIN { printf "%d", m * 1048576 }')"

profile="${1:-}"
require_profile "$profile"

package_log="$("$ZEBKIT_ROOT/scripts/package.sh" "$profile" 2>&1)" || { echo "$package_log" >&2; exit 1; }
package="$OUT_DIR/$profile/package"
report="$REPORTS_DIR/$profile.md"
mkdir -p "$REPORTS_DIR"

mib() { awk -v b="$1" 'BEGIN { printf "%.1f", b / 1048576 }'; }
field() { sed -n "s/^.*==> $1: //p" <<< "$package_log" | sed $'s/\x1b\\[[0-9;]*m//g'; }

total=0 rows=""
while IFS= read -r file; do
    size=$(stat -c %s "$file")
    total=$(( total + size ))
    rows+="$size|\`${file#"$package"/}\`"$'\n'
done < <(find "$package" -type f | sort)

if (( total <= GOAL_BYTES )); then verdict="within"; else verdict="over"; fi
xz_bytes=$(compressed_size xz "$package")
zip_bytes=$(compressed_size zip "$package")

{
    echo "# Size report: \`$profile\`"
    echo
    echo "- Upstream: Chromium $CHROMIUM_VERSION, zebkit's engine library"
    echo "- Measured: $(date -u +%Y-%m-%d) on $(uname -sm), from the package directory \`scripts/package.sh\` assembles"
    echo "- **Total: $(mib "$total") MiB** ($verdict the $(mib "$GOAL_BYTES") MiB of Electron $ELECTRON_VERSION trimmed, the baseline)"
    echo "- Compressed: $(mib "$xz_bytes") MiB as \`.tar.xz\` (xz -6, as releases ship), $(mib "$zip_bytes") MiB as \`.zip\` (deflate -9, as Electron ships)"
    echo "- Oldest glibc it can run on: $(field 'oldest usable glibc')"
    echo "- Expects from the system: $(field 'expects from the system')"
    echo
    echo "| File | MiB |"
    echo "|---|---:|"
    sort -t'|' -k1,1nr <<< "$rows" | awk -F'|' '$1 != "" { printf "| %s | %.1f |\n", $2, $1 / 1048576 }'
} > "$report"

log "$(mib "$total") MiB ($verdict Electron's $(mib "$GOAL_BYTES") MiB)"
log "wrote $report"
