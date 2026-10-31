// The plugin of plugin.c for zebkit.pdf.use(): a kind of annotation of its own,
// an undo history and text for pages without any.

export function samplePlugin() {
  const undo = [], redo = [];
  let applying = 0, redoing = false;
  return {
    name: 'sample',
    undo, redo,
    kinds: {
      Sample_Sticker: {
        prefix: 'Sample_',
        schema: {mood: {type: 'string', enum: ['happy', 'sad', ''], required: true},
                 size: {type: 'number', min: 1, max: 9}},
        check: (data) => (data.mood === '' ? "a sticker's mood is not empty" : null),
        draw(canvas, annotation, {width, height}) {
          const radius = Math.min(width, height) / 2;
          const sad = annotation.data?.mood === 'sad';
          canvas.fillColor(0xffdd00ff).beginPath().arc(width / 2, height / 2, radius, 0, 360).fill();
          canvas.strokeColor(sad ? 0x0000ffff : 0xff0000ff).lineWidth(radius / 5).beginPath();
          if (sad) canvas.arc(width / 2, height / 2 + radius * 0.8, radius * 0.5, 200, 340);
          else canvas.arc(width / 2, height / 2, radius * 0.5, 20, 160);
          canvas.stroke();
        },
        hitTest(annotation, x, y) {
          const [l, b, r, t] = annotation.rect;
          return Math.hypot(x - (l + r) / 2, y - (b + t) / 2) <= (r - l) / 2;
        },
      },
    },
    after: {
      '*'(operation) {
        if (!operation.inverse) return;
        if (applying && !redoing) redo.push(operation.inverse);
        else {
          undo.push(operation.inverse);
          if (!applying) redo.length = 0;
        }
      },
    },
    ocr: (page, image) => (image.width > 0 ? [{text: `read from page ${page}`, box: [10, 10, image.width / 2, 30]}] : null),
    async undoLast(doc) {
      const step = undo.pop();
      if (!step) return false;
      ++applying;
      try { await doc.apply(step); } finally { --applying; }
      return true;
    },
    async redoLast(doc) {
      const step = redo.pop();
      if (!step) return false;
      ++applying;
      redoing = true;
      try { await doc.apply(step); } finally { --applying; redoing = false; }
      return true;
    },
  };
}
