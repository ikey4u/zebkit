// The connection benchmark's server (scripts/connection-bench.sh,
// docs/CONNECTION_REUSE.md, step 0): HTTPS on two counted ports, one with
// ALPN h2 and http/1.1, one with http/1.1 only, each behind a proxy that
// delays every chunk by half a round trip each way (and the first one from
// the client by one more round trip, for TCP's own handshake), so that saved
// round trips show on loopback. What reached it is answered on a third, plain
// port, which is not counted.
//
//   node server.mjs <certificate directory> <round trip ms>
//
// The directory has key.pem and <host>.pem for each host: one key, one
// certificate per name, so that HTTP/2 cannot pool the hosts on one
// connection. It prints one JSON line {"h2", "h1", "control"} with the ports.
//
// Control port: GET /stats (what was counted since the last reset), GET
// /reset, GET /quit.

import fs from 'node:fs';
import http from 'node:http';
import http2 from 'node:http2';
import https from 'node:https';
import net from 'node:net';
import path from 'node:path';
import tls from 'node:tls';

const [directory, rttText] = process.argv.slice(2);
const rtt = Number(rttText ?? 40);
const KEEP_ALIVE = 10 * 60 * 1000;
const HOSTS = ['a.bench.test', 'b.bench.test', 'c.other.test', 'cdn.shared.test'];
const key = fs.readFileSync(path.join(directory, 'key.pem'));
const contexts = Object.fromEntries(HOSTS.map((host) => [host, tls.createSecureContext({
  key, cert: fs.readFileSync(path.join(directory, `${host}.pem`))})]));
const sni = (name, done) => done(null, contexts[name] ?? contexts[HOSTS[0]]);

let stats;
const reset = () => {
  stats = {tcp: {h2: 0, h1: 0}, fullHandshakes: 0, resumedHandshakes: 0, h2Sessions: 0,
           requests: 0, notModified: 0, byHost: {}, perConnection: {}};
};
reset();
const host = (name) => (stats.byHost[name] ??= {tcp: 0, fullHandshakes: 0, resumedHandshakes: 0, requests: 0,
                                               notModified: 0});

// ---- responses ----

const GIF = Buffer.from('R0lGODlhAQABAIAAAP///wAAACH5BAEAAAAALAAAAAABAAEAAAICRAEAOw==', 'base64');
const SUBRESOURCES = 10;  // of each kind: max-age and no-cache

function page(query, origin, port) {
  const b = `https://b.bench.test:${port}`;
  const images = [];
  for (let i = 0; i < SUBRESOURCES; i++)
    images.push(`<img src="/r/cache-${i}.gif">`, `<img src="/r/nocache-${i}.gif">`);
  return `<!doctype html><meta charset="utf-8"><title>bench</title>
${query.get('preconnect') ? `<link rel="preconnect" href="${b}">` : ''}
<script>addEventListener('pageshow', (e) => { if (e.persisted) window.restored = (window.restored || 0) + 1; });</script>
${images.join('\n')}
<script src="/r/late.js"></script>
`;
}

// A page on another site using a shared host's resources (partitioning).
function crossSite(port) {
  const cdn = `https://cdn.shared.test:${port}`;
  return `<!doctype html><meta charset="utf-8"><title>cross-site</title>
<img src="${cdn}/lib/cache-0.gif"><img src="${cdn}/lib/cache-1.gif"><img src="${cdn}/lib/nocache-0.gif">
`;
}

function respond(method, url, headers, name, port, send) {
  const query = url.searchParams;
  const etag = `"v1-${url.pathname}"`;
  const fresh = headers['if-none-match'] === etag;
  const common = {'timing-allow-origin': '*'};
  if (url.pathname === '/page.html')
    return send(200, {...common, 'content-type': 'text/html', 'cache-control': 'no-cache'}, page(query, name, port));
  if (url.pathname === '/page2.html')
    return send(200, {...common, 'content-type': 'text/html', 'cache-control': 'no-cache'},
                '<!doctype html><title>page2</title>');
  if (url.pathname === '/cross-site.html')
    return send(200, {...common, 'content-type': 'text/html', 'cache-control': 'no-cache'}, crossSite(port));
  if (url.pathname === '/r/late.js') {
    if (fresh) return send(304, {...common, etag, 'cache-control': 'no-cache'}, '');
    const b = `https://b.bench.test:${port}/r/b.gif`;
    return send(200, {...common, 'content-type': 'text/javascript', 'cache-control': 'no-cache', etag},
                `document.body.appendChild(Object.assign(new Image(), {src: ${JSON.stringify(b)}}));`);
  }
  if (url.pathname === '/data')
    return send(200, {...common, 'content-type': 'application/json', 'cache-control': 'no-store'}, '{"ok":true}');
  if (/^\/(r|lib)\/cache-\d+\.gif$/.test(url.pathname))
    return send(200, {...common, 'content-type': 'image/gif', 'cache-control': 'max-age=3600'}, GIF);
  if (/^\/(r|lib)\/(nocache-\d+|b)\.gif$/.test(url.pathname)) {
    if (fresh) return send(304, {...common, etag, 'cache-control': 'no-cache'}, '');
    return send(200, {...common, 'content-type': 'image/gif', 'cache-control': 'no-cache', etag}, GIF);
  }
  return send(404, {'content-type': 'text/plain'}, 'not found');
}

function handler(port) {
  return (request, response) => {
    const name = (request.headers[':authority'] ?? request.headers.host ?? '').replace(/:\d+$/, '');
    const connection = String(request.socket.remotePort);
    stats.requests++;
    host(name).requests++;
    stats.perConnection[connection] = (stats.perConnection[connection] ?? 0) + 1;
    respond(request.method, new URL(request.url, 'https://x'), request.headers, name, port(), (status, headers, body) => {
      if (status === 304) {
        stats.notModified++;
        host(name).notModified++;
      }
      response.writeHead(status, headers);
      response.end(request.method === 'HEAD' ? undefined : body);
    });
  };
}

function counted(server, kind) {
  server.on('secureConnection', (socket) => {
    const reused = socket.isSessionReused();
    if (reused) stats.resumedHandshakes++;
    else stats.fullHandshakes++;
    host(socket.servername ?? '')[reused ? 'resumedHandshakes' : 'fullHandshakes']++;
    socket.on('error', () => {});
  });
  server.on('tlsClientError', () => {});
  server.keepAliveTimeout = KEEP_ALIVE;
  server.headersTimeout = KEEP_ALIVE + 1000;
  server.requestTimeout = 0;
  server.timeout = 0;
  return server;
}

const ports = {};
const h2 = counted(http2.createSecureServer({SNICallback: sni, allowHTTP1: true, ALPNProtocols: ['h2', 'http/1.1'],
                                             settings: {maxConcurrentStreams: 100}},
                                            handler(() => ports.h2)), 'h2');
h2.on('session', (session) => {
  stats.h2Sessions++;
  session.on('error', () => {});
  session.setTimeout(0);
});
const h1 = counted(https.createServer({SNICallback: sni, ALPNProtocols: ['http/1.1']}, handler(() => ports.h1)), 'h1');

// ---- the delaying proxies ----

// Each chunk is passed on half a round trip later; timers of one duration run
// in order, so the stream stays in order. The client's first chunk waits a
// round trip more: what TCP's handshake costs a real connection.
function delayed(from, to, extraFirst) {
  let first = extraFirst;
  from.on('data', (chunk) => {
    const delay = rtt / 2 + (first ? rtt : 0);
    first = false;
    setTimeout(() => { if (!to.destroyed) to.write(chunk); }, delay);
  });
  from.on('end', () => setTimeout(() => to.end(), rtt / 2));
  from.on('error', () => to.destroy());
  from.on('close', () => setTimeout(() => to.destroy(), rtt / 2));
}

function proxy(kind, backPort) {
  const server = net.createServer({noDelay: true}, (client) => {
    stats.tcp[kind]++;
    const back = net.connect({port: backPort, host: '127.0.0.1', noDelay: true});
    delayed(client, back, rtt > 0);
    delayed(back, client, false);
  });
  return server;
}

function listen(server) {
  return new Promise((resolve) => server.listen(0, '127.0.0.1', () => resolve(server.address().port)));
}

const control = http.createServer((request, response) => {
  const url = new URL(request.url, 'http://x');
  response.setHeader('content-type', 'application/json');
  if (url.pathname === '/stats') return response.end(JSON.stringify(stats));
  if (url.pathname === '/reset') {
    reset();
    return response.end('{}');
  }
  if (url.pathname === '/quit') {
    response.end('{}');
    process.exit(0);
  }
  response.statusCode = 404;
  response.end('{}');
});

const h2Back = await listen(h2);
const h1Back = await listen(h1);
ports.h2 = await listen(proxy('h2', h2Back));
ports.h1 = await listen(proxy('h1', h1Back));
const controlPort = await listen(control);
console.log(JSON.stringify({h2: ports.h2, h1: ports.h1, control: controlPort, rtt}));
