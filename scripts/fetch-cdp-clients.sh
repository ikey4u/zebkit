#!/usr/bin/env bash
# Fetch what the client tests need into a directory: a pinned Node.js and the
# pinned npm packages puppeteer-core and playwright-core. Neither package
# downloads a browser; they are only the client libraries.
#
#   fetch-cdp-clients.sh DIR

set -euo pipefail

dir="${1:?usage: fetch-cdp-clients.sh DIR}"
node_version=v22.12.0
node_sha256=22982235e1b71fa8850f82edd09cdae7e3f32df1764a9ec298c72d25ef2c164f
puppeteer_version=24.10.0
playwright_version=1.52.0

mkdir -p "$dir"
archive="$dir/node-$node_version-linux-x64.tar.xz"
if [[ ! -x "$dir/node/bin/node" ]]; then
    [[ -f "$archive" ]] || curl -fsSL -o "$archive" "https://nodejs.org/dist/$node_version/node-$node_version-linux-x64.tar.xz"
    echo "$node_sha256  $archive" | sha256sum -c --quiet
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
