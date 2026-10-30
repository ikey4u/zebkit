"""Local HTTP/proxy challenges; collected evidence never stores credentials."""
import base64
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import itertools
import threading
from urllib.parse import urlsplit

sequence = itertools.count(1)

class Server(BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'
    def log_message(self, *args): pass
    def do_GET(self):
        fixture = self.server.fixture
        path = urlsplit(self.path).path
        header = self.headers.get('Proxy-Authorization' if fixture.proxy else 'Authorization')
        expected = 'Basic '+base64.b64encode((fixture.username+':'+fixture.password).encode()).decode()
        valid = header == expected
        fixture.received.append({'path':path,'provided':header is not None,'accepted':valid})
        public = not fixture.proxy and path.startswith('/public')
        if public:
            source = '<title>public</title><script>window.childAuthenticated=false;onmessage=e=>{if(e.data==="http-auth-ok")childAuthenticated=true}</script>'
            if path == '/public-frame': source += '<iframe src="/auth/frame"></iframe>'
        else:
            source = '<title>auth-ok</title><script>if(parent!==self)parent.postMessage("http-auth-ok","*")</script>' if valid else '<title>auth-denied</title>'
        body = source.encode()
        self.send_response(200 if public or valid else (407 if fixture.proxy else 401))
        if not public and not valid:
            self.send_header('Proxy-Authenticate' if fixture.proxy else 'WWW-Authenticate',
                             'Basic realm="'+fixture.realm+'", charset="UTF-8"')
        self.send_header('Content-Type','text/html')
        self.send_header('Cache-Control','no-store')
        self.send_header('Content-Length',str(len(body)))
        self.end_headers()
        try: self.wfile.write(body)
        except (BrokenPipeError,ConnectionResetError): pass

class AuthFixture:
    def __init__(self, proxy=False):
        case = next(sequence)
        self.proxy = proxy
        self.realm = 'zebkit-realm-'+str(case)
        self.username = '原生-user-'+str(case)
        self.password = 'päss-🔐-'+str(case)
        self.received = []
        self.server = ThreadingHTTPServer(('127.0.0.1',0),Server)
        self.server.fixture = self
        self.thread = threading.Thread(target=lambda:self.server.serve_forever(poll_interval=.02),daemon=True)
        self.thread.start()
        self.url = f'http://127.0.0.1:{self.server.server_port}'
    def close(self):
        self.server.shutdown();self.server.server_close();self.thread.join()
    def __enter__(self): return self
    def __exit__(self,*args): self.close()
