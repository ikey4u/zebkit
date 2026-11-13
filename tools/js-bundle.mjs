#!/usr/bin/env node
// js-bundle.mjs: an npm package, or an entry module that imports packages,
// made into a library zebkit loads (<zebkit/library.h>, docs/JS_LIBRARIES.md):
// one ES module, <folder>/index.mjs, with nothing to import, and the
// packages' licences beside it in <folder>/LICENSES.txt.
//
//   node js-bundle.mjs <entry.mjs | package[@version]> <folder> [options]
//
//   --install <package@version>  a package the entry imports (repeatable);
//                                a package given as the first argument is one
//   --empty <module>             an import of a Node.js built-in (or of any
//                                module) taken as an empty module: for one the
//                                code only uses where it is there (repeatable)
//   --work <dir>                 where packages are installed (kept between
//                                runs); a temporary directory by default
//   --esbuild <dir>              a node_modules with esbuild installed, in
//                                place of installing it into the work dir
//   --minify                     the bundle minified
//   --offline                    npm's cache only: nothing downloaded
//
// A package as the first argument is bundled as `export * from '<package>'`,
// which suits packages that are ES modules; for a CommonJS package, or to
// choose what the library exports, write a small entry that imports it and
// exports functions. Build time only: zebkit never downloads code, and this
// does not run where the application runs.
//
// Packages are installed with npm --ignore-scripts (their install scripts do
// not run) and bundled with esbuild (upstream.lock's PLAYWRIGHT_ESBUILD_VERSION,
// 0.28.1): neutral platform, browser fields first, NODE_ENV=production, JSON
// and WebAssembly inlined (a .wasm import is its bytes, a Uint8Array). It
// cannot bundle native addons (.node), Node.js built-ins (each import is named;
// --empty takes one as empty) or imports computed at run time. Needs Node.js 18
// or later and npm.

import {spawnSync} from 'node:child_process';
import fs from 'node:fs';
import {builtinModules, createRequire} from 'node:module';
import os from 'node:os';
import path from 'node:path';

const ESBUILD_VERSION = '0.28.1';

function usage(message) {
  if (message) console.error(`js-bundle: ${message}`);
  console.error('usage: node js-bundle.mjs <entry.mjs | package[@version]> <folder> [--install pkg@ver]... ' +
                '[--empty module]... [--work dir] [--esbuild dir] [--minify] [--offline]');
  process.exit(2);
}

const positional = [], install = [], empty = new Set();
let work = null, esbuildDir = null, minify = false, offline = false;
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
  else if (arg === '--help' || arg === '-h') usage();
  else if (arg.startsWith('--')) usage(`unknown option ${arg}`);
  else positional.push(arg);
}
if (positional.length !== 2) usage();
const [source, folder] = positional;
const out = path.resolve(folder);

// The entry: a file of the application's, or one made for a package.
const packageName = (spec) => spec.startsWith('@') ? spec.split('@').slice(0, 2).join('@') : spec.split('@')[0];
let entry;
const isFile = fs.existsSync(source) && fs.statSync(source).isFile();
if (!isFile && /^(\.|\/|[A-Za-z]:[\\/])/.test(source)) usage(`no file ${source}`);
work = work ?? fs.mkdtempSync(path.join(os.tmpdir(), 'js-bundle-'));
fs.mkdirSync(work, {recursive: true});
if (!isFile) install.unshift(source);

// npm: the packages, and esbuild unless one was given; scripts never run.
const npm = process.platform === 'win32' ? 'npm.cmd' : 'npm';
const wanted = [...install];
if (!esbuildDir) wanted.push(`esbuild@${ESBUILD_VERSION}`);
const installed = (spec) => {
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
if (isFile) {
  entry = path.resolve(source);
} else {
  entry = path.join(work, 'js-bundle-entry.mjs');
  fs.writeFileSync(entry, `export * from ${JSON.stringify(packageName(source))};\n`);
}

const require = createRequire(path.join(esbuildDir ?? path.join(work, 'node_modules'), 'index.js'));
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
// addons, each named with the module that imports it.
const builtins = new Set(builtinModules.flatMap((name) => [name, `node:${name}`]));
const refused = [];
const guard = {
  name: 'zebkit-library',
  setup(build) {
    build.onResolve({filter: /.*/}, (args) => {
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

fs.mkdirSync(out, {recursive: true});
const outfile = path.join(out, 'index.mjs');
let result;
try {
  result = await esbuild.build({
    entryPoints: [entry],
    outfile,
    bundle: true,
    format: 'esm',
    platform: 'neutral',
    target: 'es2022',
    mainFields: ['browser', 'module', 'main'],
    conditions: ['browser', 'import', 'module', 'production'],
    define: {'process.env.NODE_ENV': '"production"'},
    loader: {'.json': 'json', '.wasm': 'binary'},
    nodePaths: [path.join(work, 'node_modules')],
    minify,
    legalComments: 'none',
    metafile: true,
    logLevel: 'silent',
    plugins: [guard],
  });
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
const size = fs.statSync(outfile).size;
console.error(`js-bundle: ${path.relative(process.cwd(), outfile) || outfile}: ${(size / 1024).toFixed(1)} KiB, ` +
              `${packages.size} package${packages.size === 1 ? '' : 's'}`);
