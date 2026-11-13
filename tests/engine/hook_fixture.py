"""Local HTTP and TLS origins for the hooks of tests/engine/hook_policies*: a page's
links, downloads, client certificates, isolated worlds, file choosers and media.
One fixture a mode, each on ports of its own, so that modes sharing an engine
share no origin (no cookies, service workers or remembered client certificates,
which Chromium keeps per host and port)."""
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import atexit
import base64
import hashlib
import json
import os
import shutil
import ssl
import subprocess
import tempfile
import threading
import time

PAGE = b'<!doctype html><title>page</title><input id=i type=file><input id=m type=file multiple><input id=d type=file webkitdirectory>'
WORLD = b'''<!doctype html><title>world</title><script>
var pageVar = 1;
dispatchEvent(new Event('zkping'));
window.report = {all: document.documentElement.getAttribute('data-all'),
                 main: document.documentElement.getAttribute('data-main'),
                 global: typeof worldVar, mainWorld: typeof mainWorldVar};
if (parent !== self) parent.postMessage(report, '*');
else { window.reports = []; addEventListener('message', e => reports.push(e.data)); }
</script>'''
FRAMES = WORLD + b'<iframe src="/world"></iframe><iframe src="OTHER/world"></iframe>'
INPUT = b'''<!doctype html><title>input</title><a id=l href="/page" style="position:fixed;left:0;top:0;width:200px;height:200px;display:block">link</a><script>
window.keys = [];
addEventListener('keydown', e => keys.push(e.code));
addEventListener('contextmenu', e => { window.menu = 1; });
onload = () => requestAnimationFrame(() => requestAnimationFrame(() => { document.title = 'ready'; }));
</script>'''
# Asked by a message, answered in one: a fetch that the network hook may stop, or a socket.
WORKER_TASK = b'''
const task = (what, reply) => {
  if (what === 'socket') {
    const s = new WebSocket(self.location.origin.replace('http', 'ws') + '/ws');
    s.onmessage = e => { reply(e.data); s.close(); };
    s.onerror = () => reply('failed');
  } else fetch('/worker-held').then(r => r.text()).then(reply, () => reply('failed'));
};
'''
SERVICE_WORKER = WORKER_TASK + b'''
self.addEventListener('install', () => skipWaiting());
self.addEventListener('activate', e => e.waitUntil(clients.claim()));
self.addEventListener('message', e => task(e.data, m => e.source.postMessage(m)));
'''
SHARED_WORKER = WORKER_TASK + b'''
onconnect = e => { const port = e.ports[0]; port.onmessage = m => task(m.data, a => port.postMessage(a)); };
'''
BIG = 65 << 20  # Past what a body is told up to.


class Handler(BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'
    def log_message(self, *args): pass

    def reply(self, body, kind='text/html', extra=()):
        self.send_response(200)
        self.send_header('Content-Type', kind)
        self.send_header('Cache-Control', 'no-store')
        self.send_header('Content-Length', str(len(body)))
        for name, value in extra:
            self.send_header(name, value)
        self.end_headers()
        try: self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError): pass

    def socket(self, fixture):
        fixture.echoed.append(dict(self.headers.items()))
        accept = base64.b64encode(hashlib.sha1(self.headers['Sec-WebSocket-Key'].encode()
                                               + b'258EAFA5-E914-47DA-95CA-C5AB0DC85B11').digest())
        self.close_connection = True
        try:
            self.wfile.write(b'HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n'
                             b'Sec-WebSocket-Accept: ' + accept + b'\r\n\r\n\x81\x05hello')
            self.wfile.flush()
            while self.rfile.read(1): pass
        except OSError: pass

    def do_POST(self):
        self.rfile.read(int(self.headers.get('Content-Length', '0')))
        self.do_GET()

    def do_GET(self):
        fixture = self.server.fixture
        path = self.path.split('?')[0]
        fixture.received.append(path)
        if self.server.tls:
            peer = self.connection.getpeercert()
            name = next((v for rdn in (peer or {}).get('subject', ()) for k, v in rdn if k == 'commonName'), 'none')
            fixture.peers.append(name)
            self.reply(f'<title>cert:{name}</title>'.encode())
        elif path == '/download.bin':
            self.reply(b'z' * 65536, 'application/x-zebkit',
                       [('Content-Disposition', 'attachment; filename="report.bin"')])
        elif path == '/slow.bin':
            self.send_response(200)
            self.send_header('Content-Type', 'application/x-zebkit')
            self.send_header('Content-Disposition', 'attachment; filename="slow.bin"')
            self.send_header('Content-Length', str(1 << 24))
            self.end_headers()
            try:
                for _ in range(256):
                    self.wfile.write(b's' * 65536)
                    time.sleep(.05)
            except (BrokenPipeError, ConnectionResetError): pass
        elif path == '/ws': self.socket(fixture)
        elif path == '/big':
            self.send_response(200)
            self.send_header('Content-Type', 'application/x-zebkit')
            self.send_header('Content-Length', str(BIG))
            self.end_headers()
            try:
                for _ in range(BIG >> 16):
                    self.wfile.write(b'b' * 65536)
            except (BrokenPipeError, ConnectionResetError): pass
        elif path == '/slow-text':
            self.send_response(200)
            self.send_header('Content-Type', 'text/plain')
            self.send_header('Content-Length', str(1 << 24))
            self.end_headers()
            try:
                for _ in range(256):
                    self.wfile.write(b's' * 65536)
                    self.wfile.flush()
                    time.sleep(.05)
            except (BrokenPipeError, ConnectionResetError): pass
        elif path == '/cut':
            self.send_response(200)
            self.send_header('Content-Type', 'text/plain')
            self.send_header('Content-Length', '100000')
            self.end_headers()
            self.wfile.write(b'c' * 1000)
            self.wfile.flush()
            self.close_connection = True
        elif path == '/worker-held': self.reply(b'through', 'text/plain')
        elif path == '/net-sw.js': self.reply(SERVICE_WORKER, 'text/javascript')
        elif path == '/net-shared.js': self.reply(SHARED_WORKER, 'text/javascript')
        elif path == '/echo':
            fixture.echoed.append(dict(self.headers.items()))
            self.reply(json.dumps({k.lower(): v for k, v in self.headers.items()}).encode(), 'application/json')
        elif path == '/server-redirect':
            self.send_response(302)
            self.send_header('Location', '/echo')
            self.send_header('Content-Length', '0')
            self.end_headers()
        elif path == '/net-worker':
            self.reply(b"fetch('/echo?worker').then(r => r.text()).then(t => postMessage(t), e => postMessage('failed'))",
                       'text/javascript')
        elif path == '/world': self.reply(WORLD)
        elif path == '/frames': self.reply(FRAMES.replace(b'OTHER', fixture.other.encode()))
        elif path == '/input': self.reply(INPUT)
        else: self.reply(PAGE)


class Server(ThreadingHTTPServer):
    daemon_threads = True
    # A client that goes, or refuses a certificate, is what these tests do.
    def handle_error(self, request, client_address): pass


_keys = None
_big = None


def _certificates():
    """The server's, the client's and a stranger's key and certificate: made
    once per process, for every fixture (three RSA keys a mode were most of a
    fixture's cost)."""
    global _keys
    if _keys is None:
        _keys = tempfile.TemporaryDirectory(prefix='zebkit-hook-keys-')
        atexit.register(_keys.cleanup)
        made = Path(_keys.name)
        for stem, name in (('server', 'localhost'), ('client', 'zebkit-client'), ('stranger', 'stranger')):
            subprocess.run(['openssl', 'req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-days', '1',
                            '-keyout', str(made / f'{stem}.key'), '-out', str(made / f'{stem}.pem'),
                            '-subj', '/CN=' + name, '-addext', 'subjectAltName=DNS:localhost,IP:127.0.0.1'],
                           check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return Path(_keys.name)


def _big_file():
    """The 3 MiB file net-file answers with: written once per process too."""
    global _big
    if _big is None:
        _big = _certificates() / 'big.bin'
        _big.write_bytes(b'f' * (3 << 20))
    return _big


class HookFixture:
    def __init__(self):
        self.work = tempfile.TemporaryDirectory(prefix='zebkit-hook-fixture-')
        self.dir = Path(self.work.name)
        keys = _certificates()
        for stem in ('server', 'client', 'stranger'):
            for suffix in ('.key', '.pem'):
                shutil.copyfile(keys / (stem + suffix), self.dir / (stem + suffix))
        self.received, self.peers, self.echoed = [], [], []
        self.http = self._serve(None)
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(self.dir / 'server.pem', self.dir / 'server.key')
        context.load_verify_locations(self.dir / 'client.pem')
        context.verify_mode = ssl.CERT_OPTIONAL
        self.https = self._serve(context)
        port = self.http.server_port
        self.base = f'http://127.0.0.1:{port}'
        # Another site: frames of it are in a process of their own.
        self.other = f'http://localhost:{port}'
        self.tls = f'https://localhost:{self.https.server_port}'
        self.files = self.dir / 'files'
        (self.files / 'folder' / 'sub').mkdir(parents=True)
        (self.files / 'one.txt').write_text('first file')
        (self.files / 'two.txt').write_text('second file')
        try:
            os.link(_big_file(), self.files / 'big.bin')
        except OSError:
            shutil.copyfile(_big_file(), self.files / 'big.bin')
        (self.files / 'folder' / 'a.txt').write_text('a')
        (self.files / 'folder' / 'sub' / 'b.txt').write_text('b')

    def _serve(self, context):
        server = Server(('127.0.0.1', 0), Handler)
        server.fixture = self
        server.tls = context is not None
        if context:
            server.socket = context.wrap_socket(server.socket, server_side=True, do_handshake_on_connect=False)
        threading.Thread(target=lambda: server.serve_forever(poll_interval=.02), daemon=True).start()
        return server

    def env(self, mode, downloads):
        return {'ZEBKIT_HOOK_MODE': mode, 'ZEBKIT_HOOK_BASE': self.base, 'ZEBKIT_HOOK_TLS': self.tls,
                'ZEBKIT_HOOK_CERT': str(self.dir / 'client.pem'), 'ZEBKIT_HOOK_KEY': str(self.dir / 'client.key'),
                'ZEBKIT_HOOK_OTHER_KEY': str(self.dir / 'stranger.key'),
                'ZEBKIT_HOOK_FILES': str(self.files), 'ZEBKIT_HOOK_DOWNLOADS': str(downloads)}

    def close(self):
        self.http.shutdown(); self.https.shutdown()
        self.http.server_close(); self.https.server_close()
        self.work.cleanup()
    def __enter__(self): return self
    def __exit__(self, *args): self.close()
