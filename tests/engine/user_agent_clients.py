"""Native and external CDP overrides on a live view must retain their scope."""
from concurrent.futures import ThreadPoolExecutor
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import tempfile
import threading
import time
import zebkit
from cdp_page import Cdp

received = []
class Server(BaseHTTPRequestHandler):
    def log_message(self, *args): pass
    def do_GET(self):
        received.append((self.path, self.headers.get('User-Agent')))
        body = b'<title>ua-client</title><script>window.firstUserAgent=navigator.userAgent</script>'
        self.send_response(200)
        self.send_header('Content-Type', 'text/html')
        self.send_header('Content-Length', str(len(body)))
        self.send_header('Cache-Control', 'no-store')
        self.end_headers()
        try: self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError): pass

def wait(predicate, label):
    deadline = time.monotonic()+20
    while time.monotonic() < deadline:
        assert zebkit.pump()
        if predicate(): return
        time.sleep(.002)
    raise AssertionError(label)

server = ThreadingHTTPServer(('127.0.0.1',0), Server)
thread = threading.Thread(target=lambda:server.serve_forever(poll_interval=.02), daemon=True)
thread.start()
base = f'http://127.0.0.1:{server.server_port}'
with tempfile.TemporaryDirectory(prefix='zebkit-ua-clients-') as profile, ThreadPoolExecutor(max_workers=1) as pool:
    assert zebkit.init(['ua-clients','--no-sandbox','--headless','--disable-gpu',
                        '--no-proxy-server',f'--user-data-dir={profile}']) == 0
    cdp = None
    try:
        assert zebkit.cdp_listen('127.0.0.1',0)
        view = zebkit.View(None,320,200)
        native = zebkit._declare('zebkit_view_set_user_agent',None,zebkit._V,zebkit._S)
        native(view._check(),b'zebkit-native-before/1')
        view.load(base+'/initial')
        wait(lambda: any(path=='/initial' for path,_ in received) and view.title=='ua-client','initial load')
        def external(function):
            future = pool.submit(function)
            wait(future.done,'external CDP command')
            return future.result()
        endpoint = zebkit.cdp_url()
        target = view.target_id()
        cdp = external(lambda:Cdp(endpoint))
        session = external(lambda:cdp.call('Target.attachToTarget',{'targetId':target,'flatten':True}))['sessionId']
        def override(value):
            return external(lambda:cdp.call('Emulation.setUserAgentOverride',{'userAgent':value},session=session))
        def snapshot():
            answers = []
            view.evaluate('({current:navigator.userAgent,first:window.firstUserAgent})',
                          lambda value,error:answers.append((value,error)))
            wait(lambda:bool(answers),'native snapshot')
            assert answers[0][1] is None,answers
            return answers[0][0]
        def navigate(path, expected):
            view.load(base+path)
            wait(lambda:any(p==path for p,_ in received),'request '+path)
            assert next(agent for p,agent in received if p==path)==expected,received
            wait(lambda:snapshot().get('first')==expected,'first script '+path)
            assert snapshot()['current']==expected,snapshot()
        override('zebkit-external/2')
        assert snapshot()['current']=='zebkit-external/2'
        navigate('/external','zebkit-external/2')
        native(view._check(),b'zebkit-native-after/3')
        # Chromium keeps overrides in each attached session. A later native
        # command does not remove the external session's active override.
        navigate('/native-while-external','zebkit-external/2')
        external(lambda:cdp.call('Target.detachFromTarget',{'sessionId':session}))
        navigate('/detached','zebkit-native-after/3')
        print('ok UA native/external session scope, first-script headers and detach',flush=True)
        view.close()
    finally:
        if cdp: cdp.sock.close()
        zebkit.cdp_stop()
        zebkit.shutdown()
        server.shutdown(); server.server_close(); thread.join()
