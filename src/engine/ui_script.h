// zebkit.ui's script side: signals, elements and windows over the views
// of ui.cc. The model is Native Shell's and Solid's: components run once,
// signals update the properties that read them, and changes go to the
// native side as one batch of mount operations per turn of the loop.

#ifndef ZEBKIT_ENGINE_UI_SCRIPT_H_
#define ZEBKIT_ENGINE_UI_SCRIPT_H_

namespace zebkit {

// A function of the native object; returns zebkit.ui.
inline constexpr char kUiScript[] = R"js((function (native) {
  'use strict';

  // ---- Signals ----------------------------------------------------------

  let observer = null;  // the computation reading signals now
  let owner = null;     // the computation that owns what is made now
  let batching = 0;
  const queue = new Set();

  class Computation {
    constructor(fn, parent) {
      this.fn = fn;
      this.sources = new Set();
      this.children = [];
      this.cleanups = [];
      this.disposed = false;
      if (parent) parent.children.push(this);
    }
  }
  const clean = (c) => {
    for (const source of c.sources) source.observers.delete(c);
    c.sources.clear();
    for (const child of c.children.splice(0)) dispose(child);
    for (const cleanup of c.cleanups.splice(0)) cleanup();
  };
  const dispose = (c) => {
    clean(c);
    c.disposed = true;
  };
  const run = (c) => {
    clean(c);
    const [o, w] = [observer, owner];
    observer = owner = c;
    try { c.fn(); } finally { observer = o; owner = w; }
  };
  const flush = () => {
    while (queue.size) {
      const pending = [...queue];
      queue.clear();
      for (const c of pending) if (!c.disposed) run(c);
    }
  };

  // [read, write]: reading inside an effect makes it run again on a write.
  function signal(value, {equals = Object.is} = {}) {
    const state = {value, observers: new Set()};
    const read = () => {
      if (observer) {
        state.observers.add(observer);
        observer.sources.add(state);
      }
      return state.value;
    };
    const write = (next) => {
      if (typeof next === 'function') next = next(state.value);
      if (equals(state.value, next)) return;
      state.value = next;
      for (const o of state.observers) queue.add(o);
      if (!batching) flush();
    };
    return [read, write];
  }
  function effect(fn) {
    const c = new Computation(fn, owner);
    run(c);
    return () => dispose(c);
  }
  function computed(fn) {
    const [read, write] = signal(undefined);
    effect(() => write(fn()));
    return read;
  }
  function batch(fn) {
    ++batching;
    try { return fn(); } finally { if (--batching === 0) flush(); }
  }
  function untracked(fn) {
    const o = observer;
    observer = null;
    try { return fn(); } finally { observer = o; }
  }
  function onCleanup(fn) { if (owner) owner.cleanups.push(fn); }
  // An owner of its own, ended by the function it is handed.
  function root(fn) {
    const c = new Computation(() => {}, null);
    const [o, w] = [observer, owner];
    observer = null;
    owner = c;
    try { return fn(() => dispose(c)); } finally { observer = o; owner = w; }
  }

  // ---- Mount operations ---------------------------------------------------

  let nextId = 1;
  let operations = [];
  let scheduled = false;
  const commit = () => {
    scheduled = false;
    if (!operations.length) return;
    const pending = operations;
    operations = [];
    native.uiCommit(pending);
  };
  const operation = (...op) => {
    operations.push(op);
    if (!scheduled) {
      scheduled = true;
      Promise.resolve().then(commit);
    }
  };
  const handlers = new Map();
  native.uiListen((id, event, value) => {
    commit();
    const fn = handlers.get(id)?.[event];
    if (fn) batch(() => fn(value));
    commit();
  });

  // ---- Classes: a subset of Tailwind ------------------------------------

  const palette = {
    white: '#ffffff', black: '#000000', transparent: '#00000000',
    'gray-50': '#f9fafb', 'gray-100': '#f3f4f6', 'gray-200': '#e5e7eb', 'gray-300': '#d1d5db',
    'gray-400': '#9ca3af', 'gray-500': '#6b7280', 'gray-600': '#4b5563', 'gray-700': '#374151',
    'gray-800': '#1f2937', 'gray-900': '#111827',
    'red-500': '#ef4444', 'red-600': '#dc2626', 'green-500': '#22c55e', 'green-600': '#16a34a',
    'blue-50': '#eff6ff', 'blue-100': '#dbeafe', 'blue-500': '#3b82f6', 'blue-600': '#2563eb',
    'blue-700': '#1d4ed8', 'yellow-500': '#eab308', 'indigo-500': '#6366f1', 'indigo-600': '#4f46e5',
  };
  const sizes = {xs: 12, sm: 14, base: 16, lg: 18, xl: 20, '2xl': 24, '3xl': 30, '4xl': 36};
  const radii = {'': 4, sm: 2, md: 6, lg: 8, xl: 12, '2xl': 16, full: 9999, none: 0};
  const weights = {thin: 100, light: 300, normal: 400, medium: 500, semibold: 600, bold: 700, black: 900};
  const color = (name) => name.startsWith('[') ? name.slice(1, -1) : palette[name];
  const spacing = (n) => n.startsWith('[') ? parseFloat(n.slice(1, -1)) : parseFloat(n) * 4;
  // Properties from class names; an unknown one is an error, as Native
  // Shell's bundler makes it.
  function classProps(classes) {
    const props = {};
    let padding = [0, 0, 0, 0], padded = false;
    for (const name of String(classes).split(/\s+/).filter(Boolean)) {
      let m;
      if (name === 'flex-row') props.direction = 'row';
      else if (name === 'flex-col') props.direction = 'column';
      else if (name === 'grow' || name === 'flex-1') props.grow = 1;
      else if (name === 'hidden') props.visible = false;
      else if ((m = /^gap-(.+)$/.exec(name))) props.gap = spacing(m[1]);
      else if ((m = /^p([xytrbl]?)-(.+)$/.exec(name))) {
        const v = spacing(m[2]);
        const sides = {'': [0, 1, 2, 3], x: [1, 3], y: [0, 2], t: [0], r: [1], b: [2], l: [3]}[m[1]];
        for (const side of sides) padding[side] = v;
        padded = true;
      } else if ((m = /^items-(start|center|end|stretch)$/.exec(name))) props.align = m[1];
      else if ((m = /^justify-(start|center|end)$/.exec(name))) props.justify = m[1];
      else if ((m = /^bg-(.+)$/.exec(name)) && color(m[1])) props.background = color(m[1]);
      else if ((m = /^text-(xs|sm|base|lg|xl|2xl|3xl|4xl)$/.exec(name))) props.fontSize = sizes[m[1]];
      else if ((m = /^text-(left|center|right)$/.exec(name))) props.textAlign = m[1];
      else if ((m = /^text-(.+)$/.exec(name)) && color(m[1])) props.color = color(m[1]);
      else if ((m = /^font-(.+)$/.exec(name)) && weights[m[1]]) props.fontWeight = weights[m[1]];
      else if ((m = /^rounded(?:-(.+))?$/.exec(name)) && (m[1] ?? '') in radii) props.radius = radii[m[1] ?? ''];
      else if (name === 'border') props.border = {width: 1, color: palette['gray-300']};
      else if ((m = /^border-(.+)$/.exec(name)) && color(m[1])) props.border = {width: 1, color: color(m[1])};
      else if ((m = /^w-(.+)$/.exec(name))) props.width = spacing(m[1]);
      else if ((m = /^h-(.+)$/.exec(name))) props.height = spacing(m[1]);
      else throw new Error(`unknown class "${name}"`);
    }
    if (padded) props.padding = padding;
    return props;
  }

  // ---- Elements ------------------------------------------------------------

  const Fragment = Symbol('Fragment');
  const containers = new Set(['view', 'scroll', 'window']);
  const sizeOf = (props) => [props.width ?? null, props.height ?? null];

  function setProps(node, props) {
    const out = {};
    for (const [key, value] of Object.entries(props)) {
      if (key === 'class') Object.assign(out, classProps(value));
      else out[key] = value;
    }
    if (node.type !== 'window' && ('width' in out || 'height' in out)) {
      node.size = [out.width ?? node.size?.[0] ?? null, out.height ?? node.size?.[1] ?? null];
      out.size = node.size;
      delete out.width;
      delete out.height;
    }
    operation('set', node.id, out);
  }

  const flatten = (items) => items.flat(Infinity).filter((x) => x != null && x !== false && x !== true);

  // Nodes from what a component or a region gives: nodes, strings, arrays.
  function nodesOf(value) {
    return flatten([value]).map((item) => {
      if (typeof item === 'object' && item.id) return item;
      if (typeof item === 'function') return nodesOf(item());
      return h('text', {text: String(item)});
    }).flat();
  }

  // A region of a container whose content a function gives, kept up to
  // date: an invisible view marks where it is.
  function region(parent, fn) {
    const anchor = {id: nextId++, type: 'view'};
    operation('create', anchor.id, 'view');
    operation('set', anchor.id, {visible: false});
    operation('append', parent.id, anchor.id, 0);
    let current = [];
    effect(() => {
      const next = nodesOf(fn());
      const keep = new Set(next);
      for (const node of current) if (!keep.has(node)) operation('destroy', node.id);
      for (const node of next) operation('append', parent.id, node.id, anchor.id);
      current = next;
    });
    onCleanup(() => { for (const node of current) operation('destroy', node.id); });
  }

  function appendChildren(node, children) {
    for (const child of flatten(children)) {
      if (typeof child === 'function') region(node, child);
      else for (const n of nodesOf(child)) operation('append', node.id, n.id, 0);
    }
  }

  // An element: h('view', {class, ...props, onClick}, ...children). A
  // property given as a function is read in an effect and kept up to date.
  function h(type, props, ...children) {
    props = props || {};
    if (props.children !== undefined) children = [props.children, ...children];
    if (typeof type === 'function') {
      return untracked(() => type({...props, children: children.length === 1 ? children[0] : children}));
    }
    if (type === Fragment) return flatten(children);
    const node = {id: nextId++, type};
    operation('create', node.id, type);
    const statics = {};
    for (const [key, value] of Object.entries(props)) {
      if (key === 'children' || key === 'ref') continue;
      if (/^on[A-Z]/.test(key)) {
        if (!handlers.has(node.id)) handlers.set(node.id, {});
        handlers.get(node.id)[key.slice(2).toLowerCase()] = value;
      } else if (typeof value === 'function') {
        effect(() => setProps(node, {[key]: value()}));
      } else {
        statics[key] = value;
      }
    }
    if (Object.keys(statics).length) setProps(node, statics);
    if (containers.has(type)) {
      appendChildren(node, children);
    } else if (children.length) {
      // The text of a text or a button.
      const content = flatten(children);
      if (content.some((c) => typeof c === 'function')) {
        effect(() => setProps(node, {text: content.map((c) => typeof c === 'function' ? c() : c).join('')}));
      } else {
        setProps(node, {text: content.join('')});
      }
    }
    onCleanup(() => handlers.delete(node.id));
    if (typeof props.ref === 'function') props.ref(node);
    return node;
  }

  // ---- Control flow ------------------------------------------------------

  const resolve = (value) => typeof value === 'function' ? value() : value;

  // The children while when() is true, else the fallback.
  function Show(props) {
    const when = typeof props.when === 'function' ? props.when : () => props.when;
    const shown = computed(() => !!when());
    return () => shown() ? untracked(() => resolve(props.children)) : untracked(() => resolve(props.fallback));
  }

  // children(item, index) for each item of each(); an item's nodes are made
  // once and moved when the list changes.
  function For(props) {
    const render = Array.isArray(props.children) ? props.children[0] : props.children;
    const cache = new Map();
    onCleanup(() => { for (const entry of cache.values()) entry.dispose(); });
    return () => {
      const items = resolve(props.each) || [];
      const seen = new Set();
      const out = items.map((item, index) => {
        seen.add(item);
        let entry = cache.get(item);
        if (!entry) {
          entry = root((dispose) => ({nodes: nodesOf(render(item, index)), dispose}));
          cache.set(item, entry);
        }
        return entry.nodes;
      });
      for (const [item, entry] of cache) {
        if (!seen.has(item)) {
          entry.dispose();
          cache.delete(item);
        }
      }
      return out;
    };
  }

  // ---- Windows -----------------------------------------------------------

  // A window with the content component(): zebkit.ui.window({title,
  // width, height, class}, App). Closed by the user or by close().
  function window(props, content) {
    const node = {id: nextId++, type: 'window'};
    let disposeRoot;
    operation('create', node.id, 'window');
    root((dispose) => {
      disposeRoot = dispose;
      const {onClose, ...rest} = props || {};
      handlers.set(node.id, {close: () => { disposeRoot(); if (onClose) onClose(); }});
      const statics = {};
      for (const [key, value] of Object.entries(rest)) {
        if (typeof value === 'function') effect(() => setProps(node, {[key]: value()}));
        else statics[key] = value;
      }
      setProps(node, statics);
      appendChildren(node, [untracked(() => resolve(content))]);
    });
    operation('show', node.id);
    commit();
    return {
      node,
      close() { operation('destroy', node.id); commit(); disposeRoot(); },
    };
  }

  // ---- Automation, for tests and agents ------------------------------------

  const act = (node, action, value) => {
    commit();
    native.uiAct(node.id, action, value ?? null);
    commit();
  };

  return Object.freeze({
    signal, effect, computed, batch, untracked, onCleanup, root,
    h, Fragment, Show, For, window,
    classes: classProps,
    flush: commit,
    dump: (node) => { commit(); return native.uiDump(node.id); },
    // The node as it is shown, as PNG bytes (a promise): a screenshot of
    // native interface.
    capture: (node) => { commit(); return native.uiCapture(node.id); },
    click: (node) => act(node, 'click'),
    type: (node, text) => act(node, 'type', String(text)),
    submit: (node) => act(node, 'submit'),
  });
}))js";

}  // namespace zebkit

#endif  // ZEBKIT_ENGINE_UI_SCRIPT_H_
