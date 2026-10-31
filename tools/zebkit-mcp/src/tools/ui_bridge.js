// The server's side of zebkit.ui, in a script of the engine's. Windows are built
// from a tree of plain data, checked here: an agent's JSON is never code, and it
// reaches nothing else of what this script could do.
(() => {
  const {h, window: openWindow, dump, click, type, submit, capture} = zebkit.ui;
  const windows = new Map();
  let next = 0;
  const ELEMENTS = new Set(['view', 'text', 'button', 'input', 'image', 'scroll']);
  const PROPERTIES = new Set(['class', 'direction', 'gap', 'padding', 'align', 'justify', 'grow', 'width', 'height',
    'background', 'radius', 'border', 'color', 'fontSize', 'fontWeight', 'textAlign', 'wrap', 'visible', 'enabled',
    'text', 'value', 'placeholder', 'password', 'tooltip']);
  const decode = (base64) => {
    const binary = atob(base64);
    const bytes = new Uint8Array(binary.length);
    for (let i = 0; i < binary.length; ++i) bytes[i] = binary.charCodeAt(i);
    return bytes;
  };
  const encode = (bytes) => {
    let binary = '';
    for (let i = 0; i < bytes.length; i += 8192) binary += String.fromCharCode(...bytes.subarray(i, i + 8192));
    return btoa(binary);
  };
  const build = (spec, record, depth, count) => {
    if (typeof spec === 'string') return spec;
    if (typeof spec !== 'object' || spec === null || Array.isArray(spec)) throw new Error('a node is an object or text');
    if (depth > 32 || ++count.nodes > 2000) throw new Error('the tree is too large');
    if (spec.type === 'web') throw new Error('a web element is refused here: use page_open, which holds pages to the origins allowed');
    if (!ELEMENTS.has(spec.type)) throw new Error(`no element ${String(spec.type)}`);
    const props = {};
    for (const [key, value] of Object.entries(spec)) {
      if (key === 'type' || key === 'children' || key === 'name') continue;
      if (key === 'src' && spec.type === 'image') { props.src = decode(String(value)); continue; }
      if (!PROPERTIES.has(key)) throw new Error(`no property ${key} on ${spec.type}`);
      if (typeof value === 'object') throw new Error(`${key}: a text, number or boolean`);
      props[key] = value;
    }
    const name = typeof spec.name === 'string' ? spec.name : '';
    if (spec.type === 'button') props.onClick = () => record({event: 'click', name});
    if (spec.type === 'input') {
      props.onInput = (value) => record({event: 'input', name, value});
      props.onSubmit = (value) => record({event: 'submit', name, value});
    }
    const children = (spec.children ?? []).map((child) => build(child, record, depth + 1, count));
    return h(spec.type, props, ...children);
  };
  const entry = (id) => {
    const found = windows.get(id);
    if (!found) throw new Error(`no window ${id}; ui_tree without a window lists them`);
    return found;
  };
  const has = (tree, node) => tree.id === node || (tree.children ?? []).some((child) => has(child, node));
  const owned = (id, node) => {
    const found = entry(id);
    if (!has(dump(found.win.node), node)) throw new Error(`no node ${node} in window ${id}`);
    return {id: node};
  };
  return Object.freeze({
    open(spec) {
      if (windows.size >= 16) throw new Error('at most 16 windows at once');
      const id = `w${++next}`;
      const events = [];
      const record = (event) => { if (events.length < 1000) events.push(event); };
      const count = {nodes: 0};
      const content = build(spec.content, record, 0, count);
      const win = openWindow({title: String(spec.title ?? ''), width: spec.width ?? 480, height: spec.height ?? 360,
        onClose: () => windows.delete(id)}, () => content);
      windows.set(id, {win, events});
      return {window: id, tree: dump(win.node)};
    },
    list: () => ({windows: [...windows.keys()]}),
    tree: (id) => ({window: id, tree: dump(entry(id).win.node)}),
    click(id, node) { click(owned(id, node)); return this.tree(id); },
    type(id, node, text) { type(owned(id, node), text); return this.tree(id); },
    submit(id, node) { submit(owned(id, node)); return this.tree(id); },
    events(id) { const found = entry(id); return {events: found.events.splice(0)}; },
    close(id) { entry(id).win.close(); windows.delete(id); return {closed: id}; },
    async capture(id, node) {
      const bytes = await capture(node === null ? entry(id).win.node : owned(id, node));
      return {base64: encode(bytes)};
    },
  });
})()
