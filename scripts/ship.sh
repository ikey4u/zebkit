#!/usr/bin/env bash
# A fresh zebkit release, from the sources to GitHub, crates.io and PyPI, in
# one command: `mise run ship -- X.Y.Z` (the version VERSION has when
# omitted). Every step is incremental and the whole is resumable: run it
# again after a failure and it goes on from what is done.
#
#   1. Bump (scripts/bump-version.sh), when VERSION is not X.Y.Z yet: the
#      release's summary is the paragraph after "**A draft.**" in
#      docs/releases/next.md, written before.
#   2. The static checks (mise run check:static): stale reports stop here.
#   3. iOS (scripts/release-ios.sh), unless dist/release-ios is this version
#      already or ZEBKIT_RELEASE_NO_IOS=1.
#   4. Every archive (scripts/release.sh): Linux, macOS (Apple silicon,
#      Intel), Windows, Android, iOS, incremental in out/<profile> with siso;
#      the compiler's random crashes under load are retried (build.sh, and
#      whole runs here).
#   5. The notes' parts "To fill at release": the archives' sizes, and what
#      was checked.
#   6. Commit the release (VERSION, manifests, engines.txt, notes).
#   7. The release check (mise run check, under a session bus); a pass for
#      these sources is recorded and not run again.
#   8. Push, publish the GitHub release (release.sh --publish-only), then the
#      crates and the Python package (tokens from the environment, else
#      ZEBKIT_ENV_FILE when set).
#
# Build only through these scripts: plain ninja in a siso output directory
# makes siso rebuild much of Chromium (docs/BUILD.md).

source "$(dirname "${BASH_SOURCE[0]}")/lib.sh"
cd "$ZEBKIT_ROOT"

# git at origin over ssh never prompts and never hangs on a stalled
# connection (release.sh then goes over HTTPS with gh's credentials).
export GIT_SSH_COMMAND="${GIT_SSH_COMMAND:-ssh -o BatchMode=yes -o ConnectTimeout=20 -o ServerAliveInterval=15 -o ServerAliveCountMax=4}"

version="${1:-$(cat VERSION)}"
[[ "$version" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || die "usage: $(basename "$0") [X.Y.Z]"
notes="docs/releases/$version.md"
step() { log "ship $version: $*"; }

published() { gh release view "v$version" --json assets --jq '.assets | length' 2> /dev/null | grep -qv '^0$'; }

# 1. Bump.
if [[ "$(cat VERSION)" != "$version" ]]; then
    grep -q '^\*\*A draft\.\*\*' docs/releases/next.md \
        || die "write the release's summary in docs/releases/next.md first: a paragraph \"**A draft.** ...\", then the summary"
    step "bump"
    scripts/bump-version.sh "$version"
fi
[[ -f "$notes" ]] || die "no $notes: the bump writes it from docs/releases/next.md"

if ! published; then
    # 2. Static checks: fail early, before hours of building.
    step "static checks"
    mise run check:static

    # 3. iOS.
    plist="dist/release-ios/Zebkit.xcframework/ios-arm64/Zebkit.framework/Info.plist"
    ios_version="$(python3 -I -c 'import plistlib, sys; print(plistlib.load(open(sys.argv[1], "rb"))["CFBundleShortVersionString"])' "$plist" 2> /dev/null || true)"
    if [[ "${ZEBKIT_RELEASE_NO_IOS:-}" != 1 && "$ios_version" != "$version" ]]; then
        step "iOS package"
        scripts/release-ios.sh
    fi

    # 4. Every archive; whole runs again while the compiler's crashes stop one.
    for attempt in 1 2 3 4; do
        step "archives (run $attempt)"
        if scripts/release.sh 2>&1 | tee "$CACHE_DIR/ship-release.log"; then break; fi
        grep -q "frontend command failed due to signal" "$CACHE_DIR/ship-release.log" \
            || die "the release build failed (log: $CACHE_DIR/ship-release.log)"
        (( attempt < 4 )) || die "the compiler kept crashing (log: $CACHE_DIR/ship-release.log)"
    done

    # 5. The notes' parts to fill: the archives' sizes, and the checks.
    step "release notes"
    python3 -I - "$version" "$notes" << 'PY'
import os, re, subprocess, sys
version, notes = sys.argv[1], sys.argv[2]
targets = [
    ("x86_64-unknown-linux-gnu", "release", "Linux x86-64, glibc 2.17 or newer"),
    ("aarch64-apple-darwin", "release-macos", "macOS, Apple silicon"),
    ("x86_64-apple-darwin", "release-macos-intel", "macOS, Intel"),
    ("x86_64-pc-windows-msvc", "release-windows", "Windows 10 or newer, x64"),
    ("android", "release-android", "Android 10 (API 29) or newer: arm64 and x86-64 (the emulator), without the engine"),
    ("ios", "release-ios", "iOS 15 or newer: devices and the simulator (arm64), without the engine"),
]
def mib(n): return f"{n / 1048576:.1f} MiB"
def du(path):
    out = subprocess.run(["du", "-sb", path], capture_output=True, text=True, check=True).stdout
    return int(out.split()[0])
rows = []
for target, folder, system in targets:
    archive = f"dist/zebkit-{version}-{target}.tar.xz"
    if not os.path.exists(archive):
        continue
    rows.append(f"| `zebkit-{version}-{target}.tar.xz` | {system} | {mib(du('dist/' + folder))} | {mib(os.path.getsize(archive))} |")
text = open(notes).read()
text = re.sub(r"^\| \*\*To fill at release\*\* \|[^\n]*\n", "".join(r + "\n" for r in rows), text, flags=re.M)
text = re.sub(
    r"\*\*To fill at release\*\*: the suite's counts[^\n]*(?:\n[^\n]+)*",
    "Built on Linux from this commit for every system above; the release check\n"
    "(`mise run check`: the bindings, the engine tests in strict mode, glibc,\n"
    "web-bench) passed on the Linux package before this release was published.",
    text,
)
open(notes, "w").write(text)
left = [l for l in text.splitlines() if "To fill at release" in l]
if left:
    sys.exit(f"{notes}: still to fill by hand: {left}")
PY

    # 6. Commit what the release changed.
    if [[ -n "$(git status --porcelain)" ]]; then
        step "commit"
        git add -A VERSION README.md bindings docs/releases examples tools scripts docs/BUILD.md mise.toml
        git commit -q -m "zebkit $version: version, release notes with the archives' sizes, and the archives' SHA-256 in engines.txt"
    fi
    [[ -z "$(git status --porcelain --untracked-files=no)" ]] || die "uncommitted changes besides the release's: commit or stash them first"

    # 7. The release check (recorded per sources; a pass is not run again).
    step "release check"
    dbus-run-session -- mise run check

    # 8. Push and publish on GitHub.
    step "push"
    git push origin HEAD
    step "GitHub release"
    scripts/release.sh --publish-only
fi

# The crates and the Python package. Their tokens come from the environment;
# one not set there is read from the file ZEBKIT_ENV_FILE names, when set (NAME=value
# lines). Nothing is printed.
step "crates.io and PyPI"
env_file="${ZEBKIT_ENV_FILE:-}"
if [[ -n "$env_file" ]]; then
    [[ -f "$env_file" ]] || die "ZEBKIT_ENV_FILE: no file $env_file"
    for name in CARGO_REGISTRY_TOKEN PYPI_TOKEN UV_PUBLISH_TOKEN; do
        [[ -n "${!name:-}" ]] && continue
        line="$(grep -E "^(export )?$name=" "$env_file" | tail -n 1 || true)"
        [[ -n "$line" ]] || continue
        value="${line#*=}"
        value="${value%\"}"; value="${value#\"}"; value="${value%\'}"; value="${value#\'}"
        export "$name=$value"
    done
fi
crate_published() {
    curl -fsS -H "User-Agent: zebkit-ship" "https://crates.io/api/v1/crates/zebkit/$version" > /dev/null 2>&1
}
crate_published || mise run r:crate
pypi_published() { curl -fsS "https://pypi.org/pypi/zebkit/$version/json" > /dev/null 2>&1; }
pypi_published || mise run r:python
step "done: https://github.com/ikey4u/zebkit/releases/tag/v$version"
