// A library of markdown-it (https://github.com/markdown-it/markdown-it):
// Markdown to HTML. Bundled into a folder by build.sh (tools/js-bundle.mjs).
import MarkdownIt from 'markdown-it';

const plain = new MarkdownIt();
const rich = new MarkdownIt({html: false, linkify: true, typographer: true});

// render(text, {rich}): the HTML of a Markdown document.
export function render(text, options = {}) {
  return (options.rich ? rich : plain).render(String(text));
}

// renderInline(text): the HTML of one line, without a paragraph around it.
export function renderInline(text) {
  return plain.renderInline(String(text));
}

// renderMany(texts): many documents in one call, as a list.
export function renderMany(texts) {
  return texts.map((text) => plain.render(String(text)));
}
