// What zebkit's automation takes from Playwright (docs/AUTOMATION.md),
// generated from a Playwright checkout into src/engine/playwright/:
//
// - injected_script.js: packages/injected/src/injectedScript.ts and what it
//   imports, bundled as Playwright's utils/generate_injected.js bundles it
//   (esbuild, CommonJS, ES2019, CSS inlined, Playwright's own module
//   prelude), so that `new (module.exports.InjectedScript())(globalThis,
//   options)` makes the same object Playwright's server makes. One bundle
//   exports with it what Playwright's evaluate() runs in the page: its
//   values serialized and parsed as utilityScriptSerializers.ts does.
// - clock_script.js: packages/injected/src/clock.ts, bundled the same way,
//   what page.clock installs in every document (server/clock.ts).
// - websocket_mock_script.js: packages/injected/src/webSocketMock.ts, what
//   routeWebSocket installs (webSocketRouteDispatcher.ts).
// - webmcp_script.js: packages/injected/src/webMCP.ts, what Frame.webmcpTools
//   and webmcpCallTool run in the frame (server/webmcp.ts).
// - coverage_script.js: packages/injected/src/coverageScript.ts, what
//   tracing's coverage installs in every document (coverageRecorder.ts).
// - webauthn_script.js: packages/injected/src/webAuthn.ts, the virtual
//   authenticator credentials.ts installs (server/credentials.ts).
// - us_keyboard_layout.h: the US layout of usKeyboardLayout.ts, with the
//   aliases and shifted keys input.ts's buildLayoutClosure() derives from
//   it, as a C++ table.
//
//   node scripts/playwright-sources.mjs <playwright checkout> <esbuild module dir> <commit>

import fs from 'node:fs';
import path from 'node:path';
import { createRequire } from 'node:module';

const [root, esbuildDir, commit] = process.argv.slice(2);
const require = createRequire(path.join(esbuildDir, 'index.js'));
const esbuild = require('esbuild');
const out = path.resolve(path.dirname(new URL(import.meta.url).pathname), '..', 'src', 'engine', 'playwright');

// ---- The injected script, as utils/generate_injected.js makes it --------

const modulePrefix = `
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
`;

const inlineCSSPlugin = {
  name: 'inlineCSSPlugin',
  setup(build) {
    build.onLoad({ filter: /\.css$/ }, async args => {
      const css = await esbuild.transform(await fs.promises.readFile(args.path), { loader: 'css', minify: true });
      return { loader: 'text', contents: css.code };
    });
  },
};

// Playwright's module prelude in place of esbuild's (generate_injected.js's
// replaceEsbuildHeader()).
function withPrelude(content) {
  let start = content.indexOf('__toCommonJS');
  if (start !== -1)
    start = content.indexOf('\n', start);
  if (start === -1)
    throw new Error('no start of the bundled code');
  return modulePrefix + content.substring(start);
}

// A script of packages/injected/src, bundled as generate_injected.js does.
async function bundleInjected(name) {
  const result = await esbuild.build({
    entryPoints: [path.join(root, 'packages', 'injected', 'src', name)],
    absWorkingDir: root,
    bundle: true,
    write: false,
    outdir: 'out',
    format: 'cjs',
    platform: 'browser',
    target: 'ES2019',
    logLevel: 'warning',
  });
  return withPrelude(result.outputFiles[0].text);
}
const clockSource = await bundleInjected('clock.ts');
// The trace's DOM snapshots: snapshotterInjected.ts's frameSnapshotStreamer(),
// a function the server runs in each frame (snapshotter.ts), plain JavaScript.
const snapshotterTs = fs.readFileSync(path.join(root, 'packages', 'playwright-core', 'src', 'server', 'trace',
    'recorder', 'snapshotterInjected.ts'), 'utf8');
const snapshotterSource = (await esbuild.transform(snapshotterTs, { loader: 'ts', target: 'ES2019', format: 'esm' }))
    .code.replace(/^export function frameSnapshotStreamer/m, 'function frameSnapshotStreamer')
    .replace(/^export \{[^}]*\};?$/mg, '')
    .replace(/^export .*$/mg, '');
const webSocketMockSource = await bundleInjected('webSocketMock.ts');
// storageState()'s IndexedDB and OPFS (browserContext.ts runs it in a page of
// each origin): module.exports.StorageScript().
const storageSource = await bundleInjected('storageScript.ts');
// Frame.webmcpTools and webmcpCallTool (server/webmcp.ts runs it in the
// frame's utility world): module.exports.WebMCPScript().
const webMCPSource = await bundleInjected('webMCP.ts');
// Tracing's istanbul coverage (server/coverageRecorder.ts installs it in every
// document): module.exports.CoverageScript(), takeCoverageStashes().
const coverageSource = await bundleInjected('coverageScript.ts');
// The context's virtual WebAuthn authenticator (server/credentials.ts installs it
// in every document): module.exports.inject().
const webAuthnSource = await bundleInjected('webAuthn.ts');

const built = await esbuild.build({
  stdin: {
    contents: `export { InjectedScript } from './packages/injected/src/injectedScript';
export { serializeAsCallArgument, parseEvaluationResultValue } from './packages/isomorphic/utilityScriptSerializers';
export { renderAriaSnapshotAsYaml } from './packages/isomorphic/ariaSnapshotRenderer';
export { parseAriaSnapshotUnsafe } from './packages/isomorphic/ariaSnapshot';
export * as yaml from 'yaml';
`,
    resolveDir: root,
    sourcefile: 'zebkit-injected.ts',
    loader: 'ts',
  },
  absWorkingDir: root,
  // The YAML library, from where scripts/playwright-sources.sh installed it.
  nodePaths: [path.dirname(esbuildDir)],
  bundle: true,
  write: false,
  outdir: 'out',
  format: 'cjs',
  platform: 'browser',
  target: 'ES2019',
  plugins: [inlineCSSPlugin],
  logLevel: 'warning',
});
let content = built.outputFiles[0].text;
let start = content.indexOf('__toCommonJS');
if (start !== -1)
  start = content.indexOf('\n', start);
if (start === -1)
  throw new Error('no start of the bundled code');
content = modulePrefix + content.substring(start);
const license = `// Playwright's injected script, bundled by scripts/playwright-sources.mjs
// from https://github.com/microsoft/playwright at ${commit}.
// Copyright (c) Microsoft Corporation. Licensed under the Apache License,
// Version 2.0 (src/engine/playwright/LICENSE). Generated: do not edit.
`;
fs.writeFileSync(path.join(out, 'injected_script.js'), license + content);
fs.writeFileSync(path.join(out, 'clock_script.js'),
    license.replace("injected script", "clock (injected/src/clock.ts)") + clockSource);
fs.writeFileSync(path.join(out, 'storage_script.js'),
    license.replace("injected script", "storage script (injected/src/storageScript.ts)") + storageSource);
fs.writeFileSync(path.join(out, 'snapshotter_script.js'),
    license.replace("injected script", "DOM snapshot streamer (server/trace/recorder/snapshotterInjected.ts)") + snapshotterSource);
fs.writeFileSync(path.join(out, 'webmcp_script.js'),
    license.replace("injected script", "WebMCP script (injected/src/webMCP.ts)") + webMCPSource);
fs.writeFileSync(path.join(out, 'webauthn_script.js'),
    license.replace("injected script", "WebAuthn script (injected/src/webAuthn.ts)") + webAuthnSource);
fs.writeFileSync(path.join(out, 'coverage_script.js'),
    license.replace("injected script", "coverage script (injected/src/coverageScript.ts)") + coverageSource);
fs.writeFileSync(path.join(out, 'websocket_mock_script.js'),
    license.replace("injected script", "WebSocket mock (injected/src/webSocketMock.ts)") + webSocketMockSource);

// ---- What the protocol says of each method (protocolMetainfo.ts) --------
// For a trace: which calls are internal, which take DOM snapshots, which are
// input actions and which wait (tracing.ts's _shouldCaptureAtPhase()).
{
  const metainfo = fs.readFileSync(path.join(root, 'packages', 'isomorphic', 'protocolMetainfo.ts'), 'utf8');
  const rows = [];
  for (const [, method, body] of metainfo.matchAll(/^\s*\['([A-Za-z]+\.[A-Za-z]+)', \{(.*)\}\],?$/mg)) {
    const flag = name => new RegExp(`\\b${name}: true`).test(body) ? 'true' : 'false';
    rows.push(`    {"${method}", ${flag('internal')}, ${flag('snapshot')}, ${flag('input')}, ${flag('isAutoWaiting')}},`);
  }
  fs.writeFileSync(path.join(out, 'protocol_metainfo.h'), `// What Playwright's protocol says of its methods (packages/isomorphic/protocolMetainfo.ts),
// generated by scripts/playwright-sources.mjs from https://github.com/microsoft/playwright
// at ${commit}. Copyright (c) Microsoft Corporation. Licensed under the Apache License,
// Version 2.0 (src/engine/playwright/LICENSE). Generated: do not edit.

#ifndef ZEBKIT_ENGINE_PLAYWRIGHT_PROTOCOL_METAINFO_H_
#define ZEBKIT_ENGINE_PLAYWRIGHT_PROTOCOL_METAINFO_H_

namespace zebkit {

struct ProtocolMethodInfo {
  const char* method;
  bool internal;
  bool snapshot;
  bool input;
  bool auto_waiting;
};

inline constexpr ProtocolMethodInfo kProtocolMethods[] = {
${rows.join('\n')}
};

}  // namespace zebkit

#endif  // ZEBKIT_ENGINE_PLAYWRIGHT_PROTOCOL_METAINFO_H_
`);
}

// ---- The US keyboard layout, as input.ts's buildLayoutClosure() ---------

const layoutTs = fs.readFileSync(path.join(root, 'packages', 'playwright-core', 'src', 'server', 'usKeyboardLayout.ts'), 'utf8');
const layoutJs = (await esbuild.transform(layoutTs, { loader: 'ts', format: 'cjs' })).code;
const layoutModule = { exports: {} };
new Function('module', 'exports', layoutJs)(layoutModule, layoutModule.exports);
const layout = layoutModule.exports.USKeyboardLayout;

const aliases = new Map([
  ['ShiftLeft', ['Shift']],
  ['ControlLeft', ['Control']],
  ['AltLeft', ['Alt']],
  ['MetaLeft', ['Meta']],
  ['Enter', ['\n', '\r']],
]);
const result = new Map();
for (const code in layout) {
  const definition = layout[code];
  const description = {
    key: definition.key || '',
    keyCode: definition.keyCode || 0,
    keyCodeWithoutLocation: definition.keyCodeWithoutLocation || definition.keyCode || 0,
    code,
    text: definition.text || '',
    location: definition.location || 0,
  };
  if (definition.key.length === 1)
    description.text = description.key;
  let shifted;
  if (definition.shiftKey) {
    shifted = { ...description, key: definition.shiftKey, text: definition.shiftKey };
    if (definition.shiftKeyCode) {
      shifted.keyCode = definition.shiftKeyCode;
      shifted.keyCodeWithoutLocation = definition.shiftKeyCode;
    }
  }
  result.set(code, { ...description, shifted });
  for (const alias of aliases.get(code) || [])
    result.set(alias, description);
  if (definition.location)
    continue;
  if (description.key.length === 1)
    result.set(description.key, description);
  if (shifted)
    result.set(shifted.key, { ...shifted, shifted: undefined });
}

const c = s => JSON.stringify(s);
const entry = d => `{${c(d.key)}, ${c(d.code)}, ${c(d.text)}, ${d.keyCode}, ${d.keyCodeWithoutLocation}, ${d.location}}`;
const rows = [...result].map(([name, d]) =>
  `    {${c(name)}, ${entry(d)}, ${d.shifted ? 'true' : 'false'}, ${d.shifted ? entry(d.shifted) : '{}'}},`);
fs.writeFileSync(path.join(out, 'us_keyboard_layout.h'), `// Playwright's US keyboard layout (usKeyboardLayout.ts) with the names
// input.ts's buildLayoutClosure() gives its keys, generated by
// scripts/playwright-sources.mjs from https://github.com/microsoft/playwright
// at ${commit}. Copyright (c) Microsoft Corporation, Apache License 2.0
// (src/engine/playwright/LICENSE). Generated: do not edit.

#ifndef ZEBKIT_ENGINE_PLAYWRIGHT_US_KEYBOARD_LAYOUT_H_
#define ZEBKIT_ENGINE_PLAYWRIGHT_US_KEYBOARD_LAYOUT_H_

namespace zebkit::playwright {

struct KeyDescription {
  const char* key = "";
  const char* code = "";
  const char* text = "";
  int key_code = 0;
  int key_code_without_location = 0;
  int location = 0;
};

struct KeyEntry {
  // What Keyboard.press() and the others are given: "KeyA", "a", "A",
  // "Shift", "Enter", "\\n".
  const char* name;
  KeyDescription description;
  bool has_shifted;
  // What the key is with Shift held.
  KeyDescription shifted;
};

inline constexpr int kKeypadLocation = 3;

inline constexpr KeyEntry kUsKeyboardLayout[] = {
${rows.join('\n')}
};

}  // namespace zebkit::playwright

#endif  // ZEBKIT_ENGINE_PLAYWRIGHT_US_KEYBOARD_LAYOUT_H_
`);
console.log(`injected_script.js ${content.length} bytes, ${result.size} keys`);
