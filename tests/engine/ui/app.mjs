// zebkit.ui: a window of Chromium's views, built and kept up to date by
// signals, driven as a user would (click, type, submit) and read back laid
// out. Run by zebkit-js (scripts/engine-test.sh); throws when a check fails.
import {check, results} from '../script/helpers.mjs';

const {h, signal, computed, Show, For, window, dump, click, type, submit, flush, classes} = zebkit.ui;

// Every node with its children, by id.
const find = (tree, id) => tree.id === id ? tree : tree.children.map((c) => find(c, id)).find(Boolean);

let counterText, addButton, input, echo, list, toggle, row, first, second;
const [count, setCount] = signal(0);
const [name, setName] = signal('');
const [submitted, setSubmitted] = signal('');
const [items, setItems] = signal(['a', 'b']);
const [details, setDetails] = signal(false);
const doubled = computed(() => count() * 2);

function App() {
  return h('view', {class: 'flex-col gap-2 p-4 bg-white'},
    h('view', {class: 'flex-row gap-4 items-center', ref: (n) => (row = n)},
      h('text', {ref: (n) => (counterText = n), class: 'text-lg font-bold'}, 'count: ', count, ', doubled: ', doubled),
      h('button', {ref: (n) => (addButton = n), onClick: () => setCount((c) => c + 1)}, 'Add')),
    h('view', {class: 'flex-row gap-2'},
      h('view', {ref: (n) => (first = n), width: 40, height: 20, class: 'bg-blue-500'}),
      h('view', {ref: (n) => (second = n), width: 60, height: 20, class: 'bg-gray-200'})),
    h('input', {ref: (n) => (input = n), placeholder: 'Your name',
      onInput: (value) => setName(value), onSubmit: (value) => setSubmitted(value)}),
    h('text', {ref: (n) => (echo = n)}, () => `hello, ${name() || 'nobody'}${submitted() ? ' (sent)' : ''}`),
    h('button', {ref: (n) => (toggle = n), onClick: () => setDetails((d) => !d)}, () => details() ? 'Hide' : 'Show'),
    h(Show, {when: details, fallback: () => h('text', {}, 'no details')},
      () => h('text', {class: 'text-gray-500'}, 'the details')),
    h('view', {ref: (n) => (list = n), class: 'flex-col'},
      h(For, {each: items}, (item) => h('text', {}, `item ${item}`))));
}

const win = window({title: 'zebkit.ui', width: 480, height: 360, onClose: () => console.log('window closed')}, App);

const texts = (node) => {
  const out = [];
  const walk = (n) => {
    if (n.visible === false) return;
    if (n.type === 'text') out.push(n.text);
    n.children.forEach(walk);
  };
  walk(dump(node));
  return out;
};

await check('a window of views', () => {
  const tree = dump(win.node);
  return tree.type === 'window' && tree.title === 'zebkit.ui' && tree.children.length === 1;
});
await check('text from signals', () => dump(counterText).text === 'count: 0, doubled: 0');
await check('a click updates what reads the signal', () => {
  click(addButton);
  click(addButton);
  return dump(counterText).text === 'count: 2, doubled: 4' && count() === 2;
});
await check('flexbox: a row with a gap', () => {
  const [a, b] = [dump(first).bounds, dump(second).bounds];
  return a[2] === 40 && b[2] === 60 && b[0] === a[0] + 40 + 8 && a[1] === b[1];
});
await check('padding from classes', () => dump(row).bounds[0] === 16 && dump(row).bounds[1] === 16);
await check('typing into an input', () => {
  type(input, 'Ada');
  return name() === 'Ada' && dump(input).value === 'Ada' && dump(echo).text === 'hello, Ada';
});
await check('submitting an input', () => {
  submit(input);
  return submitted() === 'Ada' && dump(echo).text === 'hello, Ada (sent)';
});
await check('Show: a fallback, then the children', () => {
  const before = texts(win.node).includes('no details');
  click(toggle);
  const after = texts(win.node);
  return before && after.includes('the details') && !after.includes('no details') && dump(toggle).text === 'Hide';
});
await check('For: a keyed list, added to and reordered', () => {
  const first = texts(list).join();
  setItems(['b', 'c', 'a']);
  flush();
  return first === 'item a,item b' && texts(list).join() === 'item b,item c,item a';
});
await check('a capture is what the compositor shows', async () => {
  const png = await zebkit.ui.capture(win.node);
  const image = await zebkit.image.decode(png);
  const [, , width, height] = dump(win.node).bounds;
  return image.width === width && image.height === height && width > 400;
});
await check('an unknown class is refused', () => {
  try { classes('not-a-class'); return false; } catch (e) { return /unknown class/.test(e.message); }
});

win.close();
const failed = results.filter(([, outcome]) => outcome !== true);
console.log(`${results.length - failed.length} of ${results.length} checks passed`);
if (failed.length) throw new Error(`${failed.length} checks failed`);
