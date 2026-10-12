#!/usr/bin/env bash
# Measure what a profile ships and write docs/reports/<profile>.md.
#
# The number that counts is the size of the package directory that
# scripts/package.sh assembles: the stripped engine plus every library a base
# Linux system does not provide. That total is checked against the 60 MiB
# budget. The report also lists what the package still expects from the
# system, since a small package that needs a long list of system libraries
# would not be the same achievement.
#
# On macOS the report is docs/reports/macos/<profile>.md, and lists the
# package by what is in it: each framework, library and helper process,
# with how much of it is code and how much the resources beside it. The
# budget was set for Linux and is not applied there.

source "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

BUDGET_BYTES=$(( 60 * 1024 * 1024 ))

profile="${1:-}"
require_profile "$profile"

package_log="$("$ZEBKIT_ROOT/scripts/package.sh" "$profile" 2>&1)" || { echo "$package_log" >&2; exit 1; }
package="$OUT_DIR/$profile/package"
report="$REPORTS_DIR/$profile.md"
mkdir -p "$REPORTS_DIR"

mib() { awk -v b="$1" 'BEGIN { printf "%.1f", b / 1048576 }'; }
field() { sed -n "s/^.*==> $1: //p" <<< "$package_log" | sed $'s/\x1b\\[[0-9;]*m//g'; }

if [[ "$ZEBKIT_HOST" == macos ]]; then
    # One row for each thing in the package: its Mach-O files, and the rest.
    rows="$(find "$package" -type f ! -path "$package/include/*" ! -path '*/Headers/*' ! -path '*/Modules/*' \
        -exec sh -c 'case "$(file -b "$1")" in Mach-O*) kind=code ;; *) kind=other ;; esac; echo "$kind $(stat -f %z "$1") $1"' _ {} \; \
        | awk -v root="$package/" '{ path = substr($3, length(root) + 1); sub(/\/.*/, "", path); if ($1 == "code") code[path] += $2; else other[path] += $2; seen[path] = 1 }
            END { for (item in seen) printf "%d %d %s\n", code[item], other[item], item }' | sort -rn)"
    code_total="$(awk '{ total += $1 } END { print total + 0 }' <<< "$rows")"
    other_total="$(awk '{ total += $2 } END { print total + 0 }' <<< "$rows")"
    {
        echo "# Size report: \`$profile\` on macOS"
        echo
        echo "- Upstream: WebKit's Mac port at $UPSTREAM_NAME $UPSTREAM_VERSION"
        echo "- Measured: $(date -u +%Y-%m-%d) on $(uname -sm), from the package directory \`scripts/package.sh\` assembles"
        echo "- **Total: $(mib $(( code_total + other_total ))) MiB**: $(mib "$code_total") of code and $(mib "$other_total") of resources"
        echo "- Expects from the system: $(field 'expects from the system')"
        echo
        echo "Headers are for building against the engine and are not counted."
        echo
        echo "| In the package | Code, MiB | Resources, MiB |"
        echo "|---|---:|---:|"
        awk '{ printf "| `%s` | %.1f | %.1f |\n", $3, $1 / 1048576, $2 / 1048576 }' <<< "$rows"
    } > "$report"
    log "$(mib "$code_total") MiB of code + $(mib "$other_total") MiB of resources = $(mib $(( code_total + other_total ))) MiB"
    log "wrote $report"
    exit 0
fi

total=0 engine_total=0 other_total=0
engine_rows="" other_rows=""
while IFS= read -r file; do
    name="${file#"$package"/}"
    size=$(stat -c %s "$file")
    total=$(( total + size ))
    if [[ "$name" == libzebkit* || "$name" == zebkit-* ]]; then
        engine_total=$(( engine_total + size ))
        engine_rows+="| \`$name\` | $(mib "$size") |"$'\n'
    else
        other_total=$(( other_total + size ))
        other_rows+="$size|\`$name\`"$'\n'
    fi
# Headers are for building an application; they are not shipped with one.
done < <(find "$package" -type f -not -path "$package/include/*" | sort)
other_count=$(grep -c . <<< "$other_rows" || true)

if (( total <= BUDGET_BYTES )); then verdict="within"; else verdict="over"; fi

{
    echo "# Size report: \`$profile\`"
    echo
    echo "- Upstream: $UPSTREAM_NAME $UPSTREAM_VERSION"
    echo "- Measured: $(date -u +%Y-%m-%d) on $(uname -sm), from the package directory \`scripts/package.sh\` assembles"
    echo "- **Total: $(mib "$total") MiB** ($verdict the $(mib "$BUDGET_BYTES") MiB budget)"
    echo "- Oldest glibc it can run on: $(field 'oldest usable glibc')"
    echo "- Expects from the system: $(field 'expects from the system')"
    echo
    echo "## Engine: $(mib "$engine_total") MiB"
    echo
    echo "| File | MiB |"
    echo "|---|---:|"
    printf '%s' "$engine_rows"
    echo
    echo "## Bundled libraries: $(mib "$other_total") MiB in $other_count files"
    echo
    echo "Files under 0.2 MiB are summed into the last row. Libraries linked"
    echo "statically are inside the engine and counted there."
    echo
    echo "| File | MiB |"
    echo "|---|---:|"
    sort -t'|' -k1,1nr <<< "$other_rows" | awk -F'|' '
        $1 == "" { next }
        $1 >= 209715 { printf "| %s | %.1f |\n", $2, $1 / 1048576; next }
        { small += $1; n++ }
        END { if (n) printf "| %d smaller files | %.1f |\n", n, small / 1048576 }'
} > "$report"

log "engine $(mib "$engine_total") MiB + bundled $(mib "$other_total") MiB = $(mib "$total") MiB ($verdict budget)"
log "wrote $report"
