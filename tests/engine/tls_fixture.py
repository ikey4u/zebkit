"""A local HTTPS origin with an untrusted certificate of its own (one a process); never contacts a CA."""
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import atexit
import shutil
import ssl
import subprocess
import tempfile
import threading

class Server(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    def log_message(self, *args): pass
    def do_GET(self):
        self.server.received.append(self.path)
        body = b'<title>certificate-ok</title>'
        self.send_response(200)
        self.send_header('Content-Type','text/html')
        if self.path.endswith('-cache'): self.send_header('Cache-Control','public, max-age=3600')
        self.send_header('Content-Length',str(len(body)))
        self.end_headers()
        try: self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError): pass

    def do_POST(self):
        self.rfile.read(int(self.headers.get('Content-Length','0')))
        self.do_GET()

class Quiet(ThreadingHTTPServer):
    daemon_threads = True
    # A client that refuses the certificate is what these tests make.
    def handle_error(self, request, client_address): pass

_keys = None

def _key():
    """The key and certificate: made once per process, for every fixture."""
    global _keys
    if _keys is None:
        _keys = tempfile.TemporaryDirectory(prefix='zebkit-certificate-key-')
        atexit.register(_keys.cleanup)
        root = Path(_keys.name)
        subprocess.run(['openssl','req','-x509','-newkey','rsa:2048','-nodes','-days','1',
                        '-keyout',str(root/'key.pem'),'-out',str(root/'certificate.pem'),
                        '-subj','/CN=localhost','-addext','subjectAltName=DNS:localhost,IP:127.0.0.1'],
                       check=True,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
    return Path(_keys.name)

class TLSFixture:
    def __init__(self):
        self.work = tempfile.TemporaryDirectory(prefix='zebkit-certificate-test-')
        root = Path(self.work.name)
        for name in ('key.pem','certificate.pem'):
            shutil.copyfile(_key()/name, root/name)
        self.server = Quiet(('127.0.0.1',0),Server)
        self.received = self.server.received = []
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(root/'certificate.pem',root/'key.pem')
        # The handshake in the request's thread: a connection that never sends
        # its hello (Chromium opens some ahead) would hold every other one.
        self.server.socket = context.wrap_socket(self.server.socket,server_side=True,do_handshake_on_connect=False)
        self.thread = threading.Thread(target=self.server.serve_forever,daemon=True)
        self.thread.start()
        self.url = f'https://localhost:{self.server.server_port}'
    def close(self):
        self.server.shutdown();self.server.server_close();self.thread.join();self.work.cleanup()
    def __enter__(self): return self
    def __exit__(self,*args): self.close()
