#!/usr/bin/env node
// js-bundle.mjs: an npm package, or an entry module that imports packages,
// made into a library zebkit loads (<zebkit/library.h>, docs/JS_LIBRARIES.md):
// one ES module, <folder>/index.mjs, with nothing to import, the packages'
// licences beside it in <folder>/LICENSES.txt, and its exported functions
// (names, doc comments, parameter and return types) in <folder>/exports.json.
//
//   node js-bundle.mjs <entry.mjs | package[@version] | package folder> <folder> [options]
//   node js-bundle.mjs --list <folder>
//   node js-bundle.mjs --wrapper <rust|python|go|ts> <folder> [--name Name] [--package name] [--out file]
//
//   --install <package@version>  a package the entry imports (repeatable);
//                                a package given as the first argument is one
//   --empty <module>             an import of a Node.js built-in (or of any
//                                module) taken as an empty module: for one the
//                                code only uses where it is there (repeatable)
//   --work <dir>                 where packages are installed (kept between
//                                runs); a temporary directory by default
//   --esbuild <dir>              a node_modules with esbuild (and
//                                cjs-module-lexer) installed, in place of
//                                installing them into the work dir
//   --minify                     the bundle minified
//   --offline                    npm's cache only: nothing downloaded
//
// A package as the first argument is bundled whole: an ES module package as
// `export * from '<package>'`; a CommonJS one (found as esbuild resolves it)
// with an entry made for it, each name it exports (read by cjs-module-lexer,
// as Node.js reads them, without running it) a function of the library, and
// what module.exports is as "default". To choose what the library exports,
// write a small entry that imports packages and exports functions. Build time
// only: zebkit never downloads code, and this does not run where the
// application runs.
//
// --list prints a bundle's functions, with their types where they are known.
// --wrapper prints a typed facade of a bundle for the application: a type
// with one method per function, its doc comment and its parameter and return
// types, over zebkit::js::Library (Rust), zebkit.library (Python),
// zebkit.LoadLibrary (Go), or TypeScript declarations of zebkit.Library's
// calls (host JavaScript). Types come from the entry's JSDoc (@param {type},
// @returns {type}) or the package's TypeScript declarations; string, number,
// boolean, Uint8Array (a part), void and arrays of them are typed, anything
// else is a JSON value.
//
// Packages are installed with npm --ignore-scripts (their install scripts do
// not run) and bundled with esbuild (upstream.lock's PLAYWRIGHT_ESBUILD_VERSION,
// 0.28.1): neutral platform, browser fields first, NODE_ENV=production, JSON
// and WebAssembly inlined (a .wasm import is its bytes, a Uint8Array);
// `zebkit:host` (the application's functions) stays an import. It cannot
// bundle native addons (.node), Node.js built-ins (each import is named;
// --empty takes one as empty) or imports computed at run time. Needs Node.js 18
// or later and npm. The bundle's functions are listed by importing it in a
// Node.js process of its own.

import {spawnSync} from 'node:child_process';
import fs from 'node:fs';
import {builtinModules, createRequire} from 'node:module';
import os from 'node:os';
import path from 'node:path';
import {pathToFileURL} from 'node:url';

const ESBUILD_VERSION = '0.28.1';
const LEXER_VERSION = '2.2.1';

function usage(message) {
  if (message) console.error(`js-bundle: ${message}`);
  console.error('usage: node js-bundle.mjs <entry.mjs | package[@version] | package folder> <folder> [--install pkg@ver]... ' +
                '[--empty module]... [--work dir] [--esbuild dir] [--minify] [--offline]\n' +
                '       node js-bundle.mjs --list <folder>\n' +
                '       node js-bundle.mjs --wrapper <rust|python|go|ts> <folder> [--name Name] [--package name] ' +
                '[--out file]');
  process.exit(2);
}

const positional = [], install = [], empty = new Set();
let work = null, esbuildDir = null, minify = false, offline = false, list = false, wrapper = null;
let wrapperName = null, goPackage = null, outFile = null;
const argv = process.argv.slice(2);
for (let i = 0; i < argv.length; ++i) {
  const arg = argv[i];
  const value = () => argv[++i] ?? usage(`${arg} needs a value`);
  if (arg === '--install') install.push(value());
  else if (arg === '--empty') empty.add(value());
  else if (arg === '--work') work = path.resolve(value());
  else if (arg === '--esbuild') esbuildDir = path.resolve(value());
  else if (arg === '--minify') minify = true;
  else if (arg === '--offline') offline = true;
  else if (arg === '--list') list = true;
  else if (arg === '--wrapper') wrapper = value();
  else if (arg === '--name') wrapperName = value();
  else if (arg === '--package') goPackage = value();
  else if (arg === '--out') outFile = path.resolve(value());
  else if (arg === '--help' || arg === '-h') usage();
  else if (arg.startsWith('--')) usage(`unknown option ${arg}`);
  else positional.push(arg);
}

// ---- What a bundle exports: exports.json --------------------------------------

// A JSDoc comment's text and tags: {doc, params: {name: {type, optional}}, returns}.
function readJsDoc(comment) {
  const lines = comment.replace(/^\/\*\*?|\*\/$/g, '').split('\n').map((line) => line.replace(/^\s*\*? ?/, ''));
  const doc = [], params = {};
  let returns = null;
  for (const line of lines) {
    const param = /^@param\s+(?:\{(.+?)\}\s+)?(\[)?([\w$.]+)/.exec(line);
    const ret = /^@returns?\s+\{(.+?)\}/.exec(line);
    if (param) params[param[3]] = {type: param[1] ?? null, optional: !!param[2]};
    else if (ret) returns = ret[1];
    else if (!line.startsWith('@')) doc.push(line);
  }
  return {doc: doc.join('\n').trim(), params, returns};
}

// The comment just above @at in @text: a /** */ block or // lines.
function commentBefore(text, at) {
  const before = text.slice(0, at).replace(/\s+$/, '');
  if (before.endsWith('*/')) {
    const start = before.lastIndexOf('/*');
    return start >= 0 ? before.slice(start) : '';
  }
  const lines = before.split('\n');
  const comment = [];
  while (lines.length && /^\s*\/\//.test(lines[lines.length - 1])) comment.unshift(lines.pop().replace(/^\s*\/\/ ?/, ''));
  return comment.length ? '/**\n' + comment.join('\n') + '\n*/' : '';
}

// Top-level pieces of a parameter list.
function splitParams(text) {
  const pieces = [];
  let depth = 0, piece = '';
  for (const c of text) {
    if ('([{<'.includes(c)) ++depth;
    if (')]}>'.includes(c)) --depth;
    if (c === ',' && depth === 0) { pieces.push(piece); piece = ''; } else piece += c;
  }
  if (piece.trim()) pieces.push(piece);
  return pieces.map((p) => p.trim()).filter(Boolean);
}

// Whether a parameter has a default: an "=" outside its brackets.
function hasDefault(piece) {
  let depth = 0;
  for (const c of piece) {
    if ('([{'.includes(c)) ++depth;
    else if (')]}'.includes(c)) --depth;
    else if (c === '=' && depth === 0) return true;
  }
  return false;
}

// The functions an entry module declares as `export function f(...)` or
// `export const f = (...) =>`, with their JSDoc.
function fromSource(text) {
  const found = {};
  const re = /export\s+(?:(async)\s+)?function\s*\*?\s*([\w$]+)\s*\(([^)]*)\)|export\s+const\s+([\w$]+)\s*=\s*(async\s+)?(?:function\s*[\w$]*\s*)?\(([^)]*)\)/g;
  for (let m; (m = re.exec(text));) {
    const name = m[2] ?? m[4], params = m[3] ?? m[6], async = !!(m[1] ?? m[5]);
    const jsdoc = readJsDoc(commentBefore(text, m.index));
    const tagged = Object.entries(jsdoc.params).filter(([key]) => !key.includes('.'));
    found[name] = {
      doc: jsdoc.doc,
      params: splitParams(params).map((piece, i) => {
        const rest = piece.startsWith('...');
        const bare = piece.replace(/^\.\.\./, '');
        const plain = /^[\w$]+/.exec(bare)?.[0];
        const tag = (plain && jsdoc.params[plain]) || (tagged[i] ? tagged[i][1] : null);
        const name = plain && !/^[{[]/.test(bare) ? plain : tagged[i]?.[0] ?? `arg${i}`;
        return {name, type: rest ? `${tag?.type ?? 'unknown'}[]` : tag?.type ?? null,
                optional: hasDefault(bare) || !!tag?.optional, rest};
      }),
      returns: jsdoc.returns ? (async ? `Promise<${jsdoc.returns}>` : jsdoc.returns) : null,
    };
  }
  return found;
}

// The functions TypeScript declarations declare: `export declare function
// f(a: T): R;` and the like.
function fromDeclarations(text) {
  const found = {};
  const re = /(?:export\s+)?(?:declare\s+)?function\s+([\w$]+)\s*(?:<[^(]*>)?\s*\(([^)]*)\)\s*:\s*([^;{]+)/g;
  for (let m; (m = re.exec(text));) {
    if (found[m[1]]) continue;  // the first of overloads
    const jsdoc = readJsDoc(commentBefore(text, m.index));
    found[m[1]] = {
      doc: jsdoc.doc,
      params: splitParams(m[2]).map((piece, i) => {
        const p = /^(\.\.\.)?([\w$]+)(\?)?\s*(?::\s*([\s\S]+))?$/.exec(piece);
        return p ? {name: p[2], type: p[4]?.trim() ?? null, optional: !!p[3], rest: !!p[1]}
                 : {name: `arg${i}`, type: null, optional: false, rest: false};
      }),
      returns: m[3].trim(),
    };
  }
  return found;
}

// The declarations a package ships, or @types' for it.
function declarationsOf(packageDir, name, nodeModules) {
  const candidates = [];
  try {
    const manifest = JSON.parse(fs.readFileSync(path.join(packageDir, 'package.json'), 'utf8'));
    const types = manifest.types ?? manifest.typings ?? manifest.exports?.['.']?.types ??
                  manifest.exports?.['.']?.import?.types;
    if (typeof types === 'string') candidates.push(path.join(packageDir, types));
    if (manifest.main) candidates.push(path.join(packageDir, manifest.main.replace(/\.[cm]?js$/, '') + '.d.ts'));
  } catch (e) {}
  candidates.push(path.join(packageDir, 'index.d.ts'));
  const scoped = name.startsWith('@') ? name.slice(1).replace('/', '__') : name;
  candidates.push(path.join(nodeModules, '@types', scoped, 'index.d.ts'));
  const file = candidates.find((candidate) => fs.existsSync(candidate));
  return file ? fs.readFileSync(file, 'utf8') : '';
}

// The functions of the bundle at @file, by importing it in a Node.js process
// of its own: {name: true} for each exported function; null if it fails.
function functionsOf(file) {
  const script = `const m = await import(${JSON.stringify(pathToFileURL(file).href)});
    console.log(JSON.stringify(Object.keys(m).filter((k) => typeof m[k] === 'function')));`;
  const run = spawnSync(process.execPath, ['--input-type=module', '-e', script], {encoding: 'utf8', timeout: 60000});
  if (run.status !== 0) return null;
  try { return JSON.parse(run.stdout.trim().split('\n').pop()); } catch (e) { return null; }
}

function readExports(folder) {
  const file = path.join(folder, 'exports.json');
  if (fs.existsSync(file)) return JSON.parse(fs.readFileSync(file, 'utf8'));
  const names = functionsOf(path.join(folder, 'index.mjs'));
  if (!names) {
    console.error(`js-bundle: ${folder} has no exports.json, and its index.mjs did not import in Node.js`);
    process.exit(1);
  }
  return {functions: names.map((name) => ({name, doc: '', params: null, returns: null}))};
}

// ---- --list ----------------------------------------------------------------

const signature = (f) => `${f.name}(${f.params ? f.params.map((p) =>
  `${p.rest ? '...' : ''}${p.name}${p.optional ? '?' : ''}${p.type ? ': ' + p.type : ''}`).join(', ') : '...args'})` +
  (f.returns ? `: ${f.returns}` : '');

if (list) {
  if (positional.length !== 1) usage('--list takes a folder');
  for (const f of readExports(path.resolve(positional[0])).functions) {
    console.log(signature(f) + (f.doc ? `  // ${f.doc.split('\n')[0]}` : ''));
  }
  process.exit(0);
}

// ---- --wrapper ---------------------------------------------------------------

// A TypeScript type as a kind the generators know: string, number, boolean,
// bytes, void, an array of one ({array: kind}), or json (anything else).
function kindOf(type) {
  if (!type) return 'json';
  let t = type.trim().replace(/^Promise<([\s\S]*)>$/, '$1').trim();
  if (/^\(.*\)$/.test(t)) t = t.slice(1, -1).trim();
  const array = /^(?:Array<([\s\S]+)>|([\w$]+)\[\])$/.exec(t);
  if (array) {
    const item = kindOf(array[1] ?? array[2]);
    return typeof item === 'string' && ['string', 'number', 'boolean'].includes(item) ? {array: item} : 'json';
  }
  return {string: 'string', number: 'number', boolean: 'boolean', Uint8Array: 'bytes', ArrayBuffer: 'bytes',
          void: 'void', undefined: 'void'}[t] ?? 'json';
}

const words = (name) => name.replace(/([a-z0-9])([A-Z])/g, '$1 $2').split(/[^A-Za-z0-9]+/).filter(Boolean);
const snake = (name) => words(name).map((w) => w.toLowerCase()).join('_');
const camel = (name) => words(name).map((w) => w[0].toUpperCase() + w.slice(1)).join('');
const comment = (doc, prefix) => doc ? doc.split('\n').map((line) => (prefix + ' ' + line).trimEnd()).join('\n') + '\n' : '';
const RUST_KEYWORDS = new Set(['as', 'async', 'await', 'box', 'break', 'const', 'continue', 'crate', 'dyn', 'else', 'enum',
  'extern', 'false', 'fn', 'for', 'if', 'impl', 'in', 'let', 'loop', 'match', 'mod', 'move', 'mut', 'pub', 'ref',
  'return', 'self', 'static', 'struct', 'super', 'trait', 'true', 'type', 'unsafe', 'use', 'where', 'while', 'yield']);

function rust(api, name) {
  const T = {string: '&str', number: 'f64', boolean: 'bool', bytes: '&[u8]', json: 'Value'};
  const R = {string: 'String', number: 'f64', boolean: 'bool', bytes: 'Vec<u8>', json: 'Value', void: '()'};
  const argType = (k) => typeof k === 'object' ? `&[${R[k.array]}]` : T[k] ?? 'Value';
  const retType = (k) => typeof k === 'object' ? `Vec<${R[k.array]}>` : R[k];
  const toValue = (k, v) => typeof k === 'object'
    ? `Value::Array(${v}.iter().map(|item| Value::from(${k.array === 'string' ? 'item.as_str()' : '*item'})).collect())`
    : k === 'bytes' ? `part(&mut parts, ${v})` : k === 'json' ? v : `Value::from(${v})`;
  const scalar = (k, v) => ({string: `${v}.as_str().map(str::to_owned)`, number: `${v}.as_f64()`,
                             boolean: `${v}.as_bool()`})[k];
  const fromReply = (k, f) => {
    if (k === 'void') return 'Ok(())';
    if (k === 'json') return 'Ok(reply.value)';
    if (k === 'bytes') return `reply.parts.into_iter().next().ok_or_else(|| unexpected("${f}", "bytes"))`;
    if (typeof k === 'object') {
      return `reply.value.as_array().and_then(|items| items.iter().map(|item| ${scalar(k.array, 'item')}).collect::<Option<Vec<_>>>())\n` +
             `            .ok_or_else(|| unexpected("${f}", "a list of ${k.array}s"))`;
    }
    return `${scalar(k, 'reply.value')}.ok_or_else(|| unexpected("${f}", "a ${k}"))`;
  };
  const id = (n) => RUST_KEYWORDS.has(snake(n)) ? `r#${snake(n)}` : snake(n);
  let out = `//! ${name}: a typed facade of a JavaScript library (\`zebkit::js::Library\`).\n` +
    `//! Generated by tools/js-bundle.mjs --wrapper rust from its exports.json; do not edit.\n\n` +
    `#![allow(dead_code)]\n\n` +
    `use zebkit::js::{Code, Library, LibraryError};\nuse zebkit::json::Value;\n\n` +
    `pub struct ${name} {\n    pub library: Library,\n}\n\n` +
    `fn unexpected(function: &str, what: &str) -> LibraryError {\n` +
    `    LibraryError { code: Code::BadResult, message: format!("{function}() did not return {what}"), name: String::new(), stack: String::new() }\n}\n\n` +
    `/// Bytes as a part: \`{"$part": n}\` in the arguments.\nfn part(parts: &mut Vec<Vec<u8>>, bytes: &[u8]) -> Value {\n` +
    `    parts.push(bytes.to_vec());\n    Value::object([("$part", Value::from(parts.len() - 1))])\n}\n\n` +
    `impl ${name} {\n    /// Loads the library at \`path\` (\`Library::load\`): the engine's thread.\n` +
    `    pub fn load(path: impl AsRef<std::path::Path>, options: &Value) -> Result<${name}, zebkit::Error> {\n` +
    `        Ok(${name} { library: Library::load(path, options)? })\n    }\n`;
  for (const f of api.functions) {
    const params = (f.params ?? [{name: 'args', type: null, rest: true}]).map((p) => ({...p, id: id(p.name), kind: kindOf(p.type)}));
    const sig = params.map((p) => p.rest ? `${p.id}: Vec<Value>` : p.optional ? `${p.id}: Option<${argType(p.kind)}>` : `${p.id}: ${argType(p.kind)}`);
    const ret = kindOf(f.returns);
    const bytes = params.some((p) => p.kind === 'bytes');
    const required = params.filter((p) => !p.rest && !p.optional);
    const later = params.length > required.length;
    out += `\n${comment(f.doc, '    ///')}    pub async fn ${id(f.name)}(&self${sig.map((s) => ', ' + s).join('')}) -> Result<${retType(ret)}, LibraryError> {\n`;
    if (bytes) out += `        let mut parts: Vec<Vec<u8>> = Vec::new();\n`;
    out += `        let ${later ? 'mut ' : ''}args = vec![${required.map((p) => toValue(p.kind, p.id)).join(', ')}];\n`;
    for (const p of params.slice(required.length)) {
      if (p.rest) out += `        args.extend(${p.id});\n`;
      else out += `        if let Some(${p.id}) = ${p.id} {\n            args.push(${toValue(p.kind, p.id)});\n        }\n`;
    }
    const partsArg = bytes ? '&parts.iter().map(Vec::as_slice).collect::<Vec<_>>()' : '&[]';
    out += `        let reply = self.library.call_with("${f.name}", &Value::Array(args), ${partsArg}, None).await?;\n` +
           `        ${fromReply(ret, f.name)}\n    }\n`;
  }
  return out + '}\n';
}

function python(api, name) {
  const T = {string: 'str', number: 'float', boolean: 'bool', bytes: 'bytes', json: 'Any', void: 'None'};
  const typ = (k) => typeof k === 'object' ? `list[${T[k.array]}]` : T[k];
  let out = `"""${name}: a typed facade of a JavaScript library (zebkit.library).\n\n` +
    `Generated by tools/js-bundle.mjs --wrapper python from its exports.json; do not edit.\n` +
    `Each method returns a zebkit.automation.Future of the function's value.\n"""\n\n` +
    `from typing import Any\n\nimport zebkit.library\n\n_OMITTED = object()\n\n\n` +
    `def _given(*args):\n    """The arguments up to the first left out."""\n    given = list(args)\n` +
    `    while given and given[-1] is _OMITTED:\n        given.pop()\n    return [arg for arg in given if arg is not _OMITTED]\n\n\n` +
    `class ${name}:\n    def __init__(self, path, options=None, ready=None, host=None):\n` +
    `        """Loads the library at path (zebkit.library.load()): the engine's thread."""\n` +
    `        self.library = zebkit.library.load(path, options, ready, host)\n\n    def close(self):\n        self.library.close()\n`;
  for (const f of api.functions) {
    const params = f.params ?? [{name: 'args', type: null, rest: true}];
    const sig = params.map((p) => p.rest ? `*${snake(p.name)}: Any` : `${snake(p.name)}: ${typ(kindOf(p.type))}${p.optional ? ' = _OMITTED' : ''}`);
    const call = params.map((p) => (p.rest ? '*' : '') + snake(p.name)).join(', ');
    const doc = (f.doc || f.name) + (f.returns ? `\n\nA Future of ${typ(kindOf(f.returns))}.` : '');
    out += `\n    def ${snake(f.name)}(self${sig.map((s) => ', ' + s).join('')}) -> "zebkit.automation.Future":\n` +
           `        """${doc.replace(/\n/g, '\n        ').replace(/\n        \n/g, '\n\n')}"""\n` +
           `        return self.library.call("${f.name}", *_given(${call}))\n`;
  }
  return out;
}

function go(api, name, pkg) {
  const T = {string: 'string', number: 'float64', boolean: 'bool', bytes: '[]byte', json: 'json.RawMessage', void: 'struct{}'};
  const typ = (k) => typeof k === 'object' ? `[]${T[k.array]}` : T[k];
  const argTyp = (k) => k === 'json' ? 'any' : typ(k);
  let out = `// Package ${pkg}: ${name}, a typed facade of a JavaScript library (zebkit.LoadLibrary).\n` +
    `// Generated by tools/js-bundle.mjs --wrapper go from its exports.json; do not edit.\n` +
    `package ${pkg}\n\nimport (\n\t"encoding/json"\n\n\tzebkit "github.com/ikey4u/zebkit/bindings/golang"\n)\n\n` +
    `// ${name} is the library; each method calls one of its functions, and done\n// is told on the engine's thread.\n` +
    `type ${name} struct {\n\tLibrary *zebkit.Library\n}\n\n` +
    `// Load loads the library at path (zebkit.LoadLibrary): the engine's thread.\n` +
    `func Load(path string, options any) (*${name}, error) {\n\tlibrary, err := zebkit.LoadLibrary(path, options, nil)\n` +
    `\tif err != nil {\n\t\treturn nil, err\n\t}\n\treturn &${name}{Library: library}, nil\n}\n\n` +
    `// part is a {"$part": n} for bytes, which go as parts.\nfunc part(parts *[][]byte, bytes []byte) any {\n` +
    `\t*parts = append(*parts, bytes)\n\treturn map[string]int{"$part": len(*parts) - 1}\n}\n`;
  for (const f of api.functions) {
    const params = (f.params ?? [{name: 'args', type: null, rest: true}]).map((p) => ({...p, id: words(p.name).map((w, i) => i ? w[0].toUpperCase() + w.slice(1) : w.toLowerCase()).join('') || 'arg', kind: kindOf(p.type)}));
    const ret = kindOf(f.returns);
    const sig = params.map((p) => p.rest ? `${p.id} ...any` : `${p.id} ${p.optional ? 'any' : argTyp(p.kind)}`);
    out += `\n${comment(f.doc || f.name, '//')}` +
      `func (m *${name}) ${camel(f.name)}(${[...sig, `done func(${typ(ret)}, error)`].join(', ')}) error {\n` +
      `\tvar callParts [][]byte\n\tcallArgs := []any{}\n`;
    for (const p of params) {
      if (p.rest) out += `\tcallArgs = append(callArgs, ${p.id}...)\n`;
      else if (p.optional) out += `\tif ${p.id} != nil {\n\t\tcallArgs = append(callArgs, ${p.id})\n\t}\n`;
      else out += `\tcallArgs = append(callArgs, ${p.kind === 'bytes' ? `part(&callParts, ${p.id})` : p.id})\n`;
    }
    out += `\tcallJSON, err := json.Marshal(callArgs)\n\tif err != nil {\n\t\treturn err\n\t}\n` +
      `\t_, err = m.Library.Call("${f.name}", callJSON, callParts, 0, func(r zebkit.LibraryResult) {\n\t\tvar value ${typ(ret)}\n` +
      `\t\tif r.Err != nil {\n\t\t\tdone(value, r.Err)\n\t\t\treturn\n\t\t}\n`;
    if (ret === 'bytes') out += `\t\tif len(r.Parts) > 0 {\n\t\t\tvalue = r.Parts[0]\n\t\t}\n\t\tdone(value, nil)\n`;
    else if (ret === 'json') out += `\t\tdone(r.Value, nil)\n`;
    else if (ret === 'void') out += `\t\tdone(value, nil)\n`;
    else out += `\t\tdone(value, json.Unmarshal(r.Value, &value))\n`;
    out += `\t})\n\treturn err\n}\n`;
  }
  return out;
}

function typescript(api, name) {
  const T = {string: 'string', number: 'number', boolean: 'boolean', bytes: 'Uint8Array', void: 'null'};
  // A type as it is when it is one the library's values carry, else unknown.
  const typ = (type) => {
    const k = kindOf(type);
    if (typeof k === 'object') return `${T[k.array]}[]`;
    return T[k] ?? (type ? type.replace(/^Promise<([\s\S]*)>$/, '$1') : 'unknown');
  };
  let out = `// ${name}: TypeScript declarations of a JavaScript library's calls, for host\n` +
    `// JavaScript (zebkit.Library): \`const lib = zebkit.Library.load(path) as ${name};\`.\n` +
    `// Generated by tools/js-bundle.mjs --wrapper ts from its exports.json; do not edit.\n\n` +
    `export interface ${name} {\n  /** The library's exported functions, once it has loaded. */\n` +
    `  readonly ready: Promise<{functions: string[]}>;\n`;
  for (const f of api.functions) {
    const params = (f.params ?? [{name: 'args', type: null, rest: true}]).map((p) =>
      p.rest ? `...${p.name}: ${p.type ? typ(p.type) : 'unknown[]'}` : `${p.name}${p.optional ? '?' : ''}: ${typ(p.type)}`);
    out += `${f.doc ? '  /** ' + f.doc.split('\n').join('\n   * ') + ' */\n' : ''}` +
           `  call(name: '${f.name}'${params.map((p) => ', ' + p).join('')}): Promise<${typ(f.returns)}>;\n`;
  }
  return out + `  callWith(name: string, args?: unknown[], options?: {timeoutMs?: number}): Promise<unknown>;\n` +
    `  cancel(call: Promise<unknown>): void;\n  close(): void;\n}\n`;
}

if (wrapper) {
  if (positional.length !== 1) usage('--wrapper takes a folder');
  const folder = path.resolve(positional[0]);
  const api = readExports(folder);
  const name = wrapperName ?? camel(path.basename(folder)) ?? 'Library';
  const generators = {rust: () => rust(api, name), python: () => python(api, name),
                      go: () => go(api, name, goPackage ?? snake(path.basename(folder)).replace(/_/g, '')),
                      ts: () => typescript(api, name)};
  if (!generators[wrapper]) usage(`--wrapper is rust, python, go or ts, not ${wrapper}`);
  const text = generators[wrapper]();
  if (outFile) fs.writeFileSync(outFile, text);
  else process.stdout.write(text);
  process.exit(0);
}

// ---- Bundling ------------------------------------------------------------------

if (positional.length !== 2) usage();
const [source, folder] = positional;
const out = path.resolve(folder);

// The entry: a file of the application's, or one made for a package.
const packageName = (spec) => spec.startsWith('@') ? spec.split('@').slice(0, 2).join('@') : spec.split('@')[0];
let entry;
const isFile = fs.existsSync(source) && fs.statSync(source).isFile();
// A folder with a package.json: a package of the application's own.
const localPackage = !isFile && fs.existsSync(path.join(source, 'package.json'));
if (!isFile && !localPackage && /^(\.|\/|[A-Za-z]:[\\/])/.test(source)) usage(`no file or package folder ${source}`);
work = work ?? fs.mkdtempSync(path.join(os.tmpdir(), 'js-bundle-'));
fs.mkdirSync(work, {recursive: true});
const sourcePackage = localPackage ? JSON.parse(fs.readFileSync(path.join(source, 'package.json'), 'utf8')).name
                                   : packageName(source);
if (!isFile) install.unshift(localPackage ? `file:${path.resolve(source)}` : source);

// npm: the packages, and esbuild and cjs-module-lexer unless a folder with
// them was given; scripts never run.
const npm = process.platform === 'win32' ? 'npm.cmd' : 'npm';
const wanted = [...install];
if (!esbuildDir) wanted.push(`esbuild@${ESBUILD_VERSION}`, `cjs-module-lexer@${LEXER_VERSION}`);
const installed = (spec) => {
  if (spec.startsWith('file:')) return false;
  const name = packageName(spec), version = spec.slice(name.length + 1);
  try {
    const manifest = JSON.parse(fs.readFileSync(path.join(work, 'node_modules', name, 'package.json'), 'utf8'));
    return !version || manifest.version === version;
  } catch (e) {
    return false;
  }
};
const missing = wanted.filter((spec) => !installed(spec));
if (missing.length) {
  if (!fs.existsSync(path.join(work, 'package.json'))) {
    fs.writeFileSync(path.join(work, 'package.json'), JSON.stringify({private: true, name: 'js-bundle-work'}) + '\n');
  }
  // Everything wanted at once: an install removes what it is not given.
  const args = ['install', '--ignore-scripts', '--no-audit', '--no-fund', '--save=false', '--loglevel=error',
                ...(offline ? ['--offline'] : []), ...wanted];
  console.error(`js-bundle: npm ${args.join(' ')}`);
  const result = spawnSync(npm, args, {cwd: work, stdio: ['ignore', 'inherit', 'inherit'],
                                       shell: process.platform === 'win32'});
  if (result.status !== 0) {
    console.error(`js-bundle: npm install failed (${result.status ?? result.signal})`);
    process.exit(1);
  }
}
const nodeModules = path.join(work, 'node_modules');
const require = createRequire(path.join(esbuildDir ?? nodeModules, 'index.js'));
let esbuild;
try {
  esbuild = require('esbuild');
} catch (error) {
  console.error(`js-bundle: no esbuild in ${esbuildDir ?? work}: ${error.message}`);
  process.exit(1);
}
if (esbuild.version !== ESBUILD_VERSION) {
  console.error(`js-bundle: note: esbuild ${esbuild.version}, not the pinned ${ESBUILD_VERSION}`);
}

// What a library cannot have: Node.js built-ins (unless --empty) and native
// addons, each named with the module that imports it. zebkit:host is the
// engine's.
const builtins = new Set(builtinModules.flatMap((name) => [name, `node:${name}`]));
const refused = [];
const guard = {
  name: 'zebkit-library',
  setup(build) {
    build.onResolve({filter: /.*/}, (args) => {
      if (args.path === 'zebkit:host') return {path: args.path, external: true};
      if (empty.has(args.path)) return {path: args.path, namespace: 'zebkit-empty'};
      if (builtins.has(args.path)) {
        refused.push(`${args.path} (a Node.js built-in), imported by ${path.relative(process.cwd(), args.importer) || args.importer}`);
        return {path: args.path, namespace: 'zebkit-empty'};
      }
      if (args.path.endsWith('.node')) {
        refused.push(`${args.path} (a native addon), imported by ${args.importer}`);
        return {path: args.path, namespace: 'zebkit-empty'};
      }
      return undefined;
    });
    build.onLoad({filter: /.*/, namespace: 'zebkit-empty'}, () => ({contents: 'export default {};', loader: 'js'}));
  },
};
const options = {
  bundle: true,
  format: 'esm',
  platform: 'neutral',
  target: 'es2022',
  mainFields: ['browser', 'module', 'main'],
  conditions: ['browser', 'import', 'module', 'production'],
  define: {'process.env.NODE_ENV': '"production"'},
  loader: {'.json': 'json', '.wasm': 'binary'},
  nodePaths: [nodeModules],
  legalComments: 'none',
  metafile: true,
  logLevel: 'silent',
  plugins: [guard],
};

// The names a CommonJS module exports, as Node.js finds them (without
// running it), following its re-exports of relative files.
function cjsExports(file, seen = new Set()) {
  if (seen.has(file) || seen.size > 64) return [];
  seen.add(file);
  let lexer;
  try {
    lexer = require('cjs-module-lexer');
    lexer.initSync?.();
  } catch (error) {
    console.error(`js-bundle: no cjs-module-lexer in ${esbuildDir ?? work}: ${error.message}`);
    process.exit(1);
  }
  const {exports, reexports} = lexer.parse(fs.readFileSync(file, 'utf8'));
  const names = [...exports];
  for (const specifier of reexports) {
    if (!specifier.startsWith('.')) continue;
    const base = path.resolve(path.dirname(file), specifier);
    const target = [base, base + '.js', base + '.cjs', path.join(base, 'index.js')]
      .find((candidate) => fs.existsSync(candidate) && fs.statSync(candidate).isFile());
    if (target) names.push(...cjsExports(target, seen));
  }
  return [...new Set(names)].filter((name) => name !== '__esModule' && name !== 'default');
}

let described = {};
if (isFile) {
  entry = path.resolve(source);
  described = fromSource(fs.readFileSync(entry, 'utf8'));
} else {
  const name = sourcePackage;
  entry = path.join(work, 'js-bundle-entry.mjs');
  fs.writeFileSync(entry, `export * from ${JSON.stringify(name)};\n`);
  // The package as esbuild resolves it: an ES module, or CommonJS.
  const probe = await esbuild.build({...options, entryPoints: [entry], write: false});
  refused.length = 0;
  const imported = probe.metafile.inputs[path.relative(process.cwd(), entry).split(path.sep).join('/')]?.imports?.[0]?.path;
  const main = imported ? path.resolve(imported) : null;
  if (main && probe.metafile.inputs[imported]?.format === 'cjs') {
    const names = cjsExports(main);
    const ids = names.map((_, i) => `_${i}`);
    fs.writeFileSync(entry, `// js-bundle.mjs: ${name} is CommonJS; what it exports, as functions of the library.\n` +
      `import lib from ${JSON.stringify(name)};\n` +
      `const pick = (name) => typeof lib[name] === 'function' ? lib[name].bind(lib) : lib[name];\n` +
      names.map((n, i) => `const ${ids[i]} = pick(${JSON.stringify(n)});\n`).join('') +
      (names.length ? `export {${names.map((n, i) => `${ids[i]} as ${JSON.stringify(n)}`).join(', ')}};\n` : '') +
      `export default lib;\n`);
    console.error(`js-bundle: ${name} is CommonJS: ${names.length ? names.join(', ') : 'module.exports as "default"'}`);
  }
  described = fromDeclarations(declarationsOf(path.join(nodeModules, name), name, nodeModules));
}

fs.mkdirSync(out, {recursive: true});
const outfile = path.join(out, 'index.mjs');
let result;
try {
  result = await esbuild.build({...options, entryPoints: [entry], outfile, minify});
} catch (error) {
  for (const message of error.errors ?? []) {
    const at = message.location ? `${message.location.file}:${message.location.line}: ` : '';
    console.error(`js-bundle: ${at}${message.text}`);
  }
  if (!error.errors) console.error(`js-bundle: ${error.message}`);
  process.exit(1);
}
const computed = result.warnings.filter((w) => /require|import\(\)|dynamic/i.test(w.text));
for (const warning of result.warnings) {
  const at = warning.location ? `${warning.location.file}:${warning.location.line}: ` : '';
  console.error(`js-bundle: warning: ${at}${warning.text}`);
}
if (refused.length) {
  console.error('js-bundle: a library has no Node.js built-ins or native addons; these imports cannot be bundled ' +
                '(--empty <module> takes one as an empty module):');
  for (const line of [...new Set(refused)]) console.error(`  ${line}`);
  fs.rmSync(outfile, {force: true});
  process.exit(1);
}
if (computed.length) {
  console.error('js-bundle: note: imports computed at run time are not bundled; they fail when they run');
}

// The licences of the packages in the bundle.
const packages = new Map();
for (const input of Object.keys(result.metafile.inputs)) {
  const match = /^(.*node_modules[\\/](?:@[^\\/]+[\\/])?[^\\/]+)[\\/]/.exec(input);
  if (!match || packages.has(match[1])) continue;
  const dir = path.resolve(match[1]);
  let manifest = {};
  try { manifest = JSON.parse(fs.readFileSync(path.join(dir, 'package.json'), 'utf8')); } catch (e) {}
  const licenceFile = fs.existsSync(dir) ? fs.readdirSync(dir).find((name) => /^(licen[cs]e|copying)/i.test(name)) : null;
  packages.set(match[1], {
    name: manifest.name ?? path.basename(dir),
    version: manifest.version ?? '',
    license: typeof manifest.license === 'string' ? manifest.license : manifest.license?.type ?? 'see the package',
    text: licenceFile ? fs.readFileSync(path.join(dir, licenceFile), 'utf8').trim() : '',
  });
}
const notices = [...packages.values()].sort((a, b) => a.name.localeCompare(b.name)).map((p) =>
  `${p.name}${p.version ? ' ' + p.version : ''} (${p.license})\n\n${p.text || '(no licence file in the package)'}\n`);
fs.writeFileSync(path.join(out, 'LICENSES.txt'),
                 notices.length ? notices.join('\n' + '-'.repeat(72) + '\n\n') : 'No packages are in this bundle.\n');

// The functions it exports, as zebkit lists them, described where the entry
// or the package's declarations say how.
const exported = result.metafile.outputs[path.relative(process.cwd(), outfile).split(path.sep).join('/')]?.exports ?? [];
const functions = functionsOf(outfile);
if (!functions) console.error('js-bundle: note: the bundle did not import in Node.js; exports.json lists every export');
const api = {functions: (functions ?? exported).map((name) => ({name, doc: '', params: null, returns: null,
                                                               ...described[name]}))};
fs.writeFileSync(path.join(out, 'exports.json'), JSON.stringify(api, null, 2) + '\n');
const size = fs.statSync(outfile).size;
console.error(`js-bundle: ${path.relative(process.cwd(), outfile) || outfile}: ${(size / 1024).toFixed(1)} KiB, ` +
              `${packages.size} package${packages.size === 1 ? '' : 's'}, ${api.functions.length} function` +
              `${api.functions.length === 1 ? '' : 's'}`);
