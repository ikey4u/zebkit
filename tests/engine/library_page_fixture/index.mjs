// The fixture of tests/engine/library_page.py and the bindings' checks: a
// library for the backend "page" (docs/JS_LIBRARIES.md), which measures text,
// lays out and draws, and tries every way out of its page.
import {twice} from './lib/helper.mjs';

// ---- values ----
export const echo = (...args) => args;
export const double = (n) => twice(n);
export const nothing = () => undefined;
export const text = (s) => ({length: s.length, upper: s.toUpperCase(), same: s});
export const sizes = (...parts) => parts.map((p) => p instanceof Uint8Array ? [p.length, p[0] ?? -1, p[p.length - 1] ?? -1] : null);
export const roundTrip = (bytes) => bytes;
export const bytesOut = (n) => { const b = new Uint8Array(n); for (let i = 0; i < n; ++i) b[i] = i & 255; return {b, s: 'x'}; };
export function thrower() { throw new RangeError('too far'); }
export async function rejecter() { await null; throw new TypeError('rejected here'); }
export const badBigInt = () => 1n;
export const badNode = () => document.body;
export const later = (value, ms) => new Promise((resolve) => setTimeout(() => resolve(value), ms));
export const never = () => new Promise(() => {});
export const spin = () => { for (;;) {} };
export function grow() { const keep = []; for (;;) keep.push(new Array(1 << 16).fill(Math.random())); }
let counter = 0;
export const count = () => ++counter;

// ---- what a page has: canvas, fonts, layout, SVG ----
export function measure(text, font) {
  const context = document.createElement('canvas').getContext('2d');
  context.font = font;
  const m = context.measureText(text);
  return {width: m.width, ascent: m.actualBoundingBoxAscent, descent: m.actualBoundingBoxDescent};
}
export function fonts() {
  const context = new OffscreenCanvas(10, 10).getContext('2d');
  const width = (font) => { context.font = font; return context.measureText('iiiiWWWW').width; };
  return {serif: width('16px serif'), sans: width('16px sans-serif'), mono: width('16px monospace'),
          check: document.fonts.check('16px sans-serif')};
}
export function layout(width) {
  const div = document.createElement('div');
  div.style.cssText = `width: ${width}px; padding: 10px; font: 20px sans-serif; box-sizing: content-box`;
  div.textContent = 'Hello, layout';
  document.body.append(div);
  const rect = div.getBoundingClientRect();
  const style = getComputedStyle(div);
  const result = {width: rect.width, height: rect.height, padding: style.paddingLeft, font: style.fontSize};
  div.remove();
  return result;
}
export function svg(label) {
  const ns = 'http://www.w3.org/2000/svg';
  const root = document.createElementNS(ns, 'svg');
  const node = document.createElementNS(ns, 'text');
  node.setAttribute('x', '10');
  node.setAttribute('y', '30');
  node.setAttribute('font-size', '20');
  node.textContent = label;
  root.append(node);
  document.body.append(root);
  const box = node.getBBox();
  const rect = document.createElementNS(ns, 'rect');
  for (const [k, v] of Object.entries({x: box.x, y: box.y, width: box.width, height: box.height, fill: 'none'})) rect.setAttribute(k, v);
  root.prepend(rect);
  root.setAttribute('width', String(Math.ceil(box.x + box.width + 10)));
  root.setAttribute('height', '40');
  const markup = new XMLSerializer().serializeToString(root);
  root.remove();
  return {markup, width: box.width};
}
export async function png(size) {
  const canvas = new OffscreenCanvas(size, size);
  const context = canvas.getContext('2d');
  context.fillStyle = '#c00';
  context.fillRect(0, 0, size, size);
  const blob = await canvas.convertToBlob({type: 'image/png'});
  return new Uint8Array(await blob.arrayBuffer());
}

// ---- hidden, yet not throttled; no user activation ----
export function page() {
  return {visibility: document.visibilityState, hidden: document.hidden, width: innerWidth, height: innerHeight,
          origin: location.origin, protocol: location.protocol, zebkit: typeof zebkit, call: typeof __zebkit_call,
          scripts: document.scripts.length, focus: document.hasFocus()};
}
export async function timers(rounds) {
  const start = performance.now();
  for (let i = 0; i < rounds; ++i) await new Promise((resolve) => setTimeout(resolve, 0));
  const nested = performance.now() - start;
  let frames = 0;
  const until = performance.now() + 500;
  await new Promise((resolve) => {
    const frame = () => { ++frames; if (performance.now() < until) requestAnimationFrame(frame); else resolve(); };
    requestAnimationFrame(frame);
  });
  return {nested, frames};
}
export function activation() {
  let opened;
  try { opened = window.open('about:blank', '_blank'); } catch (e) { opened = 'threw ' + e.name; }
  let alerted = 'returned';
  try { alert('x'); } catch (e) { alerted = 'threw ' + e.name; }
  return {isActive: navigator.userActivation.isActive, hasBeenActive: navigator.userActivation.hasBeenActive,
          opened: opened === null ? null : typeof opened};
}

// ---- storage, its own ----
export function setItem(key, value) { localStorage.setItem(key, value); sessionStorage.setItem(key, value); return true; }
export function getItem(key) { return [localStorage.getItem(key), sessionStorage.getItem(key)]; }
export async function indexed(key, value) {
  const db = await new Promise((resolve, reject) => {
    const request = indexedDB.open('fixture', 1);
    request.onupgradeneeded = () => request.result.createObjectStore('s');
    request.onsuccess = () => resolve(request.result);
    request.onerror = () => reject(request.error);
  });
  const tx = db.transaction('s', 'readwrite');
  const store = tx.objectStore('s');
  const before = await new Promise((resolve) => { const r = store.get(key); r.onsuccess = () => resolve(r.result ?? null); });
  if (value !== undefined) store.put(value, key);
  await new Promise((resolve) => { tx.oncomplete = resolve; });
  db.close();
  return before;
}

// ---- the network: each way a page reaches out ----
const settled = (promise, ms = 3000) => Promise.race([promise, new Promise((resolve) => setTimeout(() => resolve('timeout'), ms))]);
export async function getText(url, init) {
  try {
    const r = await fetch(url, init);
    return {status: r.status, body: await r.text(), redirected: r.redirected};
  } catch (e) {
    return {error: e.name + ': ' + e.message};
  }
}
export function ownFile() {
  return getText('./data.json');
}
export async function ownImport() {
  const module = await import('./lib/helper.mjs');
  return module.twice(5);
}
export async function outsideImport(specifier) {
  try { await import(specifier); return 'imported'; } catch (e) { return 'refused'; }
}
export function image(url) {
  return settled(new Promise((resolve) => {
    const img = new Image();
    img.onload = () => resolve('loaded');
    img.onerror = () => resolve('error');
    img.src = url;
  }));
}
export function css(url) {
  return settled(new Promise((resolve) => {
    const link = document.createElement('link');
    link.rel = 'stylesheet';
    link.href = url;
    link.onload = () => resolve('loaded');
    link.onerror = () => resolve('error');
    document.head.append(link);
  }));
}
export function cssImport(url) {
  // A style the page writes itself, which pulls in another's.
  const style = document.createElement('style');
  style.textContent = `@import url("${url}"); body { background: url("${url}?bg") }`;
  document.head.append(style);
  return settled(new Promise((resolve) => setTimeout(() => resolve(getComputedStyle(document.body).backgroundImage ? 'styled' : 'none'), 300)));
}
export async function font(url) {
  try {
    const face = new FontFace('outside', `url(${url})`);
    await settled(face.load());
    return face.status;
  } catch (e) {
    return 'error';
  }
}
export function worker(url) {
  return settled(new Promise((resolve) => {
    try {
      const w = new Worker(url);
      w.onerror = () => resolve('error');
      w.onmessage = (e) => resolve('message ' + e.data);
    } catch (e) {
      resolve('threw ' + e.name);
    }
  }));
}
export function blobWorker(url) {
  const source = `fetch(${JSON.stringify(url)}).then(() => postMessage('fetched'), () => postMessage('refused'));`;
  return worker(URL.createObjectURL(new Blob([source], {type: 'text/javascript'})));
}
export function sharedWorker(url) {
  return settled(new Promise((resolve) => {
    try {
      const w = new SharedWorker(url);
      w.onerror = () => resolve('error');
      w.port.onmessage = (e) => resolve('message ' + e.data);
    } catch (e) {
      resolve('threw ' + e.name);
    }
  }));
}
export function webSocket(url) {
  return settled(new Promise((resolve) => {
    try {
      const socket = new WebSocket(url);
      socket.onopen = () => resolve('open');
      socket.onerror = () => resolve('error');
    } catch (e) {
      resolve('threw ' + e.name);
    }
  }));
}
export function frame(url) {
  return settled(new Promise((resolve) => {
    const iframe = document.createElement('iframe');
    iframe.onload = () => {
      let what = 'loaded';
      try { what = 'loaded ' + iframe.contentDocument?.body?.textContent; } catch (e) { what = 'loaded, other origin'; }
      iframe.remove();
      resolve(what);
    };
    iframe.src = url;
    document.body.append(iframe);
  }));
}
export function navigate(url) {
  location.href = url;
  return location.href;
}
export function beacon(url) {
  return navigator.sendBeacon(url, 'data');
}
export function hints(url) {
  for (const rel of ['preconnect', 'dns-prefetch', 'prefetch', 'preload']) {
    const link = document.createElement('link');
    link.rel = rel;
    link.href = url + '?' + rel;
    if (rel === 'preload') link.as = 'fetch';
    document.head.append(link);
  }
  return true;
}
export function download(url) {
  const a = document.createElement('a');
  a.href = url;
  a.download = 'x.txt';
  document.body.append(a);
  a.click();
  a.remove();
  return true;
}
export function form(url) {
  const f = document.createElement('form');
  f.action = url;
  f.method = 'post';
  document.body.append(f);
  try { f.submit(); } catch (e) { return 'threw'; }
  return 'submitted';
}
