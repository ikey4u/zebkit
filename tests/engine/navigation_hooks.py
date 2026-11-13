"""Real navigations exercise native asynchronous policy, without CDP interception:
View.on_navigation()'s stages (start, redirect, response, failure, commit),
each continued, cancelled or redirected; duplicate and truncated headers, POST,
a frame of another site; invalid, late, stale and other threads' replies; the
deadline, failing closed, a new handler, the view closed; window.open() asking
the opener's policy.

    python3 tests/engine/navigation_hooks.py [engine switches...]

Exit 0 when every check passes. The C API's own cases (a NULL request, a zero
deadline) are tests/engine/hooks.c's.
"""
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
import socket
import sys
import threading
import time
from urllib.parse import urlsplit

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from harness import check, ev, finish, must_wait, part, pump, zebkit  # noqa: E402

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
requests, outcomes = [], []
mode = 'continue'
late = []


def wait(condition, label='navigation condition', timeout=15):
    must_wait(condition, label, timeout)


def evaluate(view, code):
    """ev(), raising when the script threw or did not answer."""
    value = ev(view, code)
    assert not (isinstance(value, tuple) and value[:1] in (('error',), ('timeout',))), (code, value)
    return value


def respond(request, action='continue', url=None, valid=True):
    """request.respond(); that it was taken (valid) or refused."""
    ok = request.respond(action, url)
    assert ok == valid, (request.metadata['url'], action, url, ok)
    return ok


def outcome(request):
    return next((event for event in outcomes if event['id'] == request.metadata['id']), None)


def install(view):
    def asked(request):
        event = request.metadata
        assert event['state'] == 'pending', event
        assert request.info == event, (request.info, event)
        requests.append((request, event))
        if mode == 'hold':
            return
        if mode == 'late':
            time.sleep(.06)
            late.append(request.respond())
            return
        if mode == 'cancel-start' and event['phase'] == 'start':
            respond(request, 'cancel')
        elif mode == 'cancel-redirect' and event['phase'] == 'redirect':
            respond(request, 'cancel')
        elif mode == 'cancel-response' and event['phase'] == 'response':
            respond(request, 'cancel')
        elif mode == 'cancel-failure' and event['phase'] == 'failure':
            respond(request, 'cancel')
        elif mode == 'replace-start' and event['phase'] == 'start' and '/replace?' in event['url']:
            respond(request, 'redirect', base + '/replacement')
        elif mode == 'replace-child' and not event['mainFrame'] and event['phase'] == 'start' and event['url'].endswith('/child'):
            respond(request, 'redirect', base.replace('127.0.0.1', 'localhost') + '/child-replaced')
        else:
            respond(request)
        assert request.info is None, request.info
        respond(request, valid=False)
    view.on_navigation(asked)


def load(view, path):
    view.load(base + path)
    wait(lambda: view.url == base + path and evaluate(view, 'document.readyState') == 'complete', path)


if zebkit.init([sys.argv[0], *sys.argv[1:]], settings={'logging': {'destination': 'none'}}) != 0:
    print('FAIL zebkit.init()')
    sys.exit(1)
try:
    first, other = zebkit.View(), zebkit.View()
    closed = []
    first.on_close(lambda: closed.append(True))
    with part('the views load before any policy', fatal=True):
        load(first, '/initial')
        load(other, '/other')
    first.on_navigation_result(lambda event: outcomes.append(event))
    install(first)
    with part('a navigation\'s stages: start and response, one navigation of the main frame, duplicate headers'):
        load(first, '/headers')
        events = [event for _, event in requests]
        assert [event['phase'] for event in events] == ['start', 'response'], events
        assert len({event['navigationId'] for event in events}) == len({event['frameId'] for event in events}) == 1
        assert all(event['mainFrame'] and event['method'] == 'GET' for event in events)
        response = events[-1]
        assert response['responseStatus'] == 200 and not response['headersTruncated']
        assert [h['value'] for h in response['responseHeaders'] if h['name'].lower() == 'x-policy-test'] == ['one', 'two']
    with part('another view\'s navigation is not this view\'s policy\'s'):
        count = len(requests)
        load(other, '/independent')
        assert len(requests) == count
    with part('a real redirect: start, redirect, response, with its chain'):
        before = len(requests)
        first.load(base + '/redirect')
        wait(lambda: first.url == base + '/final?redirect')
        events = [event for _, event in requests[before:]]
        assert [event['phase'] for event in events] == ['start', 'redirect', 'response'], events
        assert events[1]['url'] == base + '/final?redirect' and events[1]['responseStatus'] == 302
        assert events[-1]['redirectChain'] == [base + '/redirect', base + '/final?redirect']
    with part('cancelled at its start: nothing asked of the server'):
        mode = 'cancel-start'
        before = len(requests)
        first.load(base + '/blocked-start')
        wait(lambda: len(requests) > before and outcome(requests[-1][0]) is not None)
        assert first.url == base + '/final?redirect' and ('GET', '/blocked-start') not in received
    with part('cancelled at the redirect: its target not asked'):
        mode = 'cancel-redirect'
        before_targets = received.count(('GET', '/final?redirect'))
        before = len(requests)
        first.load(base + '/redirect')
        wait(lambda: len(requests) >= before + 2 and outcome(requests[-1][0]) is not None)
        assert requests[-1][1]['phase'] == 'redirect' and first.url == base + '/final?redirect'
        assert received.count(('GET', '/final?redirect')) == before_targets
    with part('cancelled at the response: not committed'):
        mode = 'cancel-response'
        before = len(requests)
        first.load(base + '/blocked-response')
        wait(lambda: len(requests) >= before + 2 and outcome(requests[-1][0]) is not None)
        assert ('GET', '/blocked-response') in received and first.url == base + '/final?redirect'
    with part('redirected at its start: the replacement loaded instead'):
        mode = 'replace-start'
        first.load(base + '/replace?first')
        wait(lambda: first.url == base + '/replacement')
        assert ('GET', '/replace?first') not in received
    with part('more headers than are told: 256, marked truncated'):
        mode = 'continue'
        load(first, '/many-headers')
        assert requests[-1][1]['headersTruncated'] and len(requests[-1][1]['responseHeaders']) == 256
    with part('a frame of another site asks the view\'s policy, and is redirected'):
        mode = 'replace-child'
        load(first, '/frame')
        wait(lambda: evaluate(first, 'childURL') == base.replace('127.0.0.1', 'localhost') + '/child-replaced')
        assert first.url == base + '/frame'
        assert any(not event['mainFrame'] and event['url'].endswith('/child-replaced') for _, event in requests)
    with part('a form\'s POST, told as POST'):
        mode = 'continue'
        load(first, '/form')
        evaluate(first, 'document.forms[0].submit(); true')
        wait(lambda: first.url == base + '/post')
        assert ('POST', '/post', 'value=payload') in received
        assert requests[-1][1]['method'] == 'POST'
    with part('a failure asked about, with its network error'):
        mode = 'cancel-failure'
        before = len(requests)
        with socket.socket() as unused:
            unused.bind(('127.0.0.1', 0))
            first.load(f'http://127.0.0.1:{unused.getsockname()[1]}/failure')
            wait(lambda: any(event['phase'] == 'failure' for _, event in requests[before:]))
        assert requests[-1][1]['netError'] != 0 and first.url == base + '/post'
    with part('a held request refuses invalid replies, and stays held', fatal=True):
        mode = 'hold'
        before = len(requests)
        first.load(base + '/held')
        wait(lambda: len(requests) > before)
        held = requests[-1][0]
        try:
            held.respond('nonsense')
            raise AssertionError('an unknown action accepted')
        except ValueError:
            pass
        respond(held, 'redirect', 'not a URL', valid=False)
        respond(held, 'continue', base + '/unexpected', valid=False)
        assert held.info is not None
    with part('a reply from another thread is refused, and the request is not seen there'):
        worker_results = []
        worker = threading.Thread(target=lambda: worker_results.append((held.respond(), held.info)))
        worker.start(); worker.join(timeout=3)
        assert not worker.is_alive() and worker_results == [(False, None)], worker_results
        assert held.info is not None
    with part('a new function settles the held request: handler-changed'):
        mode = 'continue'
        install(first)
        wait(lambda: first.url == base + '/held')
        wait(lambda: outcome(held) is not None)
        assert outcome(held)['reason'] == 'handler-changed' and held.info is None
    with part('the deadline: an unanswered request continues at it, told as timeout'):
        mode = 'hold'
        first.set_navigation_timeout(40)
        before = len(requests)
        first.load(base + '/timeout')
        wait(lambda: len(requests) > before)
        timed = requests[-1][0]
        wait(lambda: first.url == base + '/timeout')
        wait(lambda: outcome(timed) is not None)
        assert outcome(timed)['reason'] == 'timeout' and timed.info is None
        respond(timed, valid=False)
    with part('a reply after the deadline, from a callback that took longer, is refused'):
        mode = 'late'
        first.set_navigation_timeout(20)
        first.load(base + '/blocked-host-callback')
        wait(lambda: first.url == base + '/blocked-host-callback')
        assert late and all(value is False for value in late), late
    with part('a navigation superseded by a newer one: its request settled'):
        mode = 'hold'
        first.set_navigation_timeout(5000)
        before = len(requests)
        first.load(base + '/superseded')
        wait(lambda: len(requests) > before)
        old = requests[-1][0]
        first.load(base + '/newer')
        wait(lambda: len(requests) > before + 1)
        new = requests[-1][0]
        assert old.metadata['id'] != new.metadata['id'] and old.info is None and new.info is not None
        respond(old, valid=False)
    with part('no policy (on_navigation(None)) continues the pending request'):
        first.on_navigation(None)
        wait(lambda: first.url == base + '/newer')
        assert new.info is None
    with part('about:blank asked at its commit'):
        mode = 'continue'
        install(first)
        before = len(requests)
        first.load('about:blank')
        wait(lambda: first.url == 'about:blank')
        assert len(requests) > before and requests[-1][1]['phase'] == 'commit'
    with part('set_navigation_timeout() refuses what is not 1 to 300000 ms'):
        first.set_navigation_timeout(5000)
        for bad in (0, -1, 300001, True, 1.5):
            try:
                first.set_navigation_timeout(bad)
                raise AssertionError('invalid public navigation timeout accepted: %r' % (bad,))
            except ValueError:
                pass
    with part('a function that redirects and removes itself; its result told, and the observer removed'):
        public_requests, public_results = [], []

        def public_asked(request):
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

        def public_completed(event):
            public_results.append(event)
            first.on_navigation_result(None)
        first.on_navigation(public_asked)
        first.on_navigation_result(public_completed)
        first.load(base + '/python-original')
        wait(lambda: first.url == base + '/python-binding' and bool(public_results), 'public Python navigation bindings')
        assert len(public_requests) == 1 and public_results[0]['id'] == public_requests[0].metadata['id']
        assert public_results[0]['state'] == 'redirected' and public_results[0]['replacementUrl'] == base + '/python-binding'
        assert ('GET', '/python-original') not in received
    first.on_navigation_result(lambda event: outcomes.append(event))
    install(first)
    # window.open() does not escape the opener's policy: the popup has no handler
    # of its own and asks the opener's, with no window-open handler anywhere.
    with part('window.open() asks the opener\'s policy, and is cancelled there'):
        first.set_popup_policy('allow')
        mode = 'cancel-start'
        before = len(requests)
        evaluate(first, f"void window.open({json.dumps(base + '/popup-blocked')})")
        wait(lambda: any('/popup-blocked' in event['url'] for _, event in requests[before:]), 'the popup is asked about')
        assert not any('/popup-blocked' in entry[1] for entry in received), 'the popup escaped the policy'

    # An unanswered navigation continues by default, and is cancelled when asked.
    with part('failing closed: an unanswered navigation is cancelled at its deadline'):
        first.set_navigation_timeout(100)
        first.set_navigation_fail_closed(True)
        mode = 'hold'
        first.load(base + '/fail-closed')
        wait(lambda: any('/fail-closed' in event['url'] for _, event in requests), 'the fail-closed stage')
        unanswered = next(request for request, event in requests if '/fail-closed' in event['url'])
        # Settled by its 100 ms deadline; fail closed, it went nowhere.
        wait(lambda: unanswered.info is None, 'the unanswered navigation reaches its deadline')
        # A request it made would take a moment to reach the server: the engine pumped meanwhile.
        pump()
        assert not any('/fail-closed' in entry[1] for entry in received), 'an unanswered navigation went on'
        first.set_navigation_fail_closed(False)
        first.set_navigation_timeout(5000)
    with part('the view closed settles its held request'):
        mode = 'hold'
        before = len(requests)
        first.load(base + '/close-held')
        wait(lambda: len(requests) > before)
        last = requests[-1][0]
        first.close()
        wait(lambda: closed, 'the view closed')
        assert last.info is None
        respond(last, valid=False)
    other.close()
finally:
    check(zebkit.shutdown(), 'zebkit.shutdown()')
    server.shutdown(); server.server_close(); server_thread.join()
sys.exit(finish())
