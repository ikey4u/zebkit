// Playwright's coverage script (injected/src/coverageScript.ts), bundled by scripts/playwright-sources.mjs
// from https://github.com/microsoft/playwright at cfe490d941f992655948eee8fd4fd94e1c4234bf.
// Copyright (c) Microsoft Corporation. Licensed under the Apache License,
// Version 2.0 (src/engine/playwright/LICENSE). Generated: do not edit.

var __commonJS = obj => {
  let required = false;
  let result;
  return function __require() {
    if (!required) {
      required = true;
      let fn;
      for (const name in obj) { fn = obj[name]; break; }
      const module = { exports: {} };
      fn(module.exports, module);
      result = module.exports;
    }
    return result;
  }
};
var __export = (target, all) => {for (var name in all) target[name] = all[name];};
var __toESM = mod => ({ ...mod, 'default': mod });
var __toCommonJS = mod => ({ ...mod, __esModule: true });


// packages/injected/src/coverageScript.ts
var coverageScript_exports = {};
__export(coverageScript_exports, {
  CoverageScript: () => CoverageScript,
  takeCoverageStashes: () => takeCoverageStashes
});
module.exports = __toCommonJS(coverageScript_exports);

// packages/isomorphic/istanbulCoverage.ts
var kCoverageStashPrefix = "__pwCoverage.";
var kCoverageStashError = "Failed to stash the coverage";
function takeCounters(counters) {
  let result;
  for (const key of Object.keys(counters)) {
    const count = counters[key];
    if (!count)
      continue;
    if (!result)
      result = {};
    result[key] = count;
    counters[key] = 0;
  }
  return result;
}
function takeBranchCounters(counters) {
  let result;
  for (const key of Object.keys(counters)) {
    const counts = counters[key];
    if (!counts.some(Boolean))
      continue;
    if (!result)
      result = {};
    result[key] = counts.slice();
    counts.fill(0);
  }
  return result;
}

// packages/injected/src/coverageScript.ts
var CoverageScript = class {
  constructor(global, takeName, sessionId) {
    this._reportedFiles = /* @__PURE__ */ new Set();
    this._onPageHide = () => this._stashCurrent();
    this._global = global;
    this._takeName = takeName;
    this._sessionId = sessionId;
    global[takeName] = Object.assign(() => this.take(), { dispose: () => this.dispose() });
    this._resetCounters();
    global.addEventListener("pagehide", this._onPageHide);
  }
  _resetCounters() {
    const coverage = this._global.__coverage__;
    for (const fileCoverage of Object.values(coverage || {})) {
      takeCounters(fileCoverage.s);
      takeCounters(fileCoverage.f);
      takeBranchCounters(fileCoverage.b);
    }
  }
  dispose() {
    this._global.removeEventListener("pagehide", this._onPageHide);
    delete this._global[this._takeName];
  }
  take() {
    const chunks = takeCoverageStashes(this._global, this._sessionId);
    const delta = this._takeCurrent();
    if (delta)
      chunks.push(JSON.stringify({ data: delta }));
    return chunks;
  }
  _takeCurrent() {
    const coverage = this._global.__coverage__;
    if (!coverage)
      return void 0;
    const delta = {};
    for (const [file, fileCoverage] of Object.entries(coverage)) {
      const s = takeCounters(fileCoverage.s);
      const f = takeCounters(fileCoverage.f);
      const b = takeBranchCounters(fileCoverage.b);
      const isFirstReport = !this._reportedFiles.has(file);
      if (!isFirstReport && !s && !f && !b)
        continue;
      const entry = { path: fileCoverage.path, s: s || {}, f: f || {}, b: b || {} };
      if (isFirstReport) {
        entry.statementMap = fileCoverage.statementMap;
        entry.fnMap = fileCoverage.fnMap;
        entry.branchMap = fileCoverage.branchMap;
        this._reportedFiles.add(file);
      }
      delta[file] = entry;
    }
    return Object.keys(delta).length ? delta : void 0;
  }
  _stashCurrent() {
    const delta = this._takeCurrent();
    if (!delta)
      return;
    const id = Math.random().toString(36).slice(2);
    try {
      this._global.localStorage.setItem(kCoverageStashPrefix + this._sessionId + "." + id, JSON.stringify({ id, data: delta }));
    } catch (error) {
      try {
        this._global.localStorage.setItem(stashErrorKey(this._sessionId), String(error));
      } catch {
      }
    }
  }
};
function stashErrorKey(sessionId) {
  return kCoverageStashPrefix + "error." + sessionId;
}
function takeCoverageStashes(global, sessionId) {
  const result = [];
  let storage;
  try {
    storage = global.localStorage;
  } catch {
    return result;
  }
  const error = storage.getItem(stashErrorKey(sessionId));
  if (error) {
    storage.removeItem(stashErrorKey(sessionId));
    throw new Error(kCoverageStashError + ": " + error);
  }
  const sessionPrefix = kCoverageStashPrefix + sessionId + ".";
  const keys = [];
  for (let i = 0; i < storage.length; i++) {
    const key = storage.key(i);
    if (key && key.startsWith(kCoverageStashPrefix))
      keys.push(key);
  }
  for (const key of keys) {
    const json = key.startsWith(sessionPrefix) ? storage.getItem(key) : void 0;
    storage.removeItem(key);
    if (json)
      result.push(json);
  }
  return result;
}
