// A library of Mermaid (https://mermaid.js.org): a diagram's text to an SVG.
// Mermaid sizes its boxes by measuring their labels in the DOM, so it runs in
// the backend "page" (docs/JS_LIBRARIES.md): load it with
// {"backend": "page"}. Bundled into a folder by build.sh (tools/js-bundle.mjs);
// it loads nothing from the network.
import mermaid from 'mermaid';

mermaid.initialize({startOnLoad: false, securityLevel: 'strict', theme: 'default'});

let next = 0;

// renderSvg(text): the diagram as an SVG document; a syntax error is thrown.
export async function renderSvg(text) {
  const {svg} = await mermaid.render(`diagram-${++next}`, text);
  return svg;
}

// renderSvgBytes(text): the same as UTF-8 bytes, a part beside the JSON.
export async function renderSvgBytes(text) {
  return new TextEncoder().encode(await renderSvg(text));
}

// diagramType(text): what kind of diagram the text is ("flowchart-v2",
// "sequence", ...), or false when it does not parse.
export async function diagramType(text) {
  const parsed = await mermaid.parse(text, {suppressErrors: true});
  return parsed ? parsed.diagramType : false;
}
