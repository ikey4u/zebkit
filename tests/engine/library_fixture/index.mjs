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
