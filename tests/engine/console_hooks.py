"""Document-console observations come from Chromium, including cross-site frames."""
import ctypes
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import sys
import threading
import time
import traceback
import zebkit

class Server(BaseHTTPRequestHandler):
    def log_message(self, *args): pass
    def do_GET(self):
        body = '<title>console hook</title>'
        if self.path == '/frame':
            body += f'<iframe src="http://localhost:{self.server.server_port}/child"></iframe>'
        if self.path == '/child': body += '<script>console.warn("cross-site child")</script>'
        data = body.encode()
        self.send_response(200)
        self.send_header('Content-Type', 'text/html; charset=utf-8')
        self.send_header('Content-Length', str(len(data)))
        self.end_headers()
        try: self.wfile.write(data)
        except (BrokenPipeError, ConnectionResetError): pass

server = ThreadingHTTPServer(('127.0.0.1', 0), Server)
thread = threading.Thread(target=server.serve_forever, daemon=True)
thread.start()
base = f'http://127.0.0.1:{server.server_port}'
EVENT = ctypes.CFUNCTYPE(None, zebkit._V, zebkit._S, zebkit._P)
on_console = zebkit._declare('zebkit_view_on_console', None, zebkit._V, EVENT, zebkit._P)
events, errors, callbacks = [], [], []

def wait(predicate, label):
    deadline = time.monotonic() + 15
    while not predicate() and time.monotonic() < deadline:
        assert not errors, errors
        assert zebkit.pump()
        time.sleep(.001)
    assert not errors, errors
    assert predicate(), label

def evaluate(view, code):
    result = []
    view.evaluate(code, lambda value, error: result.append((value, error)))
    wait(lambda: bool(result), code[:100])
    assert result[0][1] is None, result
    return result[0][0]

def load(view, path):
    view.load(base + path)
    wait(lambda: view.url == base + path and evaluate(view, 'document.readyState') == 'complete', path)

@EVENT
def event(handle, payload, data):
    try: events.append((handle, json.loads(payload)))
    except BaseException: errors.append(traceback.format_exc())
callbacks.append(event)
assert zebkit.init([sys.argv[0], *sys.argv[1:]], settings={'logging': {'destination': 'none'}}) == 0
try:
    first, other = zebkit.View(), zebkit.View()
    load(first, '/initial'); load(other, '/other')
    on_console(None, event, None)
    on_console(first._check(), event, None)
    worker = threading.Thread(target=lambda: on_console(first._handle, EVENT(), None))
    worker.start(); worker.join()
    evaluate(first, "console.log('中文😀\\u0000tail'); console.warn('warning-text'); console.error('error-text'); true\n//# sourceURL=native-console-test.js")
    wait(lambda: len(events) >= 3, 'three document messages')
    recorded = {e['message']: e for _, e in events}
    assert recorded['中文😀\0tail']['level'] == 'info'
    assert recorded['warning-text']['level'] == 'warning'
    assert recorded['error-text']['level'] == 'error'
    sample = recorded['warning-text']
    assert sample['origin'] == base and sample['mainFrame'] is True
    assert sample['frameId'].startswith('f') and sample['line'] >= 1
    assert sample['source'].endswith('native-console-test.js')
    assert sample['stackTrace'] is None or isinstance(sample['stackTrace'], str)
    assert not sample['truncated'] and int(sample['dropped']) >= 0
    count = len(events)
    evaluate(other, "console.warn('other-view'); true")
    evaluate(first, 'true')
    assert len(events) == count
    evaluate(first, "console.log('x'.repeat(65535)+'😀tail'); true")
    wait(lambda: any(e['truncated'] for _, e in events), 'bounded Unicode message')
    large = next(e for _, e in events if e['truncated'])
    assert large['message'] == 'x' * 65535
    load(first, '/frame')
    wait(lambda: any(e['message'] == 'cross-site child' for _, e in events), 'OOPIF console event')
    child = next(e for _, e in events if e['message'] == 'cross-site child')
    assert not child['mainFrame'] and child['origin'] == base.replace('127.0.0.1', 'localhost')
    assert child['frameId'] != sample['frameId']
    swapped = []
    @EVENT
    def replacement(handle, payload, data):
        try: swapped.append(json.loads(payload))
        except BaseException: errors.append(traceback.format_exc())
    @EVENT
    def swap(handle, payload, data):
        on_console(handle, replacement, None)
    callbacks.extend([replacement, swap])
    on_console(first._check(), swap, None)
    evaluate(first, "console.log('swap-now'); true")
    evaluate(first, "console.log('after-swap'); true")
    wait(lambda: bool(swapped), 'replacement observer')
    assert [e['message'] for e in swapped] == ['after-swap']
    on_console(first._check(), EVENT(), None)
    evaluate(first, "console.log('removed'); true")
    assert [e['message'] for e in swapped] == ['after-swap']
    @EVENT
    def close_in_event(handle, payload, data): first.close()
    callbacks.append(close_in_event)
    on_console(first._check(), close_in_event, None)
    first.evaluate("console.log('close-now'); true")
    wait(lambda: first._closed, 'close from observer')
    other.close()
    print('ok native console: levels, Unicode/NUL, bounds, OOPIF, view/thread isolation, replacement and closing', flush=True)
finally:
    assert zebkit.shutdown()
    server.shutdown(); server.server_close(); thread.join()
assert not errors, errors
