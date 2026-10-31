// The MCP project's own conformance suite (@modelcontextprotocol/conformance)
// against tools/zebkit-mcp over HTTP: the scenarios that apply to any server. The
// others need its reference server's own test tools, prompts and resources. Its
// runner cannot send a token, so a small proxy here adds one, and hands on the Host
// a client sent, loopback names at the server's real port, others as they came.
//
//   node mcp_conformance.mjs <dir with node_modules> <zebkit-mcp> [engine switches...]

import { execFile } from 'node:child_process';
import fs from 'node:fs';
import http from 'node:http';
import os from 'node:os';
import path from 'node:path';
import { check, finish, startHttp, stop } from './mcp_support.mjs';

const [toolsDir, binary, ...switches] = process.argv.slice(2);
const SCENARIOS = ['server-initialize', 'ping', 'tools-list', 'dns-rebinding-protection'];

const work = fs.mkdtempSync(path.join(os.tmpdir(), 'zebkit-mcp-conformance-'));
const tokenFile = path.join(work, 'token');
const server = startHttp(binary, ['--http-token-file', tokenFile, '--', ...switches]);
const port = await server.port;
const token = fs.readFileSync(tokenFile, 'utf8');

const proxy = http.createServer((request, response) => {
  const headers = { ...request.headers, authorization: `Bearer ${token}` };
  if (/^(127\.0\.0\.1|localhost)(:\d+)?$/.test(headers.host ?? '')) headers.host = `127.0.0.1:${port}`;
  if (headers.origin && /^http:\/\/(127\.0\.0\.1|localhost)(:\d+)?$/.test(headers.origin)) headers.origin = `http://127.0.0.1:${port}`;
  const forward = http.request({ host: '127.0.0.1', port, method: request.method, path: request.url, headers }, answer => {
    response.writeHead(answer.statusCode, answer.headers);
    answer.pipe(response);
  });
  forward.on('error', () => { response.writeHead(502); response.end(); });
  request.pipe(forward);
});
const proxyPort = await new Promise(resolve => proxy.listen(0, '127.0.0.1', () => resolve(proxy.address().port)));

const cli = path.join(toolsDir, 'node_modules/@modelcontextprotocol/conformance/dist/index.js');
for (const scenario of SCENARIOS) {
  const output = await new Promise(resolve => {
    execFile(process.execPath, [cli, 'server', '--url', `http://127.0.0.1:${proxyPort}/mcp`, '--scenario', scenario, '--output-dir', work],
      { timeout: 120000, cwd: work }, (error, stdout, stderr) => resolve({ code: error ? error.code ?? 1 : 0, text: `${stdout}\n${stderr}` }));
  });
  const summary = output.text.split('\n').filter(line => /passed|failed|Passed|Failed/.test(line)).pop()?.trim() ?? '';
  check(output.code === 0 && summary !== '', `conformance ${scenario}: ${summary}`, output.text.slice(-1500));
}

proxy.close();
proxy.closeAllConnections?.();
await stop(server.child);
fs.rmSync(work, { recursive: true, force: true });
process.exit(finish());
