#!/usr/bin/env bash
# Publish the Python binding of bindings/python to PyPI as `zebkit`, at the
# version in VERSION.
#
#   scripts/publish-python.sh            # mise run r:python
#   scripts/publish-python.sh --dry-run  # build and check the files, no upload
#
# The token is PyPI's API token, in UV_PUBLISH_TOKEN (uv's own name) or
# PYPI_TOKEN; it is never written anywhere. UV_PUBLISH_URL publishes elsewhere,
# such as TestPyPI's https://test.pypi.org/legacy/. The package is the binding
# alone: the engine is the GitHub release's archive of the same version.
source "$(dirname "$0")/lib.sh"

dry=0
[[ "${1:-}" == --dry-run ]] && dry=1

binding="$ZEBKIT_ROOT/bindings/python"
version="$(tr -d '[:space:]' < "$ZEBKIT_ROOT/VERSION")"
package_version="$(sed -n 's/^version = "\(.*\)"/\1/p' "$binding/pyproject.toml" | head -1)"
[[ "$package_version" == "$version" ]] || die "VERSION says $version but bindings/python/pyproject.toml says $package_version"
command -v uv > /dev/null || die "publishing needs uv; 'mise run r:python' provides it"

if (( ! dry )); then
    export UV_PUBLISH_TOKEN="${UV_PUBLISH_TOKEN:-${PYPI_TOKEN:-}}"
    [[ -n "$UV_PUBLISH_TOKEN" ]] || die "set UV_PUBLISH_TOKEN (or PYPI_TOKEN) to a PyPI API token"
    [[ -z "$(git -C "$ZEBKIT_ROOT" status --porcelain -- bindings/python)" ]] \
        || die "bindings/python has changes not committed: the package is published from what is committed"
fi

out="$ZEBKIT_ROOT/dist/python"
rm -rf "$out"
log "building zebkit $version for Python"
(cd "$binding" && uv build --out-dir "$out" --no-sources)
ls "$out"
# The wheel must hold the binding and nothing of the repository's own.
python3 - "$out" <<'EOF'
import pathlib, sys, zipfile
wheel = next(pathlib.Path(sys.argv[1]).glob("zebkit-*.whl"))
names = zipfile.ZipFile(wheel).namelist()
stray = [n for n in names if "__pycache__" in n or not (n.startswith("zebkit/") or ".dist-info/" in n)]
if "zebkit/__init__.py" not in names or stray:
    sys.exit(f"{wheel.name}: unexpected contents {stray or names}")
print(f"{wheel.name}: {len(names)} files")
EOF

if (( dry )); then
    log "built (dry run): $out"
else
    log "publishing zebkit $version to ${UV_PUBLISH_URL:-PyPI}"
    uv publish "$out"/*
    log "published: https://pypi.org/project/zebkit/$version/"
fi
