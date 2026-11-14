// A library of markdown-it (https://github.com/markdown-it/markdown-it):
// Markdown to HTML. Bundled into a folder by build.sh (tools/js-bundle.mjs),
// which reads the comments below for the facades it generates (--wrapper).
import MarkdownIt from 'markdown-it';

const plain = new MarkdownIt();
const rich = new MarkdownIt({html: false, linkify: true, typographer: true});

/**
 * The HTML of a Markdown document.
 * @param {string} text the Markdown
 * @param {{rich?: boolean}} [options] rich: links found in the text, and typographic quotes and dashes
 * @returns {string}
 */
export function render(text, options = {}) {
  return (options.rich ? rich : plain).render(String(text));
}

/**
 * The HTML of one line, without a paragraph around it.
 * @param {string} text the Markdown
 * @returns {string}
 */
export function renderInline(text) {
  return plain.renderInline(String(text));
}

/**
 * Many documents in one call.
 * @param {string[]} texts the Markdown of each
 * @returns {string[]}
 */
export function renderMany(texts) {
  return texts.map((text) => plain.render(String(text)));
}
