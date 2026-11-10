// zebkit.pdf in JavaScript, the same in a page and in the application's
// script: a function of the way calls are sent, which makes the object.
// The calls are answered by pdf_bridge.cc, over <zebkit/pdf.h>.

#ifndef ZEBKIT_ENGINE_PDF_SCRIPT_H_
#define ZEBKIT_ENGINE_PDF_SCRIPT_H_

namespace zebkit {

inline constexpr char kPdfScript[] = R"js(((send) => {
  const MODES = ['normal', 'rollover', 'down'];
  const FLAGS = {annotations: 1, forms: 2, lcdText: 4, grayscale: 8, printing: 16, noSmoothing: 32};

  // Bytes as base64, in pieces: a font is large for one call of apply().
  const toBase64 = (bytes) => {
    const b = bytes instanceof Uint8Array ? bytes : new Uint8Array(bytes.buffer ?? bytes);
    let out = '';
    for (let i = 0; i < b.length; i += 30000)
      out += btoa(String.fromCharCode.apply(null, b.subarray(i, i + 30000)));
    return out;
  };
  const toColor = (c) => Array.isArray(c)
      ? ((c[0] << 24) | (c[1] << 16) | (c[2] << 8) | (c[3] ?? 255)) >>> 0 : c >>> 0;
  let nextFont = 1;

  // zebkit.canvas: <zebkit/canvas.h> as a list of operations, kept here and
  // run by the engine when pixels are asked for, or by a document when it is
  // an appearance or a layer. Nothing lives in the engine between calls.
  class Canvas {
    #ops = [];
    #fonts = new Map();
    constructor(width = 0, height = 0) { this.width = width; this.height = height; }
    #op(...op) { this.#ops.push(op); return this; }
    save() { return this.#op('save'); }
    restore() { return this.#op('restore'); }
    translate(x, y) { return this.#op('translate', x, y); }
    scale(x, y = x) { return this.#op('scale', x, y); }
    rotate(degrees) { return this.#op('rotate', degrees); }
    transform(a, b, c, d, e, f) { return this.#op('transform', a, b, c, d, e, f); }
    setMatrix(a, b, c, d, e, f) { return this.#op('setMatrix', a, b, c, d, e, f); }
    fillColor(color) { return this.#op('fillColor', toColor(color)); }
    strokeColor(color) { return this.#op('strokeColor', toColor(color)); }
    alpha(a) { return this.#op('alpha', a); }
    blend(mode) { return this.#op('blend', String(mode)); }
    lineWidth(w) { return this.#op('lineWidth', w); }
    lineCap(cap) { return this.#op('lineCap', typeof cap === 'string' ? ['butt', 'round', 'square'].indexOf(cap) : cap); }
    lineJoin(join) { return this.#op('lineJoin', typeof join === 'string' ? ['miter', 'round', 'bevel'].indexOf(join) : join); }
    miterLimit(m) { return this.#op('miterLimit', m); }
    dash(lengths, phase = 0) { return this.#op('dash', [...lengths], phase); }
    linearGradient(x0, y0, x1, y1, offsets, colors) {
      return this.#op('linearGradient', x0, y0, x1, y1, [...offsets], [...colors].map(toColor));
    }
    radialGradient(x, y, radius, offsets, colors) {
      return this.#op('radialGradient', x, y, radius, [...offsets], [...colors].map(toColor));
    }
    beginPath() { return this.#op('beginPath'); }
    moveTo(x, y) { return this.#op('moveTo', x, y); }
    lineTo(x, y) { return this.#op('lineTo', x, y); }
    quadTo(cx, cy, x, y) { return this.#op('quadTo', cx, cy, x, y); }
    curveTo(c1x, c1y, c2x, c2y, x, y) { return this.#op('curveTo', c1x, c1y, c2x, c2y, x, y); }
    arc(x, y, r, start, end, counterclockwise = false) { return this.#op('arc', x, y, r, start, end, !!counterclockwise); }
    rect(x, y, w, h) { return this.#op('rect', x, y, w, h); }
    closePath() { return this.#op('closePath'); }
    fill(evenOdd = false) { return this.#op('fill', !!evenOdd); }
    stroke() { return this.#op('stroke'); }
    clip(evenOdd = false) { return this.#op('clip', !!evenOdd); }
    clear(color) { return this.#op('clear', toColor(color)); }
    // Pixels as zebkit.image.decode() gives them: {width, height, data}.
    drawImage({width, height, data}, x, y, w = width, h = height) {
      return this.#op('image', toBase64(data), width, height, x, y, w, h);
    }
    // A font from its bytes; the same bytes are sent once.
    font(bytes, size = 12) {
      let id = this.#fonts.get(bytes);
      if (!id) {
        id = nextFont++;
        this.#fonts.set(bytes, id);
        this.#op('font', id, toBase64(bytes));
      }
      return this.#op('setFont', id, size);
    }
    fillText(text, x, y) { return this.#op('fillText', String(text), x, y); }
    textPath(text, x, y) { return this.#op('textPath', String(text), x, y); }
    // The list of operations, for what takes one.
    get operations() { return this.#ops; }
    // {width, height, data}: RGBA, not premultiplied. @over: pixels to draw
    // over ({data} or bytes, width * height * 4, as pixels() gives them),
    // the canvas's own picture when null (zebkit_canvas_wrap()).
    pixels(over = null) { return send('canvas', 'render', [this.width, this.height, this.#ops, 'pixels', over?.data ?? over]); }
    png(over = null) { return send('canvas', 'render', [this.width, this.height, this.#ops, 'png', over?.data ?? over]); }
    // The advance of @text in the font set last, in the canvas's units.
    measureText(text) { return send('canvas', 'measure', [this.width || 1, this.height || 1, this.#ops, String(text)]); }
  }

  // zebkit.pdf.use(plugin): what every document opened from now on runs through.
  const allPlugins = [];
  const eventHook = (plugin, group, event) => plugin[group]?.[event] ?? plugin[group]?.['*'];
  // What an event's "before" hook said, as what to do. (A refusal throws.)
  const decided = (answer, current, event) => {
    if (answer === false) throw new Error('refused by a hook');
    if (answer && typeof answer === 'object') {
      if (answer.refuse) throw new Error(String(answer.refuse));
      return {...answer, event};
    }
    return current;
  };

  class PdfDocument {
    #id;
    #plugins;
    #closed = false;
    #kinds = new Map();
    #ocr = new Map();
    #fonts = new Set();
    #ready = null;
    constructor(id, plugins = []) { this.#id = id; this.#plugins = [...plugins]; }
    #call(method, ...args) { return send('pdf', method, [this.#id, ...args]); }

    // ---- Plugins: run here, on the script's own thread; the engine's PDF
    // work is on another, where a hook could not be waited for. ----

    // Adds a plugin to this document. Returns a function that removes it.
    async use(plugin) {
      if (!plugin || typeof plugin !== 'object') throw new TypeError('a plugin is an object');
      this.#plugins.push(plugin);
      await this.#prepare(plugin);
      return () => { this.unuse(plugin); };
    }
    unuse(plugin) {
      const at = this.#plugins.indexOf(plugin);
      if (at >= 0) this.#plugins.splice(at, 1);
      for (const [name, kind] of this.#kinds) {
        if (kind.plugin === plugin) {
          this.#kinds.delete(name);
          this.#call('removeHook', kind.hook).catch(() => {});
        }
      }
    }
    async #prepare(plugin) {
      this.#ready ??= this.#call('enableOperations');
      await this.#ready;
      for (const [name, kind] of Object.entries(plugin.kinds ?? {})) {
        const hook = await this.#call('registerKind', name, kind.prefix, kind.subtype ?? '', kind.schema ?? null);
        this.#kinds.set(name, {...kind, name, plugin, hook});
      }
    }
    async _start() {
      for (const plugin of this.#plugins) await this.#prepare(plugin);
    }
    // The plugins now: one a hook removes is not called after it.
    *#each() {
      for (const plugin of [...this.#plugins]) {
        if (this.#plugins.includes(plugin) && !this.#closed) yield plugin;
      }
    }
    // @act with what "before" hooks make of @operation, and "after" hooks told
    // of what the engine says happened.
    async #around(event, operation, act) {
      if (!this.#plugins.length) return act(operation);
      let current = {...operation, event};
      for (const plugin of this.#each()) {
        const hook = eventHook(plugin, 'before', event);
        if (hook) current = decided(await hook(current), current, event);
      }
      if (this.#closed) throw new Error('a hook closed the document');
      const result = await act(current);
      await this.#tellAfter();
      return result;
    }
    async #tellAfter() {
      if (this.#closed) return;
      for (const operation of await this.#call('takeOperations')) {
        for (const plugin of this.#each()) {
          const hook = eventHook(plugin, 'after', operation.event);
          if (hook) await hook(operation);
        }
      }
    }
    // The annotation's box unturned: what a canvas draws over.
    #size(annotation) {
      const r = annotation.entries?.ZebkitBox ?? annotation.rect ?? [0, 0, 0, 0];
      return {width: Math.abs(r[2] - r[0]), height: Math.abs(r[3] - r[1])};
    }
    // A kind's drawing, or an appearance plugin's, for the annotation as it is now.
    async #redraw(page, index) {
      if (!this.#plugins.length) return;
      const annotation = (await this.#call('annotations', page))[index];
      if (!annotation) return;
      const size = this.#size(annotation);
      const draw = async (fn) => {
        const canvas = new Canvas(size.width, size.height);
        if ((await fn(canvas, annotation, size)) === false) return false;
        await this.#call('drawAnnotation', page, index, canvas.operations);
        return true;
      };
      const kind = this.#kinds.get(annotation.type);
      if (kind?.draw) { await draw(kind.draw); return; }
      for (const plugin of this.#each()) {
        const fn = plugin.appearance?.[annotation.type] ?? plugin.appearance?.['*'];
        if (fn && await draw(fn)) return;
      }
    }
    // Fonts plugins have for free text about to be drawn.
    async #wantFonts(annotation) {
      if (annotation?.type !== 'freeText' && !annotation?.font && !annotation?.contents) return;
      const families = [];
      if (annotation.font && !/helv|arial|sans|tim|serif|cour|mono/i.test(annotation.font)) families.push(annotation.font);
      if (/[^\u0000-\u00ff\u2013-\u2122]/.test(annotation.contents ?? '')) families.push('');
      for (const family of families) {
        if (this.#fonts.has(family)) continue;
        for (const plugin of this.#each()) {
          const bytes = await plugin.font?.(family);
          if (bytes) {
            this.#fonts.add(family);
            await this.#call('addFont', family, bytes);
            break;
          }
        }
      }
    }
    async #checkData(type, data) {
      const problem = await this.#kinds.get(type)?.check?.(data ?? {});
      if (problem) throw new Error(String(problem));
    }
    // Runs an operation an "after" hook saw, or its "inverse".
    apply(operation) {
      return this.#around(operation.event, operation, async (current) => {
        await this.#call('apply', current);
        this.#ocr.clear();
        if (current.event === 'annotation.add') {
          const list = await this.#call('annotations', current.page);
          await this.#redraw(current.page, list.length - 1);
        }
      });
    }
    // Points of a drawing tool: the built-in filters, then the plugins'.
    async filterInput(tool, points, options = null) {
      let out = await this.#call('filterInput', String(tool), points, options);
      for (const plugin of this.#each()) {
        const filter = plugin.filters?.[tool];
        if (filter) out = (await filter(out)) ?? out;
      }
      return out;
    }
    addFont(family, bytes) { this.#fonts.add(String(family)); return this.#call('addFont', String(family), bytes); }
    pageData(index, prefix) { return this.#call('pageData', index, String(prefix)); }

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
      return this.#around('render', {page: index, options: {
        scale: options.scale ?? 1, rotation: options.rotation ?? 0, flags,
        background: options.background ?? 0xffffffff,
        x: options.x ?? 0, y: options.y ?? 0, width: options.width ?? 0, height: options.height ?? 0,
      }}, async ({page, options: chosen}) => {
        if (!this.#plugins.some((plugin) => plugin.layers?.length))
          return this.#call('render', page, chosen);
        // Page space: points, y up. Text and images stand upright in it.
        const layers = {};
        for (const plugin of this.#each()) {
          for (const layer of plugin.layers ?? []) {
            const canvas = new Canvas();
            await layer.draw(canvas, {page, scale: chosen.scale, rotation: chosen.rotation});
            const position = layer.position === 'below' ? 'below' : 'above';
            layers[position] = [...(layers[position] ?? []), ['save'], ...canvas.operations, ['restore']];
          }
        }
        return this.#call('render', page, chosen, layers);
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
    // What a plugin reads on a page without text of its own: [{text, box}].
    async #ocrLines(index) {
      if (!this.#plugins.some((plugin) => plugin.ocr)) return null;
      if (this.#ocr.has(index)) return this.#ocr.get(index);
      if (await this.#call('text', index)) { this.#ocr.set(index, null); return null; }
      this.#ocr.set(index, null);
      const image = await this.#call('render', index, {scale: 2, rotation: 0, flags: 0, background: 0xffffffff, x: 0, y: 0, width: 0, height: 0});
      let lines = null;
      for (const plugin of this.#each()) {
        lines = await plugin.ocr?.(index, image);
        if (lines) break;
      }
      this.#ocr.set(index, lines ?? null);
      return lines ?? null;
    }
    async text(index) {
      const text = await this.#call('text', index);
      if (text) return text;
      const lines = await this.#ocrLines(index);
      return lines ? lines.map((line) => line.text).join('\n') : text;
    }
    textLayout(index) { return this.#call('textLayout', index); }
    async textLines(index) {
      const lines = await this.#call('textLines', index);
      if (lines.length) return lines;
      const read = await this.#ocrLines(index);
      return read ? read.map(({text, box}) => ({text, box, unmapped: 0, ocr: true})) : lines;
    }
    images(index) { return this.#call('images', index); }
    // {data, type}: the JPEG as stored, or the pixels as PNG.
    imageData(index, image) { return this.#call('imageData', index, image); }
    textRects(index, start, count) { return this.#call('textRects', index, start, count); }
    charAt(index, x, y, tolerance = 2) { return this.#call('charAt', index, x, y, tolerance); }
    async search(query, {matchCase = false, wholeWord = false, first = 0, limit = 0} = {}) {
      const found = await this.#call('search', String(query), (matchCase ? 1 : 0) | (wholeWord ? 2 : 0), first, limit);
      if (!this.#plugins.some((plugin) => plugin.ocr)) return found;
      const pages = (await this.#call('info')).pages;
      const wanted = matchCase ? String(query) : String(query).toLowerCase();
      for (let page = first; page < pages; ++page) {
        for (const line of (await this.#ocrLines(page)) ?? []) {
          const text = matchCase ? line.text : line.text.toLowerCase();
          for (let at = text.indexOf(wanted); at >= 0; at = text.indexOf(wanted, at + 1))
            found.push({page, start: at, count: wanted.length, rects: [line.box], context: line.text, ocr: true});
        }
      }
      found.sort((a, b) => a.page - b.page);
      return limit > 0 ? found.slice(0, limit) : found;
    }
    outline() { return this.#call('outline'); }
    setOutline(items) { return this.#call('setOutline', items); }
    // The digital signatures: field, subFilter, reason, time, byteRange,
    // docMDP, signer, and contents (the PKCS#7) as bytes.
    async signatures() {
      const signatures = await this.#call('signatures');
      for (const signature of signatures) {
        for (const plugin of this.#each()) {
          const trust = await plugin.trust?.(signature);
          if (trust === true || trust === false) {
            signature.trusted = trust;
            break;
          }
        }
      }
      return signatures;
    }
    links(index) { return this.#call('links', index); }
    annotations(index) { return this.#call('annotations', index); }
    // annotations() of each of the pages, in one call.
    annotationsOfPages(pages) { return this.#call('annotationsOfPages', [...pages]); }
    addAnnotation(index, annotation) {
      return this.#around('annotation.add', {page: index, annotation}, async ({page, annotation: chosen}) => {
        await this.#checkData(chosen.type, chosen.data);
        await this.#wantFonts(chosen);
        const at = await this.#call('addAnnotation', page, chosen);
        await this.#redraw(page, at);
        return at;
      });
    }
    updateAnnotation(index, annotation, changes) {
      return this.#around('annotation.update', {page: index, index: annotation, annotation: changes},
                          async ({page, index: at, annotation: chosen}) => {
        const list = await this.#call('annotations', page);
        if (list[at]?.type && this.#kinds.has(list[at].type) && chosen.data)
          await this.#checkData(list[at].type, {...list[at].data, ...chosen.data});
        await this.#wantFonts({...chosen, type: list[at]?.type});
        await this.#call('updateAnnotation', page, at, chosen);
        await this.#redraw(page, at);
      });
    }
    removeAnnotation(index, annotation) {
      return this.#around('annotation.remove', {page: index, index: annotation},
                          ({page, index: at}) => this.#call('removeAnnotation', page, at));
    }
    // The top annotation at a point, or -1.
    async annotationAt(index, x, y, tolerance = 2) {
      if (![...this.#kinds.values()].some((kind) => kind.hitTest))
        return this.#call('annotationAt', index, x, y, tolerance);
      const list = await this.#call('annotations', index);
      for (let i = list.length - 1; i >= 0; --i) {
        const a = list[i], r = a.rect;
        if (!r || a.type === 'popup' || (a.flags & 35)) continue;
        if (x < r[0] - tolerance || x > r[2] + tolerance || y < r[1] - tolerance || y > r[3] + tolerance) continue;
        const kind = this.#kinds.get(a.type);
        if (!kind?.hitTest || await kind.hitTest(a, x, y)) return i;
      }
      return -1;
    }
    moveAnnotation(index, annotation, to) {
      return this.#around('annotation.move', {page: index, index: annotation, to},
                          ({page, index: at, to: target}) => this.#call('moveAnnotation', page, at, target));
    }
    // To a page of this or another open document. Its index there.
    copyAnnotation(index, annotation, target, targetPage) {
      return this.#call('copyAnnotation', target.#id, index, annotation, targetPage);
    }
    flattenAnnotation(index, annotation) {
      return this.#around('annotation.flatten', {page: index, index: annotation},
                          ({page, index: at}) => this.#call('flattenAnnotation', page, at));
    }
    // A page of another open document, as vectors, the appearance of a stamp.
    stampFromPage(index, annotation, source, sourcePage) {
      return this.#call('stampFromPage', source.#id, index, annotation, sourcePage);
    }
    attachFile(index, annotation, name, bytes) {
      return this.#call('attachFile', index, annotation, String(name), bytes);
    }
    // {name, data}.
    annotationFile(index, annotation) { return this.#call('annotationFile', index, annotation); }
    // A Canvas of the annotation's size, or its operations, as its appearance.
    drawAnnotation(index, annotation, canvas) {
      return this.#call('drawAnnotation', index, annotation, canvas.operations ?? canvas);
    }
    // What activating it means; plugin.action(action) true means it is done.
    async annotationAction(index, annotation) {
      const action = await this.#call('annotationAction', index, annotation);
      for (const plugin of this.#each()) {
        if (await plugin.action?.(action)) {
          action.handled = true;
          break;
        }
      }
      return action;
    }
    // 'xfdf' or 'fdf': the bytes of the file; of a page, or one annotation of it.
    exportAnnotations(format = 'xfdf', {page = -1, index = -1} = {}) {
      return this.#call('exportAnnotations', String(format), page, index);
    }
    async importAnnotations(bytes, format = 'xfdf') {
      const count = await this.#call('importAnnotations', String(format), bytes);
      // A kind's appearance is the script's to draw.
      if (this.#kinds.size) {
        const pages = (await this.#call('info')).pages;
        for (let page = 0; page < pages; ++page) {
          const list = await this.#call('annotations', page);
          for (let i = 0; i < list.length; ++i) {
            if (this.#kinds.get(list[i].type)?.draw && !list[i].appearances?.length)
              await this.#redraw(page, i);
          }
        }
      }
      await this.#tellAfter();
      return count;
    }
    // A stamp's appearance as an image, {width, height, data} in RGBA.
    setAnnotationImage(index, annotation, {width, height, data}) {
      return this.#call('setAnnotationImage', index, annotation, {width, height, data});
    }
    flatten(index, {print = false} = {}) {
      return this.#around('flatten', {page: index, print},
                          ({page, print: forPrint}) => this.#call('flatten', page, forPrint));
    }
    // Takes what is under the areas ([[l, b, r, t], ...]; null for the
    // page's redaction marks) out of the page for good, a box of fill
    // (0xRRGGBBAA, 0 for none) over each. The number of areas.
    redact(index, areas = null, fill = 0x000000ff) {
      return this.#around('redact', {page: index, areas: areas ? [...areas].map((a) => [...a]) : null, fill},
                          ({page, areas: chosen, fill: color}) => this.#call('redact', page, chosen, color));
    }
    attachments() { return this.#call('attachments'); }
    attachment(index) { return this.#call('attachment', index); }
    attach(name, bytes) { return this.#call('attach', String(name), bytes); }
    detach(index) { return this.#call('detach', index); }
    save({incremental = false, noSecurity = false} = {}) {
      return this.#around('save', {flags: (incremental ? 1 : 0) | (noSecurity ? 2 : 0)},
                          ({flags}) => this.#call('save', flags));
    }
    formFields() { return this.#call('formFields'); }
    setField(name, value) {
      return this.#around('form.set', {name: String(name), value: String(value)},
                          ({name: n, value: v}) => this.#call('setField', String(n), String(v)));
    }
    resetForm() { return this.#around('form.reset', {}, () => this.#call('resetForm')); }
    rotatePage(index, rotation) {
      return this.#around('page.rotate', {page: index, rotation},
                          ({page, rotation: r}) => this.#call('rotatePage', page, r));
    }
    deletePages(pages) {
      return this.#around('page.delete', {pages: [...pages]}, ({pages: p}) => {
        this.#ocr.clear();
        return this.#call('deletePages', p);
      });
    }
    movePages(pages, to) {
      return this.#around('page.move', {pages: [...pages], to}, ({pages: p, to: t}) => {
        this.#ocr.clear();
        return this.#call('movePages', p, t);
      });
    }
    insertPage(at, width, height) {
      return this.#around('page.insert', {at, width, height}, ({at: a, width: w, height: h}) => {
        this.#ocr.clear();
        return this.#call('insertPage', a, w, h);
      });
    }
    // Copies pages (all when null) of another open document in, at index at.
    importPages(from, pages, at) {
      return this.#around('page.import', {pages: pages ? [...pages] : null, at}, ({pages: p, at: a}) => {
        this.#ocr.clear();
        return this.#call('importPages', from.#id, p, a);
      });
    }
    close() {
      this.#closed = true;
      return this.#call('close');
    }
  }
  const started = async (id, plugins) => {
    const doc = new PdfDocument(id, plugins);
    await doc._start();
    for (const plugin of plugins) await eventHook(plugin, 'after', 'open')?.({event: 'open'});
    return doc;
  };
  return Object.freeze({
    // Bytes; in the application's script also a file's path.
    open: async (source, {password = null} = {}) => {
      const plugins = [...allPlugins];
      let current = {event: 'open', password,
                     ...(typeof source === 'string' ? {path: source} : {size: source?.length ?? source?.byteLength ?? 0})};
      for (const plugin of plugins) {
        const hook = eventHook(plugin, 'before', 'open');
        if (hook) current = decided(await hook(current), current, 'open');
      }
      return started(await send('pdf', 'open', [source, current.password ?? null]), plugins);
    },
    // An empty document, to build from others' pages.
    create: async () => started(await send('pdf', 'create', []), [...allPlugins]),
    // A plugin for every document opened from now on (a document's own
    // use() is for one): {kinds, before, after, appearance, layers, ocr, font,
    // trust, action, filters}. Returns a function that removes it.
    use: (plugin) => {
      if (!plugin || typeof plugin !== 'object') throw new TypeError('a plugin is an object');
      allPlugins.push(plugin);
      return () => {
        const at = allPlugins.indexOf(plugin);
        if (at >= 0) allPlugins.splice(at, 1);
      };
    },
    Canvas,
    canvas: Object.freeze({create: (width, height) => new Canvas(width, height)}),
    text: Object.freeze({
      // Shapes a line: font's bytes, size, text, {language, direction}.
      shape: (font, size, text, options = {}) => send('text', 'shape', [font, size, String(text), options]),
    }),
  });
}))js";

}  // namespace zebkit

#endif  // ZEBKIT_ENGINE_PDF_SCRIPT_H_
