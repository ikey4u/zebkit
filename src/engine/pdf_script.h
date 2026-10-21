// zebkit.pdf in JavaScript, the same in a page and in the application's
// script: a function of the way calls are sent, which makes the object.
// The calls are answered by pdf_bridge.cc, over <zebkit/pdf.h>.

#ifndef ZEBKIT_ENGINE_PDF_SCRIPT_H_
#define ZEBKIT_ENGINE_PDF_SCRIPT_H_

namespace zebkit {

inline constexpr char kPdfScript[] = R"js(((send) => {
  const MODES = ['normal', 'rollover', 'down'];
  const FLAGS = {annotations: 1, forms: 2, lcdText: 4, grayscale: 8, printing: 16, noSmoothing: 32};
  class PdfDocument {
    #id;
    constructor(id) { this.#id = id; }
    #call(method, ...args) { return send('pdf', method, [this.#id, ...args]); }
    info() { return this.#call('info'); }
    setMetadata(metadata) { return this.#call('setMetadata', metadata); }
    page(index) { return this.#call('page', index); }
    // Every page's page(), in order.
    pages() { return this.#call('pages'); }
    // {width, height, data}: data is RGBA, rows top to bottom.
    render(index, options = {}) {
      let flags = options.flags;
      if (flags === undefined) {
        flags = 0;
        for (const [name, bit] of Object.entries(FLAGS)) {
          if (options[name] ?? (name === 'annotations')) flags |= bit;
        }
      }
      return this.#call('render', index, {
        scale: options.scale ?? 1, rotation: options.rotation ?? 0, flags,
        background: options.background ?? 0xffffffff,
        x: options.x ?? 0, y: options.y ?? 0, width: options.width ?? 0, height: options.height ?? 0,
      });
    }
    // One annotation's appearance alone ('normal', 'rollover', 'down'),
    // over its rectangle: {width, height, data}, or null without one.
    renderAnnotation(index, annotation, {mode = 'normal', scale = 1, rotation = 0, background = 0} = {}) {
      return this.#call('renderAnnotation', index, annotation, MODES.indexOf(mode),
                        {scale, rotation, background});
    }
    // Every annotation's appearances, for a layer over the page rendered
    // without them: [{index, name, rect, normal, rollover, down}].
    renderAnnotations(index, {scale = 1, rotation = 0} = {}) {
      return this.#call('renderAnnotations', index, {scale, rotation});
    }
    text(index) { return this.#call('text', index); }
    textLayout(index) { return this.#call('textLayout', index); }
    textLines(index) { return this.#call('textLines', index); }
    images(index) { return this.#call('images', index); }
    textRects(index, start, count) { return this.#call('textRects', index, start, count); }
    charAt(index, x, y, tolerance = 2) { return this.#call('charAt', index, x, y, tolerance); }
    search(query, {matchCase = false, wholeWord = false, first = 0, limit = 0} = {}) {
      return this.#call('search', String(query), (matchCase ? 1 : 0) | (wholeWord ? 2 : 0), first, limit);
    }
    outline() { return this.#call('outline'); }
    links(index) { return this.#call('links', index); }
    annotations(index) { return this.#call('annotations', index); }
    // annotations() of each of the pages, in one call.
    annotationsOfPages(pages) { return this.#call('annotationsOfPages', [...pages]); }
    addAnnotation(index, annotation) { return this.#call('addAnnotation', index, annotation); }
    updateAnnotation(index, annotation, changes) {
      return this.#call('updateAnnotation', index, annotation, changes);
    }
    removeAnnotation(index, annotation) { return this.#call('removeAnnotation', index, annotation); }
    // A stamp's appearance as an image, {width, height, data} in RGBA.
    setAnnotationImage(index, annotation, {width, height, data}) {
      return this.#call('setAnnotationImage', index, annotation, {width, height, data});
    }
    flatten(index, {print = false} = {}) { return this.#call('flatten', index, print); }
    // Takes what is under the areas ([[l, b, r, t], ...]; null for the
    // page's redaction marks) out of the page for good, a box of fill
    // (0xRRGGBBAA, 0 for none) over each. The number of areas.
    redact(index, areas = null, fill = 0x000000ff) {
      return this.#call('redact', index, areas ? [...areas].map((a) => [...a]) : null, fill);
    }
    attachments() { return this.#call('attachments'); }
    attachment(index) { return this.#call('attachment', index); }
    attach(name, bytes) { return this.#call('attach', String(name), bytes); }
    detach(index) { return this.#call('detach', index); }
    save({incremental = false, noSecurity = false} = {}) {
      return this.#call('save', (incremental ? 1 : 0) | (noSecurity ? 2 : 0));
    }
    formFields() { return this.#call('formFields'); }
    setField(name, value) { return this.#call('setField', String(name), String(value)); }
    resetForm() { return this.#call('resetForm'); }
    rotatePage(index, rotation) { return this.#call('rotatePage', index, rotation); }
    deletePages(pages) { return this.#call('deletePages', [...pages]); }
    movePages(pages, to) { return this.#call('movePages', [...pages], to); }
    insertPage(at, width, height) { return this.#call('insertPage', at, width, height); }
    // Copies pages (all when null) of another open document in, at index at.
    importPages(from, pages, at) { return this.#call('importPages', from.#id, pages ? [...pages] : null, at); }
    close() { return this.#call('close'); }
  }
  return Object.freeze({
    // Bytes; in the application's script also a file's path.
    open: async (source, {password = null} = {}) =>
        new PdfDocument(await send('pdf', 'open', [source, password])),
    // An empty document, to build from others' pages.
    create: async () => new PdfDocument(await send('pdf', 'create', [])),
  });
}))js";

}  // namespace zebkit

#endif  // ZEBKIT_ENGINE_PDF_SCRIPT_H_
