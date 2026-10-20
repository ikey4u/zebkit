// A to-do list in native widgets: zebkit.ui, Chromium's views, driven by
// signals. No web page, no DOM.
//
//   zebkit-js examples/ui/todo.mjs
//   zebkit-js --headless examples/ui/todo.mjs --capture todo.png
const {h, signal, computed, For, Show, window, capture, click, type, submit} = zebkit.ui;

const [todos, setTodos] = signal([]);
const [draft, setDraft] = signal('');
const left = computed(() => todos().filter((t) => !t.done).length);
let nextId = 1;

const add = () => {
  const text = draft().trim();
  if (!text) return;
  setTodos((list) => [...list, {id: nextId++, text, done: false}]);
  setDraft('');
};
const toggle = (todo) => setTodos((list) => list.map((t) => t === todo ? {...t, done: !t.done} : t));

let input, addButton, root;
function App() {
  return h('view', {class: 'flex-col gap-3 p-6 bg-gray-50', ref: (n) => (root = n)},
    h('text', {class: 'text-2xl font-bold text-gray-900'}, 'Things to do'),
    h('view', {class: 'flex-row gap-2 items-center'},
      h('input', {class: 'grow', ref: (n) => (input = n), placeholder: 'What needs doing?',
        value: draft, onInput: setDraft, onSubmit: add}),
      h('button', {ref: (n) => (addButton = n), onClick: add}, 'Add')),
    h(Show, {when: () => todos().length === 0,
             fallback: () => h('view', {class: 'flex-col gap-1'},
               h(For, {each: todos}, (todo) =>
                 h('view', {class: 'flex-row gap-2 items-center p-2 bg-white rounded-md border'},
                   h('button', {onClick: () => toggle(todo)}, todo.done ? 'Undo' : 'Done'),
                   h('text', {class: todo.done ? 'text-gray-400' : 'text-gray-800'}, todo.text))))},
      () => h('text', {class: 'text-gray-500'}, 'Nothing yet.')),
    h('text', {class: 'text-sm text-gray-500'}, () => `${left()} left`));
}

const win = window({title: 'To do', width: 420, height: 440}, App);

const captureTo = zebkit.args.indexOf('--capture');
if (captureTo >= 0) {
  // A scripted session, and a picture of it.
  for (const item of ['Write the roadmap', 'Build zebkit.ui', 'Ship it']) {
    type(input, item);
    submit(input);
  }
  await zebkit.fs.writeFile(zebkit.args[captureTo + 1], await capture(win.node));
  win.close();
}
