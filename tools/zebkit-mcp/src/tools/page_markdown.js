// A page's content as Markdown, in the page: what a reader sees of it, without
// scripts, styles or hidden elements. Called with an element or the body.
(root) => {
  const skip = new Set(['SCRIPT', 'STYLE', 'NOSCRIPT', 'TEMPLATE', 'SVG', 'CANVAS', 'IFRAME', 'OBJECT', 'EMBED', 'HEAD']);
  const hidden = (element) => {
    if (element.hidden || element.getAttribute('aria-hidden') === 'true') return true;
    const style = getComputedStyle(element);
    return style.display === 'none' || style.visibility === 'hidden';
  };
  const clean = (text) => text.replace(/\s+/g, ' ');
  const inline = (node) => {
    let out = '';
    for (const child of node.childNodes) {
      if (child.nodeType === Node.TEXT_NODE) { out += clean(child.nodeValue); continue; }
      if (child.nodeType !== Node.ELEMENT_NODE) continue;
      const tag = child.tagName;
      if (skip.has(tag.toUpperCase()) || hidden(child)) continue;
      if (tag === 'BR') { out += '\n'; continue; }
      const inner = inline(child);
      if (tag === 'A' && child.getAttribute('href') && inner.trim()) out += `[${inner.trim()}](${child.href})`;
      else if ((tag === 'STRONG' || tag === 'B') && inner.trim()) out += `**${inner.trim()}**`;
      else if ((tag === 'EM' || tag === 'I') && inner.trim()) out += `*${inner.trim()}*`;
      else if (tag === 'CODE' && inner.trim()) out += '`' + inner.trim() + '`';
      else if (tag === 'IMG') out += child.alt ? `![${clean(child.alt)}](${child.src})` : '';
      else if (tag === 'INPUT' || tag === 'SELECT' || tag === 'TEXTAREA' || tag === 'BUTTON') {
        out += `[${tag.toLowerCase()}${child.name ? ' ' + child.name : ''}${inner.trim() ? ': ' + inner.trim() : ''}]`;
      } else out += inner;
    }
    return out;
  };
  const blocks = new Set(['P', 'DIV', 'SECTION', 'ARTICLE', 'MAIN', 'HEADER', 'FOOTER', 'NAV', 'ASIDE', 'FIGURE', 'FIGCAPTION', 'FORM', 'FIELDSET', 'ADDRESS', 'DETAILS', 'SUMMARY', 'BODY', 'HTML', 'LI', 'DD', 'DT', 'DL', 'CENTER']);
  const hasBlock = (element) => [...element.children].some((child) =>
    blocks.has(child.tagName) || /^(H[1-6]|UL|OL|TABLE|PRE|BLOCKQUOTE|HR)$/.test(child.tagName));
  const out = [];
  const walk = (element, indent) => {
    for (const child of element.childNodes) {
      if (child.nodeType === Node.TEXT_NODE) {
        const text = clean(child.nodeValue).trim();
        if (text) out.push(indent + text);
        continue;
      }
      if (child.nodeType !== Node.ELEMENT_NODE) continue;
      const tag = child.tagName;
      if (skip.has(tag.toUpperCase()) || hidden(child)) continue;
      const heading = /^H([1-6])$/.exec(tag);
      if (heading) { const text = inline(child).trim(); if (text) out.push('', '#'.repeat(+heading[1]) + ' ' + text, ''); continue; }
      if (tag === 'UL' || tag === 'OL') {
        let index = 1;
        for (const item of child.children) {
          if (item.tagName !== 'LI' || hidden(item)) continue;
          const mark = tag === 'OL' ? `${index++}. ` : '- ';
          if (hasBlock(item)) { out.push(indent + mark.trimEnd()); walk(item, indent + '  '); }
          else out.push(indent + mark + inline(item).trim());
        }
        out.push('');
        continue;
      }
      if (tag === 'TABLE') {
        const rows = [...child.rows].map((row) => [...row.cells].map((cell) => inline(cell).trim().replace(/\|/g, '\\|')));
        rows.forEach((row, index) => {
          out.push('| ' + row.join(' | ') + ' |');
          if (index === 0) out.push('|' + ' --- |'.repeat(row.length));
        });
        out.push('');
        continue;
      }
      if (tag === 'PRE') { out.push('', '```', child.innerText.replace(/\n$/, ''), '```', ''); continue; }
      if (tag === 'BLOCKQUOTE') { out.push('> ' + inline(child).trim(), ''); continue; }
      if (tag === 'HR') { out.push('', '---', ''); continue; }
      if (blocks.has(tag) && hasBlock(child)) { walk(child, indent); continue; }
      const text = inline(child).trim();
      if (text) {
        if (blocks.has(tag)) out.push('', indent + text, '');
        else out.push(indent + text);
      }
    }
  };
  walk(root, '');
  return out.join('\n').replace(/\n{3,}/g, '\n\n').trim();
}
