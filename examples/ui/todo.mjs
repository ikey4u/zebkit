// A day planner in native widgets: zebkit.ui, Chromium's views, driven by
// signals. No web page, no DOM, no CSS engine: about 120 lines of JavaScript.
//
//   zebkit-js examples/ui/todo.mjs
//   zebkit-js --headless examples/ui/todo.mjs --capture todo.png
const {h, signal, computed, For, Show, window, capture, click, type, submit} = zebkit.ui;

// The colors, in one place.
const ink = '#e6e8f2', muted = '#8a91aa', faint = '#4b5270';
const bg = '#0b1020', card = '#141a2e', line = '#252d4a', accent = '#7c5cff';
const priorities = {
  high: {label: 'High', fg: '#ffd5dc', bg: '#5a1f2e'},
  medium: {label: 'Medium', fg: '#ffe7b8', bg: '#4d3a14'},
  low: {label: 'Low', fg: '#c9f2d8', bg: '#173d2a'},
};

// The state: tasks, the draft, the priority for the next task, the filter.
const [tasks, setTasks] = signal([]);
const [draft, setDraft] = signal('');
const [priority, setPriority] = signal('medium');
const [filter, setFilter] = signal('all');
const done = computed(() => tasks().filter((t) => t.done).length);
const left = computed(() => tasks().length - done());
const shown = computed(() => tasks().filter((t) =>
  filter() === 'all' || (filter() === 'done') === t.done));
let nextId = 1;

const add = () => {
  const text = draft().trim();
  if (!text) return;
  setTasks((list) => [...list, {id: nextId++, text, done: false, priority: priority()}]);
  setDraft('');
};
const update = (task, change) => setTasks((list) => list.map((t) => t.id === task.id ? {...t, ...change} : t));
const remove = (task) => setTasks((list) => list.filter((t) => t.id !== task.id));

// A pill that is chosen or not: the priority picker and the filters.
const Chip = ({label, chosen, onClick}) =>
  h('button', {onClick, class: () => chosen()
    ? `rounded-full px-3 py-1 bg-[${accent}] text-white font-semibold`
    : `rounded-full px-3 py-1 bg-[${card}] text-[${muted}] border-[${line}]`}, label);

const BAR = 412;
const Progress = () =>
  h('view', {class: 'flex-col gap-2'},
    h('view', {class: `flex-row w-[${BAR}] h-2 rounded-full bg-[${line}]`},
      h('view', {class: `h-2 rounded-full bg-[${accent}]`,
                 width: () => tasks().length ? Math.max(8, BAR * done() / tasks().length) : 0})),
    h('text', {class: `text-sm text-[${muted}]`}, () => tasks().length
      ? `${Math.round(100 * done() / tasks().length)}% done · ${left()} to go`
      : 'Add your first task'));

const Task = (task) =>
  h('view', {class: `flex-row items-center gap-3 p-3 rounded-xl bg-[${card}] border-[${line}]`},
    h('button', {onClick: () => update(task, {done: !task.done}),
                 class: task.done ? `w-8 h-8 p-0 rounded-full bg-[${accent}] text-white text-lg font-bold`
                                  : `w-8 h-8 p-0 rounded-full bg-[${bg}] text-[${faint}] border-[${faint}]`},
      task.done ? '✓' : ''),
    h('text', {class: `grow text-base ${task.done ? `text-[${faint}]` : `text-[${ink}] font-medium`}`}, task.text),
    h('text', {class: `text-xs font-semibold rounded-full px-2 py-1 ` +
                      `bg-[${priorities[task.priority].bg}] text-[${priorities[task.priority].fg}]`},
      priorities[task.priority].label),
    h('button', {onClick: () => remove(task), class: `w-8 h-8 p-0 rounded-full text-[${muted}]`}, '✕'));

let input, root;
function App() {
  return h('view', {class: `flex-col gap-4 p-6 bg-[${bg}]`, ref: (n) => (root = n)},
    h('view', {class: 'flex-row items-center'},
      h('view', {class: 'flex-col grow'},
        h('text', {class: `text-sm font-semibold text-[${accent}]`}, 'TODAY'),
        h('text', {class: 'text-3xl font-bold text-white'}, 'Ship something great')),
      h('text', {class: `text-lg font-bold rounded-full px-3 py-1 bg-[${card}] text-[${ink}]`},
        () => `${done()}/${tasks().length}`)),
    Progress(),
    h('view', {class: 'flex-row gap-2 items-center'},
      h('input', {class: `grow rounded-lg px-3 py-2 bg-[${card}] text-[${ink}] border-[${line}]`,
                  ref: (n) => (input = n), placeholder: 'Add a task and press Enter',
                  value: draft, onInput: setDraft, onSubmit: add}),
      h('button', {onClick: add, class: `rounded-lg px-5 py-2 bg-[${accent}] text-white font-semibold`}, 'Add')),
    h('view', {class: 'flex-row gap-2 items-center'},
      h('text', {class: `text-sm text-[${muted}]`}, 'Priority'),
      ...Object.entries(priorities).map(([key, p]) =>
        Chip({label: p.label, chosen: () => priority() === key, onClick: () => setPriority(key)}))),
    h('view', {class: 'flex-col gap-2'},
      h(Show, {when: () => shown().length > 0,
               fallback: () => h('text', {class: `text-[${muted}] p-3`}, 'Nothing here. Enjoy the calm.')},
        () => h(For, {each: shown}, Task))),
    h('view', {class: 'flex-row gap-2 items-center'},
      ...[['all', 'All'], ['active', 'Active'], ['done', 'Done']].map(([key, label]) =>
        Chip({label, chosen: () => filter() === key, onClick: () => setFilter(key)})),
      h('view', {class: 'grow'}),
      h('button', {onClick: () => setTasks((list) => list.filter((t) => !t.done)),
                   class: `rounded-full px-3 py-1 text-[${muted}]`}, 'Clear done')));
}

const win = window({title: 'Today', width: 460, height: 680}, App);

const captureTo = zebkit.args.indexOf('--capture');
if (captureTo >= 0) {
  // A scripted session, as a person would use it, and a picture of it.
  for (const [text, level] of [['Fix the login bug', 'high'], ['Review the design', 'medium'],
                               ['Write the release notes', 'medium'], ['Book the team lunch', 'low'],
                               ['Ship 0.2.0', 'high']]) {
    setPriority(level);
    type(input, text);
    submit(input);
  }
  update(tasks()[1], {done: true});
  update(tasks()[3], {done: true});
  update(tasks()[4], {done: true});
  setPriority('medium');
  await zebkit.fs.writeFile(zebkit.args[captureTo + 1], await capture(win.node));
  win.close();
}
