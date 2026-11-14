#!/usr/bin/env bash
# The libraries of this example, bundled with tools/js-bundle.mjs: markdown-it
# (markdown/) and ECharts (charts/), and Mermaid (diagram/, for the backend
# "page"), each into <out>/<name>/index.mjs with its packages' licences beside
# it. Build time only; npm downloads the
# packages (once: <out>/work keeps them).
#
#   examples/js-libraries/build.sh [out]     out: .cache/js-libraries
#
# scripts/engine-test.sh --fetch runs it for tests/engine/library.py, whose
# run is offline after that. markdown/markdown.rs and markdown/markdown.py are
# the facades tools/js-bundle.mjs --wrapper makes of <out>/markdown, which
# library.py checks they still are:
#
#   node tools/js-bundle.mjs --wrapper rust .cache/js-libraries/markdown --name Markdown \
#       --out examples/js-libraries/markdown/markdown.rs   (and python, markdown.py)

source "$(dirname "${BASH_SOURCE[0]}")/../../scripts/lib.sh"

MARKDOWN_IT_VERSION=14.1.0
ECHARTS_VERSION=5.6.0
MERMAID_VERSION=12.2.0

out="$(caller_path "${1:-$CACHE_DIR/js-libraries}")"
here="$ZEBKIT_ROOT/examples/js-libraries"
mkdir -p "$out/work"
# esbuild at upstream.lock's version, installed with the packages.
"${node_cmd[@]}" "$ZEBKIT_ROOT/tools/js-bundle.mjs" "$here/markdown/entry.mjs" "$out/markdown" \
    --install "markdown-it@$MARKDOWN_IT_VERSION" --install "echarts@$ECHARTS_VERSION" \
    --install "mermaid@$MERMAID_VERSION" --work "$out/work" --minify \
    || die "markdown-it was not bundled"
"${node_cmd[@]}" "$ZEBKIT_ROOT/tools/js-bundle.mjs" "$here/charts/entry.mjs" "$out/charts" \
    --install "markdown-it@$MARKDOWN_IT_VERSION" --install "echarts@$ECHARTS_VERSION" \
    --install "mermaid@$MERMAID_VERSION" --work "$out/work" --minify \
    || die "ECharts was not bundled"
# Mermaid lays its diagrams out in the DOM: a library for the backend "page".
"${node_cmd[@]}" "$ZEBKIT_ROOT/tools/js-bundle.mjs" "$here/diagram/entry.mjs" "$out/diagram" \
    --install "markdown-it@$MARKDOWN_IT_VERSION" --install "echarts@$ECHARTS_VERSION" \
    --install "mermaid@$MERMAID_VERSION" --work "$out/work" --minify \
    || die "Mermaid was not bundled"
log "bundled into $out: markdown, charts, diagram"
