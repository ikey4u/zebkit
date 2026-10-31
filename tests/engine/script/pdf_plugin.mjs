// zebkit.canvas, zebkit.text and zebkit.pdf.use() in the application's script:
// the example plugin, the same as C's, and each kind of hook. The font's path
// comes last in zebkit.args.

import {samplePlugin} from '../../../examples/pdf-plugin/plugin.mjs';

let failures = 0, checks = 0;
const check = (ok, what, detail = '') => {
  ++checks;
  if (!ok) ++failures;
  console.log(`${ok ? 'ok  ' : 'FAIL'} ${what}${detail === '' ? '' : `: ${detail}`}`);
};
const refuses = async (promise, what) => {
  try {
    await promise;
    check(false, what, 'it did not fail');
  } catch (e) {
    check(true, what, e.message);
  }
};

const fontPath = zebkit.args[zebkit.args.length - 1];
const font = await zebkit.fs.readFile(fontPath);
const blank = async (width = 300, height = 300) => {
  const doc = await zebkit.pdf.create();
  await doc.insertPage(0, width, height);
  return doc;
};
const pixel = (image, x, y) => {
  const at = ((image.height - 1 - Math.floor(y)) * image.width + Math.floor(x)) * 4;
  return [image.data[at], image.data[at + 1], image.data[at + 2]];
};
const near = (p, r, g, b, t = 40) => Math.abs(p[0] - r) <= t && Math.abs(p[1] - g) <= t && Math.abs(p[2] - b) <= t;
const white = (p) => p[0] > 240 && p[1] > 240 && p[2] > 240;
const same = (a, b) => a.width === b.width && a.height === b.height && a.data.every((v, i) => v === b.data[i]);

try {
// zebkit.canvas
{
  const canvas = zebkit.canvas.create(100, 100);
  canvas.fillColor([255, 0, 0]).rect(10, 10, 20, 20).fill();
  canvas.save().translate(50, 0).scale(2).fillColor(0x00ff00ff).beginPath().rect(0, 5, 10, 10).fill().restore();
  canvas.strokeColor(0x0000ffff).lineWidth(4).beginPath().moveTo(10, 50).lineTo(90, 50).stroke();
  canvas.linearGradient(50, 0, 90, 0, [0, 1], [0xff0000ff, 0x0000ffff]).beginPath().rect(50, 70, 40, 10).fill();
  canvas.drawImage({width: 1, height: 1, data: new Uint8Array([9, 9, 9, 255])}, 0, 80, 20, 20);
  canvas.font(font, 20).fillColor(0x000000ff).fillText('X', 40, 30);
  const {width, height, data} = await canvas.pixels();
  const at = (x, y) => [...data.subarray((y * width + x) * 4, (y * width + x) * 4 + 4)];
  check(width === 100 && height === 100, 'canvas: pixels');
  check(near(at(15, 15), 255, 0, 0, 3), 'canvas: a fill');
  check(near(at(65, 15), 0, 255, 0, 3) && at(71, 31)[3] === 0, 'canvas: a matrix');
  check(near(at(50, 50), 0, 0, 255, 3), 'canvas: a stroke');
  check(at(52, 75)[0] > 200 && at(88, 75)[2] > 200, 'canvas: a gradient');
  check(near(at(10, 90), 9, 9, 9, 3), 'canvas: an image');
  check(near(at(45, 20), 0, 0, 0, 3) && at(45, 5)[3] === 0, 'canvas: text in a font from bytes');
  const png = await canvas.png();
  const decoded = await zebkit.image.decode(png);
  check(decoded.width === 100 && decoded.data[(15 * 100 + 15) * 4] === 255, 'canvas: PNG out, decoded');
  await refuses(zebkit.canvas.create(0, 0).pixels(), 'canvas: no size');
  const wrong = zebkit.canvas.create(10, 10);
  await refuses(wrong.blend('Nope').pixels(), 'canvas: no such blend');
  await refuses(zebkit.canvas.create(10, 10).fillText('x', 0, 0).pixels(), 'canvas: text without a font');
}

// zebkit.text
{
  const shaped = await zebkit.text.shape(font, 20, 'AB', {language: 'en'});
  check(shaped.direction === 'ltr' && shaped.glyphs.length === 2 && shaped.advance[0] === 40 && shaped.glyphs[1].cluster === 1,
        'text: shaped', JSON.stringify(shaped.advance));
  const rtl = await zebkit.text.shape(font, 20, 'AB', {direction: 'rtl'});
  check(rtl.direction === 'rtl' && rtl.glyphs[0].cluster === 1, 'text: right to left');
  await refuses(zebkit.text.shape(new Uint8Array(4), 20, 'a'), 'text: what is not a font');
  await refuses(zebkit.text.shape(font, -1, 'a'), 'text: a size below 0');
  await refuses(zebkit.text.shape(font, 20, 'a', {direction: 'up'}), 'text: no such direction');
}

// New keys, from JavaScript too.
{
  const doc = await blank();
  await doc.addFont('Ahem', font);
  const line = await doc.addAnnotation(0, {type: 'line', line: [50, 100, 250, 100], color: [255, 0, 0], border: 2,
                                           lineEndings: ['none', 'closedArrow'], interiorColor: [0, 0, 255]});
  await doc.addAnnotation(0, {type: 'stamp', rect: [50, 200, 250, 280], stamp: 'Approved'});
  const note = await doc.addAnnotation(0, {type: 'freeText', rect: [10, 10, 200, 60], contents: 'X', font: 'Ahem', fontSize: 20, textColor: [0, 128, 0]});
  const image = await doc.render(0);
  check(near(pixel(image, 150, 100), 255, 0, 0) && near(pixel(image, 243, 100), 0, 0, 255), 'annotations: a line and its ending');
  check(!white(pixel(image, 150, 201)), 'annotations: a stamp');
  check(near(pixel(image, 20, 45), 0, 128, 0), 'annotations: free text in a given font');
  check(await doc.annotationAt(0, 150, 100, 2) === line, 'annotations: the one at a point');
  await doc.moveAnnotation(0, note, 0);
  check((await doc.annotations(0))[0].type === 'freeText', 'annotations: reordered');
  const file = await doc.addAnnotation(0, {type: 'fileAttachment', rect: [270, 270, 290, 290], icon: 'Paperclip'});
  await doc.attachFile(0, file, 'a.txt', new TextEncoder().encode('hello'));
  const again = await zebkit.pdf.open(await doc.save());
  const got = await again.annotationFile(0, file);
  check(got.name === 'a.txt' && new TextDecoder().decode(got.data) === 'hello', 'annotations: a file through a save');
  const xfdf = await doc.exportAnnotations('xfdf');
  const copy = await blank();
  check(await copy.importAnnotations(xfdf, 'xfdf') === 4, 'annotations: XFDF out and in');
  const fdf = await doc.exportAnnotations('fdf');
  const copy2 = await blank();
  check(await copy2.importAnnotations(fdf, 'fdf') === 4, 'annotations: FDF out and in');
  await refuses(copy.importAnnotations(new TextEncoder().encode('<!DOCTYPE a [<!ENTITY b "c">]><xfdf/>')), 'annotations: a DTD refused');
  await refuses(doc.addAnnotation(0, {type: 'line', line: [1, 1, 2, 2], lineEndings: ['nope', 'none']}), 'annotations: wrong values refused');
  await doc.flattenAnnotation(0, 1);
  const stamps = await blank();
  const stamp = await stamps.addAnnotation(0, {type: 'stamp', rect: [0, 0, 300, 300]});
  const source = await blank(100, 100);
  await source.addAnnotation(0, {type: 'square', rect: [0, 0, 100, 100], interiorColor: [0, 0, 255], border: 0});
  await source.flatten(0);
  await stamps.stampFromPage(0, stamp, source, 0);
  check(near(pixel(await stamps.render(0), 150, 150), 0, 0, 255), 'annotations: a stamp from a page');
  const copyAt = await doc.copyAnnotation(0, 0, copy, 0);
  check(copyAt === 4, 'annotations: copied to another document');
  const ops = zebkit.canvas.create(100, 100);
  ops.fillColor(0xff0000ff).rect(0, 0, 100, 100).fill();
  const drawn = await stamps.addAnnotation(0, {type: 'stamp', rect: [10, 10, 60, 60]});
  await stamps.drawAnnotation(0, drawn, ops);
  check(near(pixel(await stamps.render(0), 30, 30), 255, 0, 0), 'annotations: a canvas as an appearance');
}

// The example plugin: the kind, its data, the undo history, OCR.
{
  const doc = await blank();
  const plugin = samplePlugin();
  await doc.use(plugin);
  await refuses(doc.addAnnotation(0, {type: 'Sample_Sticker', rect: [50, 50, 150, 150], data: {}}), 'plugin: a sticker needs a mood');
  await refuses(doc.addAnnotation(0, {type: 'Sample_Sticker', rect: [50, 50, 150, 150], data: {mood: 'angry'}}), 'plugin: not one of the moods');
  await refuses(doc.addAnnotation(0, {type: 'Sample_Sticker', rect: [50, 50, 150, 150], data: {mood: ''}}), "plugin: the kind's own check");
  check((await doc.annotations(0)).length === 0, 'plugin: none added');
  const sticker = await doc.addAnnotation(0, {type: 'Sample_Sticker', rect: [50, 50, 150, 150], data: {mood: 'happy', size: 3}});
  let image = await doc.render(0);
  check(near(pixel(image, 100, 130), 255, 221, 0) && white(pixel(image, 53, 53)), 'plugin: its own appearance');
  check(near(pixel(image, 100, 75), 255, 0, 0), 'plugin: a smile, y down in the canvas');
  const [read] = await doc.annotations(0);
  check(read.type === 'Sample_Sticker' && read.data.mood === 'happy' && read.data.size === 3, 'plugin: its data');
  check(await doc.annotationAt(0, 100, 100, 0) === sticker && await doc.annotationAt(0, 53, 53, 0) === -1, 'plugin: its hit test');
  await refuses(doc.updateAnnotation(0, 0, {data: {mood: 'angry'}}), 'plugin: a change is checked');
  await doc.updateAnnotation(0, 0, {data: {mood: 'sad'}});
  check((await doc.annotations(0))[0].data.size === 3 && (await doc.annotations(0))[0].data.mood === 'sad', 'plugin: merged');

  const saved = await zebkit.pdf.open(await doc.save());
  const savedPlugin = samplePlugin();
  await saved.use(savedPlugin);
  const reread = (await saved.annotations(0))[0];
  check(reread.type === 'Sample_Sticker' && reread.data.mood === 'sad', 'plugin: after a save');
  check(near(pixel(await saved.render(0), 100, 140), 255, 221, 0), 'plugin: its appearance after a save');
  const xfdf = await doc.exportAnnotations('xfdf');
  const other = await blank();
  await refuses(other.importAnnotations(xfdf), 'plugin: no such kind where it is read');
  const otherPlugin = samplePlugin();
  await other.use(otherPlugin);
  check(await other.importAnnotations(xfdf) === 1, 'plugin: XFDF');
  check(near(pixel(await other.render(0), 100, 140), 255, 221, 0) && (await other.annotations(0))[0].data.mood === 'sad', 'plugin: drawn where it is read');
  check(await doc.copyAnnotation(0, 0, other, 0) === 1 && (await other.annotations(0))[1].data.mood === 'sad', 'plugin: a copy');
  await other.flatten(0);
  const kept = await other.pageData(0, 'Sample_');
  check(kept.length === 2 && kept[0].Sample_Data.mood === 'sad', 'plugin: flattened, its data stays');

  // The history.
  const undoable = await blank();
  const history = samplePlugin();
  await undoable.use(history);
  const empty = await undoable.render(0);
  await undoable.addAnnotation(0, {type: 'square', rect: [50, 50, 150, 150], interiorColor: [255, 0, 0], border: 0});
  const one = await undoable.render(0);
  await undoable.updateAnnotation(0, 0, {interiorColor: [0, 0, 255]});
  const two = await undoable.render(0);
  await undoable.removeAnnotation(0, 0);
  check(history.undo.length >= 3, 'plugin: the history grows', history.undo.length);
  check(await history.undoLast(undoable) && same(await undoable.render(0), two), 'plugin: undo a removal');
  check(await history.undoLast(undoable) && same(await undoable.render(0), one), 'plugin: undo a change');
  check(await history.undoLast(undoable) && same(await undoable.render(0), empty), 'plugin: undo an addition');
  check(await history.redoLast(undoable) && same(await undoable.render(0), one), 'plugin: redo');

  // OCR.
  const scanned = await blank();
  await scanned.use(samplePlugin());
  check(await scanned.text(0) === 'read from page 0', 'plugin: OCR text');
  const lines = await scanned.textLines(0);
  check(lines.length === 1 && lines[0].ocr === true, 'plugin: OCR lines');
  const found = await scanned.search('PAGE');
  check(found.length === 1 && found[0].ocr === true, 'plugin: OCR search');
  await saved.close();
  await other.close();
}

// Each kind of hook.
{
  const doc = await blank();
  const told = [];
  const before = {};
  const plugin = {
    before,
    after: {'*': (operation) => { told.push(operation); }},
  };
  const remove = await doc.use(plugin);
  // Refused: nothing happens, as C's.
  before['annotation.add'] = () => ({refuse: 'not today'});
  await refuses(doc.addAnnotation(0, {type: 'square', rect: [1, 1, 2, 2]}), 'hooks: refused by a hook');
  check((await doc.annotations(0)).length === 0, 'hooks: nothing added');
  before['annotation.add'] = () => false;
  await refuses(doc.addAnnotation(0, {type: 'square', rect: [1, 1, 2, 2]}), 'hooks: refused by false');
  // Changed.
  before['annotation.add'] = (operation) => ({...operation, annotation: {...operation.annotation, interiorColor: [0, 0, 255], border: 0}});
  await doc.addAnnotation(0, {type: 'square', rect: [10, 10, 50, 50], interiorColor: [255, 0, 0]});
  check(near(pixel(await doc.render(0), 30, 30), 0, 0, 255), 'hooks: changed by a hook');
  delete before['annotation.add'];
  check(told.some((o) => o.event === 'annotation.add' && o.inverse?.event === 'annotation.remove'), 'hooks: told, with an inverse');
  told.length = 0;
  // Every event, each in the same shape.
  const runs = [
    ['annotation.update', () => doc.updateAnnotation(0, 0, {contents: 'x'})],
    ['annotation.move', () => doc.moveAnnotation(0, 0, 0)],
    ['save', () => doc.save()],
    ['render', () => doc.render(0)],
    ['redact', () => doc.redact(0, [[200, 200, 210, 210]], 0)],
    ['page.insert', () => doc.insertPage(1, 100, 100)],
    ['page.move', () => doc.movePages([1], 0)],
    ['page.rotate', () => doc.rotatePage(1, 90)],
    ['flatten', () => doc.flatten(1)],
    ['page.delete', () => doc.deletePages([0])],
  ];
  for (const [event, run] of runs) {
    let asked = 0;
    before[event] = () => { ++asked; return {refuse: 'no'}; };
    await refuses(run(), `hooks: ${event} refused`);
    before[event] = () => { ++asked; };
    told.length = 0;
    await run();
    delete before[event];
    check(asked === 2 && told.some((o) => o.event === event), `hooks: ${event} before and after`, told.map((o) => o.event).join());
  }
  // Removing itself, closing: on the script's thread, in the same shape.
  remove();
  const closing = await blank();
  let later = 0;
  await closing.use({before: {'annotation.add': () => { closing.close(); }}, after: {'*': () => { ++later; }}});
  await refuses(closing.addAnnotation(0, {type: 'square', rect: [1, 1, 2, 2]}), 'hooks: a hook closed the document');
  check(later === 0, 'hooks: none told after it');
  const self = await blank();
  let calls = 0;
  const once = {before: {'annotation.add': () => { ++calls; self.unuse(once); }}};
  await self.use(once);
  await self.addAnnotation(0, {type: 'square', rect: [1, 1, 2, 2]});
  await self.addAnnotation(0, {type: 'square', rect: [1, 1, 2, 2]});
  check(calls === 1, 'hooks: a hook that removes itself');

  // zebkit.pdf.use: every document opened from now on; opening refused.
  const seen = [];
  const removeAll = zebkit.pdf.use({
    before: {open: (operation) => { seen.push(operation); }},
    after: {open: () => { seen.push('opened'); }},
  });
  const opened = await blank();
  check(seen.includes('opened'), 'hooks: for all documents');
  await opened.close();
  const refusing = zebkit.pdf.use({before: {open: () => ({refuse: 'closed for today'})}});
  await refuses(zebkit.pdf.open(await (await blank()).save()), 'hooks: opening refused');
  refusing();
  removeAll();

  // Appearance providers, layers, fonts, trust, actions, filters.
  const provided = await blank();
  await provided.use({
    appearance: {stamp: (canvas, annotation, {width, height}) => { canvas.fillColor(0x00ff00ff).rect(0, 0, width, height).fill(); }},
    layers: [{position: 'above', draw: (canvas) => { canvas.fillColor(0xff0000ff).rect(200, 200, 20, 20).fill(); }},
             {position: 'below', draw: (canvas) => { canvas.fillColor(0x0000ffff).rect(250, 250, 20, 20).fill(); }}],
    font: (family) => (family === 'Boxes' ? font : null),
    filters: {ink: (points) => [...points, [9, 9]]},
    action: (action) => action.kind === 'uri',
  });
  await provided.addAnnotation(0, {type: 'stamp', rect: [10, 10, 60, 60], stamp: 'Draft'});
  await provided.addAnnotation(0, {type: 'freeText', rect: [70, 10, 190, 60], contents: 'X', font: 'Boxes', fontSize: 20, textColor: [255, 0, 0]});
  await provided.addAnnotation(0, {type: 'link', rect: [1, 1, 9, 9], url: 'https://example.com/'});
  const shown = await provided.render(0);
  check(near(pixel(shown, 30, 30), 0, 255, 0), 'hooks: an appearance provider');
  check(near(pixel(shown, 210, 210), 255, 0, 0), 'hooks: a layer above, in page space');
  check(near(pixel(shown, 260, 260), 0, 0, 255), 'hooks: a layer below');
  check(near(pixel(shown, 80, 45), 255, 0, 0), 'hooks: a font from a plugin');
  const filtered = await provided.filterInput('ink', [[0, 0], [10, 0], [10, 10]], {smooth: 1});
  check(filtered.length === 7 && filtered[6][0] === 9, 'hooks: filters, built-in and a plugin', JSON.stringify(filtered));
  await refuses(provided.filterInput('ink', [[0, 0]], {smooth: 99}), 'hooks: a filter of wrong options');
  const action = await provided.annotationAction(0, 2);
  check(action.kind === 'uri' && action.handled === true, 'hooks: an action taken');
  const signed = await blank();
  await signed.use({trust: () => true});
  check((await signed.signatures()).length === 0, 'hooks: trust with nothing signed');
}
} catch (e) {
  check(false, 'an exception', e.stack || String(e));
}

console.log(`${checks - failures} of ${checks} checks passed`);
zebkit.quit();
