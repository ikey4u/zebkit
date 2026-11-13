"""The headers the network adds, through <zebkit/hooks.h>'s stages inside the network:
sendHeaders, sent and headersReceived, against what a server receives, what the engine's
own CDP says (Network.requestWillBeSentExtraInfo), and what real Chrome's says when there
is one. What a rule holds fails closed.

    ZEBKIT_DIR=<package> PYTHONPATH=bindings/python:tests/engine \
        python3 tests/engine/network_stages.py [engine switches...]

ZEBKIT_TEST_CHROME names Chrome, otherwise it is looked for on the PATH; without one, that
comparison is reported as not run.
"""
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import base64
import hashlib
import json
import os
import shutil
import socket
import sys
import tempfile
import threading
import time
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import harness  # noqa: E402
from harness import Cdp, check, finish, launch_engine, settled, skip, zebkit  # noqa: E402

CREDENTIALS = base64.b64encode(b'user:secret').decode()
AUTHORIZATION = 'Basic ' + CREDENTIALS
WORKER = b"fetch('/echo?worker').then(r => r.text()).then(t => postMessage(t), () => postMessage('failed'))"
errors = []
wire = []
wire_lock = threading.Lock()


class Handler(BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'

    def log_message(self, *args):
        pass

    def answer(self, status=200, body=b'', kind='text/plain', extra=()):
        self.send_response(status)
        self.send_header('Content-Type', kind)
        self.send_header('Content-Length', str(len(body)))
        self.send_header('Cache-Control', 'no-store')
        self.send_header('Access-Control-Allow-Origin', self.headers.get('Origin', '*'))
        self.send_header('Access-Control-Allow-Headers', 'X-Asked')
        for name, value in extra:
            self.send_header(name, value)
        self.end_headers()
        self.wfile.write(body)

    def do_OPTIONS(self):
        with wire_lock:
            wire.append(('OPTIONS', self.path, {k.lower(): v for k, v in self.headers.items()}))
        self.answer(204)

    def do_GET(self):
        with wire_lock:
            wire.append(('GET', self.path, {k.lower(): v for k, v in self.headers.items()}))
        path = self.path.split('?')[0]
        if path == '/ws':
            # A WebSocket's handshake, then nothing: what it sent is on the wire.
            key = self.headers.get('Sec-WebSocket-Key', '')
            accept = base64.b64encode(hashlib.sha1((key + '258EAFA5-E914-47DA-95CA-C5AB0DC85B11').encode()).digest())
            self.send_response(101)
            self.send_header('Upgrade', 'websocket')
            self.send_header('Connection', 'Upgrade')
            self.send_header('Sec-WebSocket-Accept', accept.decode())
            self.end_headers()
            self.wfile.flush()
            self.close_connection = True
            return
        if path == '/page':
            self.answer(body=b'<!doctype html><title>page</title>', kind='text/html',
                        extra=[('Set-Cookie', 'added=by-network; Path=/; HttpOnly')])
        elif path == '/set-cookie':
            self.answer(extra=[('Set-Cookie', 'kept=' + self.path.split('?')[1] + '; Path=/; HttpOnly')])
        elif path == '/redirect':
            self.answer(302, extra=[('Location', '/echo?after')])
        elif path == '/worker.js':
            self.answer(body=WORKER, kind='text/javascript')
        elif path.startswith('/auth/'):
            if self.headers.get('Authorization') != AUTHORIZATION:
                self.answer(401, extra=[('WWW-Authenticate', 'Basic realm="stages"')])
            elif path == '/auth/page':
                self.answer(body=b'<!doctype html><title>signed in</title>', kind='text/html')
            else:
                self.answer(body=b'in')
        else:
            self.answer(body=b'echo')


def on_wire(path):
    with wire_lock:
        return [headers for _, seen, headers in wire if seen == path]


def forget():
    with wire_lock:
        wire.clear()


def wait(condition, label, seconds=20):
    """harness.wait(), a failed check "in time: <label>" when it times out; an
    error in a function of the test's stops the test."""
    done = harness.wait(lambda: errors or condition(), seconds)
    assert not errors, errors
    if not done:
        check(False, 'in time: ' + label)
    return done


def guarded(function):
    def call(*args):
        try:
            function(*args)
        except BaseException as error:  # noqa: BLE001
            errors.append(repr(error))
    return call


def ev(view, script):
    """The script's value; None when it threw or no answer came (a failed
    check "in time: <script>")."""
    value = harness.ev(view, script, 20)
    assert not errors, errors
    if not settled(value):
        check(False, 'in time: ' + script)
        return None
    return None if isinstance(value, tuple) else value


def opened(url):
    view = zebkit.View()
    view.load(url)
    wait(lambda: ev(view, 'location.href + " " + document.readyState') == url + ' complete', 'load ' + url)
    return view


def fetched(view, url, options='{}'):
    return ev(view, f"fetch({json.dumps(url)}, {options}).then(r => r.text(), () => 'failed')")


def named(headers, name):
    return [h['value'] for h in headers if h['name'].lower() == name.lower()]


def plain(headers):
    return {h['name'].lower(): h['value'] for h in headers}


def told(view):
    events = []
    view.on_network_event(guarded(events.append))
    return events


def test_seen(base):
    """Each stage has what is there by then, and the wire has what the last said."""
    view = opened(base + '/page')
    events = told(view)
    view.set_network_filter([{'url': '**/echo?seen', 'mode': 'observe'},
                             {'url': '**/echo?seen', 'mode': 'observe', 'stage': 'sendHeaders'},
                             {'url': '**/echo?seen', 'mode': 'observe', 'stage': 'sent'},
                             {'url': '**/set-cookie*', 'mode': 'observe', 'stage': 'headersReceived'},
                             {'url': '**/set-cookie*', 'mode': 'observe', 'stage': 'response'}])
    check(fetched(view, '/echo?seen') == 'echo', 'a request with observers goes on')
    wait(lambda: {'request', 'sendHeaders', 'sent'} <= {e.get('kind') for e in events}, 'the three stages told')
    stages = {e['kind']: e for e in events if e.get('kind') in ('request', 'sendHeaders', 'sent')}
    if len(stages) == 3:
        check(not named(stages['request']['headers'], 'Cookie'), 'request: no Cookie yet')
        check(named(stages['sendHeaders']['headers'], 'Cookie') == ['added=by-network'], 'sendHeaders: the Cookie',
              stages['sendHeaders']['headers'])
        check(named(stages['sent']['headers'], 'Cookie') == ['added=by-network'], 'sent: the Cookie')
        check(named(stages['sent']['headers'], 'Host') != [], 'sent: Host, which only the wire has')
        check(not named(stages['sendHeaders']['headers'], 'Host'), 'sendHeaders: no Host yet')
        check(stages['request']['requestId'] == stages['sent']['requestId'] == stages['sendHeaders']['requestId'],
              'one requestId through the stages')
        check(stages['sent']['url'].endswith('/echo?seen'), 'with its url')
        sent = plain(stages['sent']['headers'])
        arrived = on_wire('/echo?seen')[-1]
        check(sent == arrived, 'sent is what the server received', {k: (sent.get(k), arrived.get(k))
                                                                     for k in set(sent) ^ set(arrived)})
    fetched(view, '/set-cookie?seen')
    wait(lambda: {'headersReceived', 'response'} <= {e.get('kind') for e in events}, 'response stages told')
    event = next((e for e in events if e.get('kind') == 'headersReceived'), {})
    check(named(event.get('responseHeaders', []), 'Set-Cookie') == ['kept=seen; Path=/; HttpOnly'], 'headersReceived: Set-Cookie')
    event = next((e for e in events if e.get('kind') == 'response'), {})
    check(not named(event.get('responseHeaders', []), 'Set-Cookie'), 'response: the network has taken it out')
    view.close()


def test_changed(base):
    view = opened(base + '/page')
    view.set_network_filter([{'url': '**/echo?change', 'stage': 'sendHeaders'}])
    asked = []

    def decide(request):
        asked.append(request)
        metadata = request.metadata
        assert metadata['stage'] == 'sendHeaders' and metadata['requestId'], metadata
        # The page's stage refuses these; inside the network they are the application's.
        assert not request.respond('fulfill', body=b'x', status=200)
        assert not request.respond('redirect', url=base + '/echo')
        assert not request.respond_file('/nonexistent', status=200)
        assert request.respond_stream(status=200) is None
        headers = [h for h in metadata['headers'] if h['name'].lower() != 'cookie']
        assert request.respond('continue', headers=headers + [{'name': 'Cookie', 'value': 'changed=1'},
                                                              {'name': 'X-Added', 'value': 'late'}])
    view.on_network(guarded(decide))
    check(fetched(view, '/echo?change') == 'echo' and len(asked) == 1, 'a held request goes on')
    arrived = on_wire('/echo?change')[-1]
    check(arrived.get('cookie') == 'changed=1' and arrived.get('x-added') == 'late', 'the wire has what was answered', arrived)

    # Taken out.
    view.set_network_filter([{'url': '**/echo?remove', 'stage': 'sendHeaders'}])
    view.on_network(guarded(lambda request: request.respond(
        'continue', headers=[h for h in request.metadata['headers'] if h['name'].lower() != 'cookie'])))
    fetched(view, '/echo?remove')
    check('cookie' not in on_wire('/echo?remove')[-1], 'a Cookie taken out does not go')

    # A navigation.
    seen = []
    view.set_network_filter([{'url': '**/page?nav', 'stage': 'sendHeaders', 'resourceTypes': ['document']}])
    view.on_network(guarded(lambda request: (seen.append(request.metadata), request.respond('continue'))))
    view.load(base + '/page?nav')
    wait(lambda: on_wire('/page?nav'), 'the navigation')
    check(seen and 'added=by-network' in ''.join(named(seen[0]['headers'], 'Cookie')) and seen[0]['navigation'] and seen[0]['mainFrame'],
          'a navigation: its Cookie, held', seen and seen[0]['headers'])
    view.close()


def test_websocket(base):
    """A WebSocket's handshake has its own client in the network: its Cookie is seen
    and changed at sendHeaders, as a loader's is."""
    view = opened(base + '/page')
    events = told(view)
    ws = base.replace('http:', 'ws:') + '/ws'
    view.set_network_filter([{'url': '**/ws?seen', 'mode': 'observe', 'stage': 'sendHeaders'},
                             {'url': '**/ws?seen', 'mode': 'observe', 'stage': 'headersReceived'}])
    opened_socket = ("new Promise(f => { const s = new WebSocket(%s); "
                     "s.onopen = () => f('open'); s.onerror = () => f('error'); })")
    check(ev(view, opened_socket % json.dumps(ws + '?seen')) == 'open', 'a WebSocket with observers opens')
    wait(lambda: {'sendHeaders', 'headersReceived'} <= {e.get('kind') for e in events}, 'its stages told')
    stages = {e['kind']: e for e in events if e.get('kind') in ('sendHeaders', 'headersReceived')}
    if len(stages) == 2:
        check(stages['sendHeaders'].get('resourceType') == 'websocket', 'of type websocket', stages['sendHeaders'].get('resourceType'))
        check('added=by-network' in ''.join(named(stages['sendHeaders']['headers'], 'Cookie')), 'sendHeaders: the socket\'s Cookie',
              stages['sendHeaders']['headers'])
        check(stages['headersReceived'].get('status') == 101, 'headersReceived: the handshake\'s 101',
              stages['headersReceived'].get('status'))
    view.set_network_filter([{'url': '**/ws?change', 'stage': 'sendHeaders'}])

    def decide(request):
        headers = [h for h in request.metadata['headers'] if h['name'].lower() != 'cookie']
        request.respond('continue', headers=headers + [{'name': 'Cookie', 'value': 'socket=changed'}])
    view.on_network(guarded(decide))
    check(ev(view, opened_socket % json.dumps(ws + '?change')) == 'open', 'a held WebSocket goes on')
    arrived = on_wire('/ws?change')
    check(arrived and arrived[-1].get('cookie') == 'socket=changed', 'the wire has the Cookie answered',
          arrived and arrived[-1].get('cookie'))
    view.set_network_filter([{'url': '**/ws?block', 'stage': 'sendHeaders'}])
    view.on_network(guarded(lambda request: request.respond('block')))
    check(ev(view, opened_socket % json.dumps(ws + '?block')) == 'error' and not on_wire('/ws?block'),
          'one blocked at sendHeaders never reaches the server')
    view.close()


def test_redirect(base):
    view = opened(base + '/page')
    seen = []
    view.set_network_filter([{'url': '**/redirect', 'stage': 'sendHeaders'}, {'url': '**/echo?after', 'stage': 'sendHeaders'}])
    view.on_network(guarded(lambda request: (seen.append(request.metadata), request.respond('continue'))))
    check(fetched(view, '/redirect') == 'echo', 'through a redirect')
    check([m['url'].rsplit('/', 1)[1] for m in seen] == ['redirect', 'echo?after'], 'each hop is held', [m['url'] for m in seen])
    check(len({m['requestId'] for m in seen}) == 1, 'the same requestId')
    check(all(named(m['headers'], 'Cookie') for m in seen), 'each hop with its Cookie')
    view.close()


def test_closed(base):
    """What a rule is for is not got past."""
    view = opened(base + '/page')
    events = told(view)
    view.set_network_timeout(500)

    forget()
    view.set_network_filter([{'url': '**/echo?block', 'stage': 'sendHeaders'}])
    view.on_network(guarded(lambda request: request.respond('block')))
    check(fetched(view, '/echo?block') == 'failed' and not on_wire('/echo?block'), 'block: it does not reach the server')

    view.set_network_filter([{'url': '**/echo?silent', 'stage': 'sendHeaders'}])
    kept = []
    view.on_network(guarded(kept.append))
    check(fetched(view, '/echo?silent') == 'failed' and not on_wire('/echo?silent'), 'no answer in time: it fails')
    wait(lambda: any(e.get('reason') == 'timeout' for e in events), 'told')
    timed = next((e for e in events if e.get('reason') == 'timeout'), {})
    check(timed.get('state') == 'refused', 'and is told as refused', timed.get('state'))

    view.set_network_timeout(30000)
    view.set_network_filter([{'url': '**/echo?replaced', 'stage': 'sendHeaders'}])
    kept.clear()
    box = []
    view.evaluate("fetch('/echo?replaced').then(r => r.text(), () => 'failed')", lambda value, error=None: box.append(value))
    wait(lambda: kept, 'asked')
    view.on_network(guarded(lambda request: request.respond('continue')))
    wait(lambda: box, 'the answer')
    check(box == ['failed'] and not on_wire('/echo?replaced'), 'its function replaced: it fails', box)

    view.on_network(None)
    view.set_network_filter([{'url': '**/echo?nobody', 'stage': 'sendHeaders'}])
    check(fetched(view, '/echo?nobody') == 'failed' and not on_wire('/echo?nobody'), 'no function: it fails')

    view.set_network_filter([{'url': '**/set-cookie*', 'stage': 'headersReceived'}])
    check(fetched(view, '/set-cookie?nobody') == 'failed', 'no function at headersReceived: it fails')
    view.on_network(guarded(lambda request: request.respond('block')))
    check(fetched(view, '/set-cookie?blocked') == 'failed', 'block at headersReceived')
    view.set_network_filter([])
    check('kept=blocked' not in on_wire('/echo?none')
          and fetched(view, '/echo?none') == 'echo'
          and 'kept=blocked' not in on_wire('/echo?none')[-1].get('cookie', ''), 'a response blocked there stores no cookie')
    view.close()

    # Not what a rule is not for.
    view = opened(base + '/page')
    view.set_network_filter([{'url': '**/nothing-here', 'stage': 'sendHeaders'}])
    check(fetched(view, '/echo?other') == 'echo', 'a request no rule matches goes on')
    view.close()


def test_filters(base):
    view = opened(base + '/page')
    for invalid in ([{'stage': 'sent'}], [{'stage': 'sent', 'mode': 'hold'}], [{'stage': 'early'}],
                    [{'stage': 'sendHeaders', 'body': True, 'mode': 'observe'}]):
        try:
            view.set_network_filter(invalid)
        except zebkit.Error:
            check(True, 'refused', invalid)
        else:
            check(False, 'refused', invalid)
    view.close()


def test_response(base):
    """Before the cookie store, unlike the response stage."""
    view = opened(base + '/page')
    view.set_network_filter([{'url': '**/set-cookie?stripped', 'stage': 'headersReceived'}])
    seen = []

    def decide(request):
        seen.append(request.metadata)
        assert request.respond('continue', headers=[h for h in request.metadata['responseHeaders']
                                                    if h['name'].lower() != 'set-cookie'] +
                               [{'name': 'X-Added', 'value': 'network'}])
    view.on_network(guarded(decide))
    answer = ev(view, "fetch('/set-cookie?stripped').then(r => r.headers.get('X-Added'), () => 'failed')")
    check(answer == 'network', 'changed headers reach the page', answer)
    check(seen and seen[0]['status'] == 200 and seen[0]['url'].endswith('/set-cookie?stripped'), 'its status and url')
    fetched(view, '/echo?after-strip')
    check('kept=stripped' not in on_wire('/echo?after-strip')[-1].get('cookie', ''), 'a Set-Cookie taken out stores nothing',
          on_wire('/echo?after-strip')[-1].get('cookie'))
    view.close()


def test_preflight(base, other):
    view = opened(base + '/page')
    seen = []
    rule = [{'url': '**/echo?cross', 'stage': 'sendHeaders'}]
    view.set_network_filter(rule)
    view.on_network(guarded(lambda request: (seen.append(request.metadata), request.respond('continue'))))
    url = other + '/echo?cross'
    options = '{headers: {"X-Asked": "1"}}'
    check(fetched(view, url, options) == 'echo', 'a preflighted request goes on')
    preflights = [m for m in seen if m.get('preflight')]
    actual = [m for m in seen if not m.get('preflight')]
    check(len(preflights) == 1 and preflights[0]['method'] == 'OPTIONS' and preflights[0]['resourceType'] == 'preflight',
          'its preflight is held', preflights)
    check(preflights and named(preflights[0]['headers'], 'Access-Control-Request-Headers') != [],
          'with the headers the network gives it')
    check(len(actual) == 1 and actual[0]['method'] == 'GET', 'and then the request')

    forget()
    view.set_network_filter([{'url': '**/echo?cross2', 'stage': 'sendHeaders', 'resourceTypes': ['preflight']}])
    view.on_network(guarded(lambda request: request.respond('block')))
    check(fetched(view, other + '/echo?cross2', options) == 'failed' and not on_wire('/echo?cross2'),
          'a preflight blocked: neither reaches the server')

    view.on_network(None)
    forget()
    check(fetched(view, other + '/echo?cross2', options) == 'failed' and not on_wire('/echo?cross2'),
          'a preflight no one answers: the same')
    view.close()


def test_worker(base):
    view = opened(base + '/page')
    seen = []
    view.set_network_filter([{'url': '**/echo?worker', 'stage': 'sendHeaders'}])
    view.on_network(guarded(lambda request: (seen.append(request.metadata), request.respond('continue'))))
    answer = ev(view, "new Promise(ok => { const w = new Worker('/worker.js'); w.onmessage = e => ok(e.data); })")
    check(answer == 'echo', 'a dedicated worker\'s fetch goes on', answer)
    check(seen and seen[0]['worker'] and 'added=by-network' in ''.join(named(seen[0]['headers'], 'Cookie')), 'held, with its Cookie')
    view.close()


class Session(threading.Thread):
    """CDP on its own thread, while the main thread pumps the engine."""

    def __init__(self, url, target_url=None):
        super().__init__(daemon=True)
        self.cdp = Cdp(url)
        self.target_url = target_url

    def attach(self):
        targets = self.cdp.call('Target.getTargets')['targetInfos']
        if self.target_url:
            target = next(t['targetId'] for t in targets if t['type'] == 'page' and self.target_url in t['url'])
        else:
            target = self.cdp.call('Target.createTarget', {'url': 'about:blank'})['targetId']
        self.session = self.cdp.call('Target.attachToTarget', {'targetId': target, 'flatten': True})['sessionId']
        for domain in ('Network', 'Runtime', 'Page'):
            self.cdp.call(domain + '.enable', session=self.session)


def off_thread(function):
    box = []

    def run():
        try:
            box.append((function(), None))
        except BaseException as error:  # noqa: BLE001
            box.append((None, error))
    thread = threading.Thread(target=run, daemon=True)
    thread.start()
    if not wait(lambda: box, 'CDP', 40):
        return None
    if box[0][1]:
        raise box[0][1]
    return box[0][0]


def extra_info(cdp, session, url_end, seconds=10):
    """The headers of each ExtraInfo of the request for @url_end, in order."""
    ids = set()
    found = []
    end = time.monotonic() + seconds
    while time.monotonic() < end and not found:
        for message in cdp.events:
            if message.get('method') == 'Network.requestWillBeSent' and message['params']['request']['url'].endswith(url_end):
                ids.add(message['params']['requestId'])
        found = [message['params']['headers'] for message in cdp.events
                 if message.get('method') == 'Network.requestWillBeSentExtraInfo' and message['params']['requestId'] in ids]
        if not found:
            cdp.event('#none', 0.2)
    return found


def test_cdp(base, port):
    """The engine's own DevTools and the hook, of one request; also with CDP's Fetch in the chain."""
    view = zebkit.View()
    view.load(base.replace('//', '//user:secret@') + '/auth/page')
    wait(lambda: ev(view, 'document.title') == 'signed in', 'signed in by the URL')
    events = told(view)
    view.set_network_filter([{'url': '**/auth/echo*', 'mode': 'observe', 'stage': 'sendHeaders'},
                             {'url': '**/auth/echo*', 'mode': 'observe', 'stage': 'sent'}])
    # The engine serves these on the thread this one pumps.
    def attached():
        with urllib.request.urlopen(f'http://127.0.0.1:{port}/json/version') as answer:
            url = json.load(answer)['webSocketDebuggerUrl']
        session = Session(url, '/auth/page')
        session.attach()
        return session
    session = off_thread(attached)

    # A URL with credentials is no base for a fetch.
    fetched(view, base + '/auth/echo?cdp')
    wait(lambda: sum(e.get('kind') == 'sent' for e in events) >= 1, 'sent told')
    infos = off_thread(lambda: extra_info(session.cdp, session.session, '/auth/echo?cdp'))
    sent = next((e for e in events if e.get('kind') == 'sent'), {'headers': []})
    send = next((e for e in events if e.get('kind') == 'sendHeaders'), {'headers': []})
    check(named(sent['headers'], 'Authorization') == [AUTHORIZATION], 'sent: the Authorization the network adds', sent['headers'])
    check(named(send['headers'], 'Authorization') == [], 'sendHeaders: not yet, as in Chromium its transaction adds it')
    check(bool(infos), "CDP's ExtraInfo of the same request")
    theirs = {k.lower(): v for k, v in infos[0].items()} if infos else {}
    ours = plain(sent['headers'])
    check(ours == theirs, 'sent is what CDP says', {k: (ours.get(k), theirs.get(k)) for k in set(ours) ^ set(theirs)})
    check(on_wire('/auth/echo?cdp')[-1] == ours, 'and what the server received')

    # With CDP's Fetch holding the same request.
    events.clear()
    off_thread(lambda: session.cdp.call('Fetch.enable', {'patterns': [{'urlPattern': '*fetch-too*'}]}, session=session.session))
    box = []
    view.evaluate(f"fetch('{base}/auth/echo?fetch-too').then(r => r.text(), () => 'failed')", lambda value, error=None: box.append(value))

    def continued():
        paused = session.cdp.event('Fetch.requestPaused', 15)
        session.cdp.call('Fetch.continueRequest', {'requestId': paused['params']['requestId']}, session=session.session)
    off_thread(continued)
    wait(lambda: box, 'the fetch through CDP')
    check(box == ['in'], "through CDP's Fetch", box)
    wait(lambda: any(e.get('kind') == 'sent' for e in events), 'sent with Fetch')
    sent = next((e for e in events if e.get('kind') == 'sent'), {'headers': []})
    check(named(sent['headers'], 'Authorization') == [AUTHORIZATION], "with CDP's Fetch too")
    off_thread(lambda: session.cdp.call('Fetch.disable', session=session.session))
    view.close()
    return ours


def test_chrome(base, engine_sent):
    chrome = os.environ.get('ZEBKIT_TEST_CHROME') or shutil.which('google-chrome') or shutil.which('chromium')
    if not chrome:
        skip('the comparison with Chrome', 'no Chrome here (ZEBKIT_TEST_CHROME)')
        return
    with tempfile.TemporaryDirectory(prefix='zebkit-stages-chrome-', ignore_cleanup_errors=True) as profile:
        # Started as the engine is, on the port it is given: its DevTools line.
        chrome_engine = launch_engine([chrome, '--headless=new', '--no-sandbox', '--no-first-run',
                                       f'--user-data-dir={profile}'])
        try:
            url = chrome_engine.url
            if not url:
                check(False, 'Chrome serves CDP')
                return
            session = Session(url)
            session.attach()
            cdp, sid = session.cdp, session.session

            def evaluate(script):
                return cdp.call('Runtime.evaluate', {'expression': script, 'returnByValue': True, 'awaitPromise': True},
                                session=sid)['result'].get('value')
            for page in ('/page', '/auth/page'):
                target = base + page if page == '/page' else base.replace('//', '//user:secret@') + page
                cdp.call('Page.navigate', {'url': target}, session=sid)
                cdp.event('Page.loadEventFired', 15)
            check(evaluate('document.title') == 'signed in', 'Chrome: signed in by the URL')
            forget()
            answer = evaluate(f"fetch('{base}/auth/echo?chrome').then(r => r.text(), () => 'failed')")
            check(answer == 'in', 'Chrome: the same fetch', answer)
            infos = extra_info(cdp, sid, '/auth/echo?chrome')
            theirs = {k.lower(): v for k, v in infos[0].items()} if infos else {}
            check(theirs.get('authorization') == AUTHORIZATION, 'Chrome: DevTools shows the Authorization')
            check('added=by-network' in theirs.get('cookie', ''), 'Chrome: DevTools shows the Cookie', theirs.get('cookie'))
            check(theirs.get('authorization') == engine_sent.get('authorization'), 'the same Authorization as ours')
            check(theirs.get('host') == engine_sent.get('host'), 'the same Host as ours')
            check('added=by-network' in engine_sent.get('cookie', ''), 'the same Cookie as ours', engine_sent.get('cookie'))
            print('names only in Chrome:', sorted(set(theirs) - set(engine_sent)),
                  'only in the engine:', sorted(set(engine_sent) - set(theirs)), flush=True)
        finally:
            chrome_engine.stop()


def main():
    server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
    server.daemon_threads = True
    threading.Thread(target=server.serve_forever, daemon=True).start()
    base = f'http://127.0.0.1:{server.server_port}'
    other = f'http://localhost:{server.server_port}'
    with socket.socket() as free:
        free.bind(('127.0.0.1', 0))
        cdp_port = free.getsockname()[1]
    with tempfile.TemporaryDirectory(prefix='zebkit-stages-') as profile:
        if zebkit.init(['network-stages', f'--user-data-dir={profile}', f'--remote-debugging-port={cdp_port}'] + sys.argv[1:]) != 0:
            check(False, 'zebkit.init()')
            return finish()
        engine_sent = {}
        try:
            test_seen(base)
            test_changed(base)
            test_websocket(base)
            test_redirect(base)
            test_closed(base)
            test_filters(base)
            test_response(base)
            test_preflight(base, other)
            test_worker(base)
            engine_sent = test_cdp(base, cdp_port)
            harness.pump(.1)
            check(not errors, 'no error in a function', errors)
        finally:
            zebkit.shutdown()
    test_chrome(base, engine_sent)
    return finish()


sys.exit(main())
