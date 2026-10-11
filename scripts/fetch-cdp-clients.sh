#!/usr/bin/env bash
# Fetch what the client tests need into a directory: a pinned Node.js and the
# pinned npm packages puppeteer-core and playwright-core. Neither package
# downloads a browser; they are only the client libraries.
#
#   fetch-cdp-clients.sh DIR

set -euo pipefail

dir="${1:?usage: fetch-cdp-clients.sh DIR}"
node_version=v22.12.0
puppeteer_version=24.10.0
playwright_version=1.52.0

# Node.js as nodejs.org builds it for this machine, with the checksum it
# publishes for that build.
case "$(uname -s)-$(uname -m)" in
    Linux-x86_64) node_build=linux-x64 node_sha256=22982235e1b71fa8850f82edd09cdae7e3f32df1764a9ec298c72d25ef2c164f ;;
    Linux-aarch64) node_build=linux-arm64 node_sha256=8cfd5a8b9afae5a2e0bd86b0148ca31d2589c0ea669c2d0b11c132e35d90ed68 ;;
    Darwin-arm64) node_build=darwin-arm64 node_sha256=0047be0cfda922eb73876f9ef41de361c36b7654c884d13d9b783b0efd1db9aa ;;
    Darwin-x86_64) node_build=darwin-x64 node_sha256=d68ef0c4c19b3b3b88c0e7408668d0a539607c136a14668e079feed0c6ec8bec ;;
    *) echo "error: no Node.js build is pinned for $(uname -s) on $(uname -m)" >&2; exit 1 ;;
esac

mkdir -p "$dir"
archive="$dir/node-$node_version-$node_build.tar.xz"
if [[ ! -x "$dir/node/bin/node" ]]; then
    [[ -f "$archive" ]] || curl -fsSL -o "$archive" "https://nodejs.org/dist/$node_version/node-$node_version-$node_build.tar.xz"
    if command -v sha256sum >/dev/null; then got="$(sha256sum "$archive")"; else got="$(shasum -a 256 "$archive")"; fi
    [[ "${got%% *}" == "$node_sha256" ]] || { echo "error: checksum mismatch for $archive" >&2; exit 1; }
    rm -rf "$dir/node"
    mkdir "$dir/node"
    tar -xJf "$archive" -C "$dir/node" --strip-components=1
fi

stamp="$dir/node_modules/.zebkit-$puppeteer_version-$playwright_version"
if [[ ! -f "$stamp" ]]; then
    PATH="$dir/node/bin:$PATH" npm install --prefix "$dir" --no-audit --no-fund --no-save --ignore-scripts \
        "puppeteer-core@$puppeteer_version" "playwright-core@$playwright_version" >&2
    touch "$stamp"
fi
