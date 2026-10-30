"""Real navigations exercise native asynchronous policy, without CDP interception."""
import ctypes
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import socket
import sys
import threading
import time
import traceback
from urllib.parse import urlsplit
import zebkit

received = []
class Server(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass
    def do_GET(self):
        received.append(('GET', self.path))
        path = urlsplit(self.path).path
        if path == '/redirect':
            self.send_response(302)
            self.send_header('Location', '/final?redirect')
            self.send_header('X-Policy-Test', 'redirect')
            self.send_header('Content-Length', '0')
            self.end_headers()
            return
        title = path.lstrip('/') or 'home'
        body = '<title>' + title + '</title>'
        if path == '/frame':
            body += '<script>window.childURL=null;onmessage=e=>{childURL=e.data}</script>'
            body += f'<iframe src="http://localhost:{self.server.server_port}/child"></iframe>'
        if path.startswith('/child'):
            body += '<script>parent.postMessage(location.href,"*")</script>'
        if path == '/form':
            body += '<form method="post" action="/post"><input name="value" value="payload"></form>'
        data = body.encode()
        self.send_response(200)
        self.send_header('Content-Type', 'text/html')
        self.send_header('Cache-Control', 'no-store')
        self.send_header('X-Policy-Test', 'one')
        self.send_header('X-Policy-Test', 'two')
        if path == '/many-headers':
            for index in range(300):
                self.send_header('X-Extra-' + str(index), 'value')
        self.send_header('Content-Length', str(len(data)))
        self.end_headers()
        try:
            self.wfile.write(data)
        except (BrokenPipeError, ConnectionResetError):
            pass
    def do_POST(self):
        body = self.rfile.read(int(self.headers.get('Content-Length', 0)))
        received.append(('POST', self.path, body.decode()))
        data = b'<title>posted</title>'
        self.send_response(200)
        self.send_header('Content-Type', 'text/html')
        self.send_header('Content-Length', str(len(data)))
        self.end_headers()
        self.wfile.write(data)

server = ThreadingHTTPServer(('127.0.0.1', 0), Server)
server_thread = threading.Thread(target=server.serve_forever, daemon=True)
server_thread.start()
base = f'http://127.0.0.1:{server.server_port}'
P, V, S = zebkit._P, zebkit._V, zebkit._S
NAV = ctypes.CFUNCTYPE(None, V, P, S, P)
RESULT = ctypes.CFUNCTYPE(None, V, S, P)
on_navigation = zebkit._declare('zebkit_view_on_navigation', None, V, NAV, P)
on_result = zebkit._declare('zebkit_view_on_navigation_result', None, V, RESULT, P)
set_timeout = zebkit._declare('zebkit_view_set_navigation_timeout', ctypes.c_bool, V, ctypes.c_uint32)
request_info = zebkit._declare('zebkit_navigation_request_info', P, P)
reply = zebkit._declare('zebkit_navigation_respond', ctypes.c_bool, P, ctypes.c_int, S, ctypes.POINTER(P))
CONTINUE, CANCEL, REDIRECT = 0, 1, 2
requests, outcomes, errors, callbacks = [], [], [], []
mode = 'continue'
late = []


def wait(condition, label='navigation condition', timeout=15):
    until = time.monotonic() + timeout
    while not condition() and time.monotonic() < until:
        assert not errors, errors
        assert zebkit.pump()
        time.sleep(.001)
    assert not errors, errors
    assert condition(), label


def evaluate(view, code):
    answers = []
    view.evaluate(code, lambda value, error: answers.append((value, error)))
    wait(lambda: bool(answers), code)
    assert answers[0][1] is None, answers
    return answers[0][0]


def info(token):
    value = request_info(token)
    return json.loads(zebkit._string(value)) if value else None


def respond(token, action=CONTINUE, url=None, valid=True):
    error = P()
    ok = bool(reply(token, action, url.encode() if url else None, ctypes.byref(error)))
    message = zebkit._string(error.value)
    assert ok == valid and (not message if valid else bool(message)), (ok, message)
    return ok


def outcome(token):
    return next((event for event in outcomes if event['id'] == str(token)), None)


@RESULT
def completed(view, payload, data):
    try:
        outcomes.append(json.loads(payload))
    except BaseException:
        errors.append(traceback.format_exc())
callbacks.append(completed)


def install(view):
    @NAV
    def asked(handle, token, payload, data):
        try:
            event = json.loads(payload)
            assert event['id'] == str(token) and event['state'] == 'pending'
            assert info(token) == event
            requests.append((token, event))
            if mode == 'hold':
                return
            if mode == 'late':
                time.sleep(.06)
                late.append(respond(token, valid=False))
                return
            if mode == 'cancel-start' and event['phase'] == 'start':
                respond(token, CANCEL)
            elif mode == 'cancel-redirect' and event['phase'] == 'redirect':
                respond(token, CANCEL)
            elif mode == 'cancel-response' and event['phase'] == 'response':
                respond(token, CANCEL)
            elif mode == 'cancel-failure' and event['phase'] == 'failure':
                respond(token, CANCEL)
            elif mode == 'replace-start' and event['phase'] == 'start' and '/replace?' in event['url']:
                respond(token, REDIRECT, base + '/replacement')
            elif mode == 'replace-child' and not event['mainFrame'] and event['phase'] == 'start' and event['url'].endswith('/child'):
                respond(token, REDIRECT, base.replace('127.0.0.1', 'localhost') + '/child-replaced')
            else:
                respond(token)
            assert info(token) is None
            respond(token, valid=False)
        except BaseException:
            errors.append(traceback.format_exc())
    callbacks.append(asked)
    on_navigation(view._check(), asked, None)


def load(view, path):
    view.load(base + path)
    wait(lambda: view.url == base + path and evaluate(view, 'document.readyState') == 'complete', path)


assert zebkit.init([sys.argv[0], *sys.argv[1:]], settings={'logging': {'destination': 'none'}}) == 0
try:
    first, other = zebkit.View(), zebkit.View()
    load(first, '/initial')
    load(other, '/other')
    on_result(first._check(), completed, None)
    install(first)
    load(first, '/headers')
    events = [event for _, event in requests]
    assert [event['phase'] for event in events] == ['start', 'response'], events
    assert len({event['navigationId'] for event in events}) == len({event['frameId'] for event in events}) == 1
    assert all(event['mainFrame'] and event['method'] == 'GET' for event in events)
    response = events[-1]
    assert response['responseStatus'] == 200 and not response['headersTruncated']
    assert [h['value'] for h in response['responseHeaders'] if h['name'].lower() == 'x-policy-test'] == ['one', 'two']
    count = len(requests)
    load(other, '/independent')
    assert len(requests) == count
    before = len(requests)
    first.load(base + '/redirect')
    wait(lambda: first.url == base + '/final?redirect')
    events = [event for _, event in requests[before:]]
    assert [event['phase'] for event in events] == ['start', 'redirect', 'response'], events
    assert events[1]['url'] == base + '/final?redirect' and events[1]['responseStatus'] == 302
    assert events[-1]['redirectChain'] == [base + '/redirect', base + '/final?redirect']
    mode = 'cancel-start'
    before = len(requests)
    first.load(base + '/blocked-start')
    wait(lambda: len(requests) > before and outcome(requests[-1][0]) is not None)
    assert first.url == base + '/final?redirect' and ('GET', '/blocked-start') not in received
    mode = 'cancel-redirect'
    before_targets = received.count(('GET', '/final?redirect'))
    before = len(requests)
    first.load(base + '/redirect')
    wait(lambda: len(requests) >= before + 2 and outcome(requests[-1][0]) is not None)
    assert requests[-1][1]['phase'] == 'redirect' and first.url == base + '/final?redirect'
    assert received.count(('GET', '/final?redirect')) == before_targets
    mode = 'cancel-response'
    before = len(requests)
    first.load(base + '/blocked-response')
    wait(lambda: len(requests) >= before + 2 and outcome(requests[-1][0]) is not None)
    assert ('GET', '/blocked-response') in received and first.url == base + '/final?redirect'
    mode = 'replace-start'
    first.load(base + '/replace?first')
    wait(lambda: first.url == base + '/replacement')
    assert ('GET', '/replace?first') not in received
    mode = 'continue'
    load(first, '/many-headers')
    assert requests[-1][1]['headersTruncated'] and len(requests[-1][1]['responseHeaders']) == 256
    mode = 'replace-child'
    load(first, '/frame')
    wait(lambda: evaluate(first, 'childURL') == base.replace('127.0.0.1', 'localhost') + '/child-replaced')
    assert first.url == base + '/frame'
    assert any(not event['mainFrame'] and event['url'].endswith('/child-replaced') for _, event in requests)
    mode = 'continue'
    load(first, '/form')
    evaluate(first, 'document.forms[0].submit(); true')
    wait(lambda: first.url == base + '/post')
    assert ('POST', '/post', 'value=payload') in received
    assert requests[-1][1]['method'] == 'POST'
    mode = 'cancel-failure'
    before = len(requests)
    with socket.socket() as unused:
        unused.bind(('127.0.0.1', 0))
        first.load(f'http://127.0.0.1:{unused.getsockname()[1]}/failure')
        wait(lambda: any(event['phase'] == 'failure' for _, event in requests[before:]))
    assert requests[-1][1]['netError'] != 0 and first.url == base + '/post'
    mode = 'hold'
    before = len(requests)
    first.load(base + '/held')
    wait(lambda: len(requests) > before)
    token = requests[-1][0]
    respond(token, 99, valid=False)
    respond(token, REDIRECT, 'not a URL', valid=False)
    respond(token, CONTINUE, base + '/unexpected', valid=False)
    assert info(token) is not None
    worker_results = []
    worker = threading.Thread(target=lambda: worker_results.append((bool(reply(token, CONTINUE, None, None)), info(token))))
    worker.start(); worker.join(timeout=3)
    assert not worker.is_alive() and worker_results == [(False, None)]
    mode = 'continue'
    install(first)
    wait(lambda: first.url == base + '/held')
    wait(lambda: outcome(token) is not None)
    assert outcome(token)['reason'] == 'handler-changed' and info(token) is None
    mode = 'hold'
    assert not set_timeout(first._check(), 0) and not set_timeout(first._check(), 300001)
    assert set_timeout(first._check(), 40)
    before = len(requests)
    first.load(base + '/timeout')
    wait(lambda: len(requests) > before)
    token = requests[-1][0]
    wait(lambda: first.url == base + '/timeout')
    wait(lambda: outcome(token) is not None)
    assert outcome(token)['reason'] == 'timeout' and info(token) is None
    respond(token, valid=False)
    mode = 'late'
    assert set_timeout(first._check(), 20)
    first.load(base + '/blocked-host-callback')
    wait(lambda: first.url == base + '/blocked-host-callback')
    assert late and all(value is False for value in late)
    mode = 'hold'
    assert set_timeout(first._check(), 5000)
    before = len(requests)
    first.load(base + '/superseded')
    wait(lambda: len(requests) > before)
    old = requests[-1][0]
    first.load(base + '/newer')
    wait(lambda: len(requests) > before + 1)
    new = requests[-1][0]
    assert old != new and info(old) is None and info(new) is not None
    respond(old, valid=False)
    on_navigation(first._check(), NAV(), None)
    wait(lambda: first.url == base + '/newer')
    assert info(new) is None
    mode = 'continue'
    install(first)
    before = len(requests)
    first.load('about:blank')
    wait(lambda: first.url == 'about:blank')
    assert len(requests) > before and requests[-1][1]['phase'] == 'commit'
    public_requests, public_results = [], []
    first.set_navigation_timeout(5000)
    for bad in (0, -1, 300001, True, 1.5):
        try:
            first.set_navigation_timeout(bad)
            raise AssertionError('invalid public navigation timeout accepted')
        except ValueError:
            pass
    def public_asked(request):
        try:
            public_requests.append(request)
            assert request.info == request.metadata
            assert not request.respond('redirect', 'not a URL') and request.info is not None
            try:
                request.respond('redirect', base + '/bad\0url')
                raise AssertionError('NUL replacement accepted')
            except ValueError:
                pass
            assert request.respond('redirect', base + '/python-binding')
            assert request.info is None and not request.respond()
            first.on_navigation(None)
        except BaseException:
            errors.append(traceback.format_exc())
    def public_completed(event):
        public_results.append(event)
        first.on_navigation_result(None)
    first.on_navigation(public_asked)
    first.on_navigation_result(public_completed)
    first.load(base + '/python-original')
    wait(lambda: first.url == base + '/python-binding' and bool(public_results), 'public Python navigation bindings')
    assert len(public_requests) == 1 and public_results[0]['id'] == public_requests[0].metadata['id']
    assert public_results[0]['action'] == 'redirect' and public_results[0]['replacementUrl'] == base + '/python-binding'
    assert ('GET', '/python-original') not in received
    install(first)
    # window.open() does not escape the opener's policy: the popup has no handler
    # of its own and asks the opener's, with no window-open handler anywhere.
    first.set_popup_policy('allow')
    mode = 'cancel-start'
    before = len(requests)
    evaluate(first, f"void window.open({json.dumps(base + '/popup-blocked')})")
    wait(lambda: any('/popup-blocked' in event['url'] for _, event in requests[before:]), 'the popup is asked about')
    assert not any('/popup-blocked' in entry[1] for entry in received), 'the popup escaped the policy'

    # An unanswered navigation continues by default, and is cancelled when asked.
    first.set_navigation_timeout(100)
    first.set_navigation_fail_closed(True)
    mode = 'hold'
    first.load(base + '/fail-closed')
    wait(lambda: any('/fail-closed' in event['url'] for _, event in requests), 'the fail-closed stage')
    time.sleep(.5)
    assert not any('/fail-closed' in entry[1] for entry in received), 'an unanswered navigation went on'
    first.set_navigation_fail_closed(False)
    first.set_navigation_timeout(5000)

    mode = 'hold'
    before = len(requests)
    first.load(base + '/close-held')
    wait(lambda: len(requests) > before)
    token = requests[-1][0]
    first.close()
    wait(lambda: first._closed)
    assert info(token) is None
    respond(token, valid=False)
    print('ok native navigation hooks: stages, real redirects, cancellation, replacement, duplicate headers, POST, OOPIF, deadlines, lifecycle and isolation', flush=True)
finally:
    assert zebkit.shutdown()
    server.shutdown(); server.server_close(); server_thread.join()
assert not errors, errors
