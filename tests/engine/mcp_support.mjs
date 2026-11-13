// What tests/engine/mcp_server.mjs and mcp_agent_run.mjs share: starting the
// server, an official MCP client over either transport, and fixtures.

import { spawn } from 'node:child_process';
import { createRequire } from 'node:module';
import fs from 'node:fs';
import path from 'node:path';

export function loadSdk(toolsDir) {
  const require = createRequire(path.resolve(toolsDir, 'package.json'));
  const { Client } = require('@modelcontextprotocol/sdk/client/index.js');
  const { StdioClientTransport } = require('@modelcontextprotocol/sdk/client/stdio.js');
  const { StreamableHTTPClientTransport } = require('@modelcontextprotocol/sdk/client/streamableHttp.js');
  return { Client, StdioClientTransport, StreamableHTTPClientTransport };
}

let failures = 0, checks = 0;
export function check(ok, what, detail) {
  checks++;
  if (!ok) failures++;
  const shown = detail === undefined ? '' : `: ${String(typeof detail === 'string' ? detail : JSON.stringify(detail)).slice(0, 400)}`;
  console.log(`${ok ? 'ok   ' : 'FAIL '}${what}${ok ? '' : shown}`);
}
const skipped = [];
/** A part not run on this machine: a "skip " line, never a pass. */
export function skip(what, why) {
  skipped.push(what);
  console.log(`skip ${what}: ${why}`);
}
/** The summary line, last; the exit code: 0 only when there were checks and none failed. */
export function finish() {
  if (skipped.length) console.log(`${skipped.length} skipped: ${skipped.join('; ')}`);
  console.log(`${checks - failures} of ${checks} checks passed`);
  return failures || !checks ? 1 : 0;
}

/** A client whose calls give {result, structured, text, isError}, and keeps every result. */
export function wrap(client, transcript) {
  return {
    client,
    async call(name, args = {}, options = {}) {
      const result = await client.callTool({ name, arguments: args }, undefined, { timeout: 180000, ...options });
      transcript.push(JSON.stringify(result));
      return {
        result, structured: result.structuredContent ?? {}, isError: !!result.isError,
        text: (result.content ?? []).filter(item => item.type === 'text').map(item => item.text).join('\n'),
        image: (result.content ?? []).find(item => item.type === 'image'),
      };
    },
  };
}

export async function stdioClient(sdk, binary, args, env = {}) {
  const transport = new sdk.StdioClientTransport({ command: binary, args, stderr: 'pipe', env: { ...process.env, ...env } });
  const log = { text: '' };
  const client = new sdk.Client({ name: 'zebkit-engine-test', version: '1' });
  const connecting = client.connect(transport);
  transport.stderr?.on('data', data => { log.text += data; });
  await connecting;
  return { client, log, close: () => client.close() };
}

/** Starts the server with --http 0, waits until it says where. */
export function startHttp(binary, args, env = {}) {
  const child = spawn(binary, ['--http', '0', ...args], { stdio: ['ignore', 'ignore', 'pipe'], env: { ...process.env, ...env } });
  const log = { text: '' };
  const port = new Promise((resolve, reject) => {
    const timer = setTimeout(() => reject(new Error('the server did not start: ' + log.text)), 120000);
    child.stderr.on('data', data => {
      log.text += data;
      const found = log.text.match(/listening on http:\/\/127\.0\.0\.1:(\d+)\/mcp/);
      if (found) { clearTimeout(timer); resolve(Number(found[1])); }
    });
    child.on('exit', code => { clearTimeout(timer); reject(new Error(`the server exited with ${code}: ${log.text}`)); });
  });
  return { child, log, port };
}

export async function httpClient(sdk, port, token) {
  const transport = new sdk.StreamableHTTPClientTransport(new URL(`http://127.0.0.1:${port}/mcp`), {
    requestInit: { headers: { Authorization: `Bearer ${token}` } },
  });
  const client = new sdk.Client({ name: 'zebkit-engine-test', version: '1' });
  await client.connect(transport);
  return client;
}

/** Ends a server, whatever it is doing. */
export async function stop(child) {
  if (child.exitCode !== null) return;
  child.kill('SIGTERM');
  const ended = new Promise(resolve => child.once('exit', resolve));
  const timer = setTimeout(() => child.kill('SIGKILL'), 10000);
  await ended;
  clearTimeout(timer);
}

const escapeText = text => text.replace(/[\\()]/g, '\\$&');

/**
 * A PDF of pages, each an array of lines, with an outline to its first pages and
 * a link on the first; with `form`, a text field and a check box.
 */
export function makePdf(pages, { form = false } = {}) {
  const objects = [];
  const add = body => { objects.push(body); return objects.length; };
  const reserve = () => { objects.push(''); return objects.length; };
  const catalog = reserve(), tree = reserve(), font = add('<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>');
  const outlines = reserve();
  const pageIds = [];
  let widgets = [];
  if (form) {
    widgets = [
      add('<< /Type /Annot /Subtype /Widget /FT /Tx /T (name) /V (old) /Rect [50 600 300 630] /DA (/F1 12 Tf 0 g) /F 4 >>'),
      add('<< /Type /Annot /Subtype /Widget /FT /Btn /T (agree) /V /Off /AS /Off /Rect [50 560 70 580] /F 4 /AP << /N << /Yes << >> /Off << >> >> >> >>'),
    ];
  }
  pages.forEach((lines, index) => {
    const text = lines.map((line, n) => `1 0 0 1 50 ${740 - n * 16} Tm (${escapeText(line)}) Tj`).join('\n');
    const content = add(`<< /Length ${text.length + 18} >>\nstream\nBT /F1 12 Tf\n${text}\nET\nendstream`);
    const annots = [];
    if (index === 0) {
      annots.push(add('<< /Type /Annot /Subtype /Link /Rect [50 700 200 720] /A << /S /URI /URI (https://example.com/doc) >> >>'));
      annots.push(...widgets);
    }
    pageIds.push(add(`<< /Type /Page /Parent ${tree} 0 R /MediaBox [0 0 612 792] /Resources << /Font << /F1 ${font} 0 R >> >> /Contents ${content} 0 R${annots.length ? ` /Annots [${annots.map(id => `${id} 0 R`).join(' ')}]` : ''} >>`));
  });
  const chapters = pageIds.slice(0, Math.min(3, pageIds.length));
  const items = chapters.map((page, index) => {
    const first = outlines + 1000; // replaced below
    return { page, index, first };
  });
  const itemIds = items.map(() => reserve());
  items.forEach((item, index) => {
    const links = [index > 0 ? `/Prev ${itemIds[index - 1]} 0 R` : '', index < items.length - 1 ? `/Next ${itemIds[index + 1]} 0 R` : ''].join(' ');
    objects[itemIds[index] - 1] = `<< /Title (Chapter ${index + 1}) /Parent ${outlines} 0 R /Dest [${item.page} 0 R /Fit] ${links} >>`;
  });
  objects[outlines - 1] = `<< /Type /Outlines /First ${itemIds[0]} 0 R /Last ${itemIds[itemIds.length - 1]} 0 R /Count ${itemIds.length} >>`;
  objects[tree - 1] = `<< /Type /Pages /Kids [${pageIds.map(id => `${id} 0 R`).join(' ')}] /Count ${pageIds.length} >>`;
  const acro = form ? ` /AcroForm << /Fields [${widgets.map(id => `${id} 0 R`).join(' ')}] /DR << /Font << /F1 ${font} 0 R >> >> /DA (/F1 12 Tf 0 g) >>` : '';
  objects[catalog - 1] = `<< /Type /Catalog /Pages ${tree} 0 R /Outlines ${outlines} 0 R${acro} >>`;
  let out = '%PDF-1.7\n';
  const offsets = [];
  objects.forEach((body, index) => {
    offsets.push(Buffer.byteLength(out, 'latin1'));
    out += `${index + 1} 0 obj\n${body}\nendobj\n`;
  });
  const start = Buffer.byteLength(out, 'latin1');
  out += `xref\n0 ${objects.length + 1}\n0000000000 65535 f \n`;
  out += offsets.map(offset => `${String(offset).padStart(10, '0')} 00000 n \n`).join('');
  out += `trailer\n<< /Size ${objects.length + 1} /Root ${catalog} 0 R >>\nstartxref\n${start}\n%%EOF\n`;
  return Buffer.from(out, 'latin1');
}

/** The file tests/engine/pdf.c keeps as a C string: a PDF signed for real. */
export function signedPdf(testsDir) {
  const source = fs.readFileSync(path.join(testsDir, 'pdf.c'), 'latin1');
  const body = source.split('static const char really_signed[] =')[1].split(';\n')[0];
  let out = '';
  for (const [, literal] of body.matchAll(/"((?:[^"\\]|\\.)*)"/g)) {
    out += literal.replace(/\\(n|r|t|"|\\|x[0-9a-fA-F]{2}|[0-7]{1,3})/g, (_, escape) => {
      if (escape === 'n') return '\n';
      if (escape === 'r') return '\r';
      if (escape === 't') return '\t';
      if (escape === '"' || escape === '\\') return escape;
      if (escape[0] === 'x') return String.fromCharCode(parseInt(escape.slice(1), 16));
      return String.fromCharCode(parseInt(escape, 8));
    });
  }
  return Buffer.from(out, 'latin1');
}
