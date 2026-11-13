#!/usr/bin/env bash
# Give the checkout a new version: every place a version of zebkit's lives,
# then check_versions (lib.sh) over them.
#
#   scripts/bump-version.sh X.Y.Z    (or X.Y.Z-suffix, a pre-release)
#
# It changes, from the version VERSION holds now to X.Y.Z:
#   - VERSION;
#   - bindings/rust/Cargo.toml's workspace version, and the "=<version>" pins
#     of the crates on one another (bindings/rust/*/Cargo.toml);
#   - the lock files of bindings/rust, tools/zebkit-mcp,
#     examples/native-host/winit and examples/gpui-browser: the entries of
#     zebkit's own crates (from a path, no source line), edited in place;
#   - the versions bindings/rust/README.md and zebkit-build's documentation
#     (src/lib.rs) tell users to put in Cargo.toml, and the README's latest engine;
#   - README.md's instructions: its "Version" line, the archives' names and
#     download links, the Cargo.toml lines and go get's @v<version>;
#   - bindings/python/pyproject.toml, tools/zebkit-mcp/Cargo.toml and the Go
#     binding's BindingVersion (bindings/golang/zebkit.go);
#   - bindings/rust/zebkit-rs/engines.txt: the archives of the old version are
#     dropped, since the crates of X.Y.Z take only lines of X.Y.Z (release.sh
#     writes those);
#   - docs/releases/next.md becomes docs/releases/X.Y.Z.md, titled
#     "zebkit X.Y.Z", without its "**A draft.**" paragraph (the summary of
#     the release is the paragraph after it), and a new next.md is started,
#     "since X.Y.Z". The parts still marked "To fill at release" are listed.
#
# It commits nothing, and stops before changing anything when the tree has
# changes in those files or X.Y.Z is not newer. What still names the old
# version elsewhere (README.md's prose, docs/) is listed at the end: those are
# statements about releases, for a person to read. Then "mise run r" and the
# rest of the release (docs/BUILD.md, "Releases").

source "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

new="${1:-}"
[[ "$new" =~ ^[0-9]+\.[0-9]+\.[0-9]+(-[0-9A-Za-z.]+)?$ ]] \
    || die "usage: $(basename "$0") X.Y.Z   (a version: 0.5.0, 0.5.0-alpha.1)"
(( $# == 1 )) || die "usage: $(basename "$0") X.Y.Z"
old="$(tr -d '[:space:]' < "$ZEBKIT_ROOT/VERSION")"
check_versions
[[ "$new" != "$old" ]] || die "VERSION is $old already"
# Newer: sort -V puts a pre-release after its release, so the suffixes are
# compared apart.
newest="$(printf '%s\n' "${old%%-*}" "${new%%-*}" | sort -V | tail -1)"
if [[ "${new%%-*}" == "${old%%-*}" ]]; then
    [[ "$old" == *-* ]] || die "$new is not newer than $old"
elif [[ "$newest" != "${new%%-*}" ]]; then
    die "$new is not newer than $old"
fi

cd "$ZEBKIT_ROOT"
files=(VERSION README.md bindings/rust/Cargo.toml bindings/rust/README.md bindings/rust/zebkit-build/src/lib.rs
    bindings/rust/zebkit-rs/engines.txt bindings/python/pyproject.toml tools/zebkit-mcp/Cargo.toml
    bindings/golang/zebkit.go docs/releases/next.md)
for manifest in bindings/rust/*/Cargo.toml; do files+=("$manifest"); done
locks=(bindings/rust/Cargo.lock tools/zebkit-mcp/Cargo.lock examples/native-host/winit/Cargo.lock examples/gpui-browser/Cargo.lock)
for lock in "${locks[@]}"; do [[ -f "$lock" ]] && files+=("$lock"); done
[[ -f docs/releases/next.md ]] || die "no docs/releases/next.md: the notes of the release to make"
[[ ! -e "docs/releases/$new.md" ]] || die "docs/releases/$new.md exists already"
if [[ -n "$(git status --porcelain -- "${files[@]}")" ]]; then
    git status --short -- "${files[@]}" >&2
    die "these files have changes not committed: commit them first, so that the bump is a change of its own"
fi

# replace <file> <perl substitution>: the file changed by it, which must change it.
replace() {
    local file="$1" expression="$2"
    OLD="$old" NEW="$new" perl -0pi -e "$expression" "$file"
}
must_change() {
    git diff --quiet -- "$1" && die "$1: nothing named $old where it was expected"
    return 0
}

log "$old -> $new"
printf '%s\n' "$new" > VERSION

replace bindings/rust/Cargo.toml 's/^(version = )"\Q$ENV{OLD}\E"/$1"$ENV{NEW}"/m'
must_change bindings/rust/Cargo.toml
for manifest in bindings/rust/*/Cargo.toml; do
    replace "$manifest" 's/^((?:zebkit-rs|zebkit-build) = \{[^\n}]*version = )"=\Q$ENV{OLD}\E"/$1"=$ENV{NEW}"/mg'
done
replace bindings/python/pyproject.toml 's/^(version = )"\Q$ENV{OLD}\E"/$1"$ENV{NEW}"/m'
must_change bindings/python/pyproject.toml
replace tools/zebkit-mcp/Cargo.toml 's/^(version = )"\Q$ENV{OLD}\E"/$1"$ENV{NEW}"/m'
must_change tools/zebkit-mcp/Cargo.toml
replace bindings/golang/zebkit.go 's/^(const BindingVersion = )"\Q$ENV{OLD}\E"/$1"$ENV{NEW}"/m'
must_change bindings/golang/zebkit.go

# README.md's instructions: the version line, the archives' names and links,
# Cargo.toml's lines, go get's tag; its prose about releases is left alone.
replace README.md 's/^(\| Version \| `VERSION`: )\Q$ENV{OLD}\E( \|)/$1$ENV{NEW}$2/mg; s/\bzebkit-\Q$ENV{OLD}\E-(x86_64|aarch64)/zebkit-$ENV{NEW}-$1/g; s{/releases/download/v\Q$ENV{OLD}\E/}{/releases/download/v$ENV{NEW}/}g; s/^(zebkit(?:-build)? = )"\Q$ENV{OLD}\E"/$1"$ENV{NEW}"/mg; s/(bindings\/golang\@v)\Q$ENV{OLD}\E\b/$1$ENV{NEW}/g'

# What users are told to write in Cargo.toml, and the latest engine.
replace bindings/rust/README.md 's/^((?:\/\/! )?zebkit(?:-build)? = )"\Q$ENV{OLD}\E"/$1"$ENV{NEW}"/mg; s/(latest engine is\s+)\Q$ENV{OLD}\E\b/$1$ENV{NEW}/g'
replace bindings/rust/zebkit-build/src/lib.rs 's/^(\/\/! zebkit(?:-build)? = )"\Q$ENV{OLD}\E"/$1"$ENV{NEW}"/mg'

# The lock files: the [[package]] entries of zebkit's crates that have no
# source (a path, not a registry), whatever else has the same version number.
for lock in "${locks[@]}"; do
    [[ -f "$lock" ]] || continue
    replace "$lock" 's/(\[\[package\]\]\nname = "(?:zebkit|zebkit-rs|zebkit-build|zebkit-mcp)"\nversion = )"\Q$ENV{OLD}\E"(?=\n(?!source))/$1"$ENV{NEW}"/g'
done

# engines.txt: the comments only; release.sh adds the lines of $new.
engines=bindings/rust/zebkit-rs/engines.txt
grep -E '^#' "$engines" > "$engines.new" || true
mv "$engines.new" "$engines"

# The release notes: next.md is this release's; a new one for what comes after.
git mv docs/releases/next.md "docs/releases/$new.md"
replace "docs/releases/$new.md" 's/\A# (?:Unreleased[^\n]*|zebkit \Q$ENV{NEW}\E \(draft\))\n/# zebkit $ENV{NEW}\n/'
grep -qx "# zebkit $new" "docs/releases/$new.md" || die "docs/releases/$new.md: its title is neither \"# Unreleased...\" nor \"# zebkit $new (draft)\""
# The paragraph that says the notes are a draft, from "**A draft.**" to the
# blank line after it: the release's notes are not one.
replace "docs/releases/$new.md" 's/^\*\*A draft\.\*\*.*?\n(?:\n|\z)//ms'
! grep -q '^\*\*A draft\.\*\*' "docs/releases/$new.md" || die "docs/releases/$new.md: its \"**A draft.**\" paragraph is still there"
cat > docs/releases/next.md <<EOF
# Unreleased: on \`main\` since $new

What \`main\` has that the $new release does not (\`git log v$new..main\`).
It goes into the next release's notes; until then nothing here is in a
release archive, on crates.io or on PyPI.

## Changes

Nothing yet.
EOF
git add docs/releases/next.md

check_versions
for lock in "${locks[@]}"; do
    [[ -f "$lock" ]] || continue
    if grep -A1 -E '^name = "(zebkit|zebkit-rs|zebkit-build|zebkit-mcp)"$' "$lock" | grep -qx "version = \"$old\""; then
        die "$lock still has a crate of zebkit's at $old"
    fi
done
log "VERSION and every manifest say $new"
git status --short -- "${files[@]}" "docs/releases/$new.md"
log "still naming $old (statements about releases, to read by hand):"
git grep -n -F "$old" -- README.md docs bindings tools examples ':!docs/releases' ':!*.lock' ':!*.json' || true
log "docs/releases/$new.md: the parts to fill from the release's build and runs (docs/BUILD.md, \"Releases\", step 3):"
grep -n -i 'to fill at release' "docs/releases/$new.md" || log "none"
