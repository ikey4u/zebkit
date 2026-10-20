// The same interface as web.mjs, in native widgets (zebkit.ui): a list of
// N rows, each a text and a button, then every row updated once.
// scripts/ui-bench.sh runs both and compares them.
const n = Number(zebkit.args[0] || 1000);
const {h, signal, For, window, dump} = zebkit.ui;
const started = Date.now();
// A signal per row's label, as fine-grained updates are written.
const rows = Array.from({length: n}, (_, i) => {
  const [label, setLabel] = signal(`row ${i}`);
  return {id: i, label, setLabel};
});
let list;
const win = window({title: 'native', width: 600, height: 800}, () =>
  h('scroll', {height: 760},
    h('view', {class: 'flex-col gap-1 p-2', ref: (x) => (list = x)},
      h(For, {each: rows}, (row) => h('view', {class: 'flex-row gap-2 items-center'},
        h('button', {}, 'Open'), h('text', {}, row.label))))));
dump(win.node);  // laid out
const shown = Date.now() - started;
const updateStart = Date.now();
zebkit.ui.batch(() => { for (const row of rows) row.setLabel(`${row.label()}!`); });
dump(win.node);
const updated = Date.now() - updateStart;
console.log(`RESULT ${JSON.stringify({shown, updated})}`);
setTimeout(() => {}, 1e9);  // stay up to be measured
