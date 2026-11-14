// The library every binding's check loads (<zebkit/library.h>):
// tests/engine/library.py has its own, wider; this one is for
// python_parity.py, Rust's tests/engine.rs, Go's internal/check and
// script/library.mjs.
import {twice} from './lib/helper.mjs';

export const echo = (...args) => args;
export const double = (n) => twice(n);
export const text = (s) => ({same: s, length: s.length});
export const bytes = (n) => { const b = new Uint8Array(n); for (let i = 0; i < n; ++i) b[i] = i & 255; return {b}; };
export const sizes = (...parts) => parts.map((p) => p.length);
export function thrower() { throw new RangeError('too far'); }
export async function rejecter() { await null; throw new TypeError('rejected here'); }
export const badResult = () => new Map();
export const later = (value, ms) => new Promise((resolve) => setTimeout(() => resolve(value), ms));
export const spin = () => { for (;;) {} };
export function grow() { const keep = []; for (;;) keep.push(new Array(100000).fill(Math.random())); }
export async function get(url) { const r = await fetch(url); return {status: r.status, body: await r.text()}; }

// The application's functions (the load's "host"), each binding's own.
import host from 'zebkit:host';
export async function work(steps) {
  for (let i = 1; i <= steps; ++i) await host.call('progress', i, steps);
  return 'done';
}
export async function ask(question, bytes) {
  const answer = await host.call('lookup', question, bytes);
  return {text: answer.text, bytes: answer.data instanceof Uint8Array ? [...answer.data] : null};
}
export async function failing() {
  try { await host.call('fails', 'x'); return 'resolved'; } catch (error) { return error.message; }
}
export const threaded = (n) => host.call('threaded', n);
export async function unanswered() {
  try { await host.call('never'); return 'resolved'; } catch (error) { return error.name; }
}
// Busy for ms milliseconds, as a long computation is: instances run these in parallel.
export function busy(ms) { const end = Date.now() + ms; while (Date.now() < end) {} return ms; }
