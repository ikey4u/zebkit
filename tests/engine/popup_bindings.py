"""Public Python wrappers preserve the actual popup and its close lifecycle."""
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import sys
import threading
import time
import zebkit

class Server(BaseHTTPRequestHandler):
    def log_message(self, *args): pass
    def do_GET(self):
        body = b'<title>popup binding</title>'
        self.send_response(200)
        self.send_header('Content-Type', 'text/html')
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)

server = ThreadingHTTPServer(('127.0.0.1', 0), Server)
thread = threading.Thread(target=server.serve_forever, daemon=True)
thread.start()
base = f'http://127.0.0.1:{server.server_port}'

def wait(predicate, label):
    deadline = time.monotonic() + 15
    while not predicate() and time.monotonic() < deadline:
        assert zebkit.pump()
        time.sleep(.001)
    assert predicate(), label

def evaluate(view, script):
    result = []
    view.evaluate(script, lambda value, error: result.append((value, error)))
    wait(lambda: bool(result), script)
    assert result[0][1] is None, result
    return result[0][0]

assert zebkit.init([sys.argv[0], *sys.argv[1:]], settings={'logging': {'destination': 'none'}}) == 0
try:
    parent = zebkit.View(base)
    wait(lambda: parent.url == base + '/', 'parent navigation')
    assert parent.window_open_info == {}
    parent.set_popup_policy('allow')
    children, events = [], []
    def opened(child, url):
        assert child.window_open_info['url'] == url
        children.append(child)
        # Removing a ctypes trampoline while it is executing must be safe.
        parent.on_window_open(None)
    def event(metadata):
        events.append(metadata)
        if metadata['phase'] == 'created': parent.on_window_open_event(None)
    parent.on_window_open(opened)
    parent.on_window_open_event(event)
    assert evaluate(parent, "window.child=window.open('/child','child','left=0,top=0,width=320,height=240'); !!child") is True
    wait(lambda: bool(children) and any(e['phase'] == 'created' for e in events), 'public popup callbacks')
    child = children[0]
    wait(lambda: child.url == base + '/child', 'child navigation')
    assert evaluate(child, 'window.opener !== null') is True
    assert evaluate(child, "window.opener.fromChild='delivered'; true") is True
    assert evaluate(parent, 'window.fromChild') == 'delivered'
    snapshot = child.window_open_info
    assert snapshot['targetId'] == child.target_id()
    child.load(base + '/moved')
    wait(lambda: child.url == base + '/moved', 'child navigation after adoption')
    assert child.window_open_info == snapshot
    closed = []
    child.on_close(lambda: closed.append(True))
    child.close()
    wait(lambda: bool(closed), 'adopted child closes')
    assert evaluate(parent, 'window.child.closed') is True
    try: child.window_open_info
    except zebkit.Error: pass
    else: raise AssertionError('closed child metadata must reject access')
    parent.close()
    print('ok public Python popup: native adoption, creation snapshot, opener messaging and callback cleanup', flush=True)
finally:
    assert zebkit.shutdown()
    server.shutdown(); server.server_close(); thread.join()
