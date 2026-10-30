import http2 from 'node:http2';
import fs from 'node:fs';
const server = http2.createSecureServer({key: fs.readFileSync(process.argv[2]), cert: fs.readFileSync(process.argv[3])});
const sessions = new WeakMap();
let nextSession = 0;
server.on('session', session => { sessions.set(session, ++nextSession); session.on('error', () => {}); });
server.on('tlsClientError', () => {});
server.on('stream', (stream, headers) => {
  stream.on('error', () => {});
  console.log(JSON.stringify({kind: 'request', path: headers[':path'], session: sessions.get(stream.session)}));
  stream.respond({':status': 200, 'content-type': 'text/html', 'cache-control': 'no-store'});
  stream.end('<title>certificate-ok</title>');
});
server.listen(0, '127.0.0.1', () => console.log(JSON.stringify({kind: 'ready', port: server.address().port})));
