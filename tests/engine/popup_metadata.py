"""New-window decisions and created-view metadata use actual Chromium parameters."""
import ctypes
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import sys
import threading
import time
import traceback
from urllib.parse import urlsplit
import zebkit

class Server(BaseHTTPRequestHandler):
    def log_message(self, *args): pass
    def do_GET(self):
        path = urlsplit(self.path).path
        body = '<title>' + path + '</title>'
        if path == '/blocked':
            body += "<script>window.open('/popup-blocked','blocked','left=0,top=0,width=420,height=360,popup=yes')</script>"
        if path == '/with-frame':
            body += f'<iframe src="http://localhost:{self.server.server_port}/frame"></iframe>'
        if path == '/frame':
            body += "<script>window.open('/from-frame','from-frame','width=300,height=240,popup=yes')</script>"
        data = body.encode()
        self.send_response(200)
        self.send_header('Content-Type', 'text/html')
        self.send_header('Content-Length', str(len(data)))
        self.send_header('Cache-Control', 'no-store')
        self.end_headers()
        try: self.wfile.write(data)
        except (BrokenPipeError, ConnectionResetError): pass

server = ThreadingHTTPServer(('127.0.0.1', 0), Server)
server_thread = threading.Thread(target=server.serve_forever, daemon=True)
server_thread.start()
base = f'http://127.0.0.1:{server.server_port}'
P, V, S = zebkit._P, zebkit._V, zebkit._S
EVENT = ctypes.CFUNCTYPE(None, V, S, P)
WINDOW = ctypes.CFUNCTYPE(None, V, V, S, P)
CLOSE = ctypes.CFUNCTYPE(None, V, P)
on_event = zebkit._declare('zebkit_view_on_window_open_event', None, V, EVENT, P)
get_info = zebkit._declare('zebkit_view_get_window_open_info', P, V)
on_window = zebkit._declare('zebkit_view_on_window_open', None, V, WINDOW, P)
events, legacy, windows, errors, callbacks = [], [], [], [], []
closed = set()


def wait(predicate, label='popup condition', timeout=15):
    until = time.monotonic() + timeout
    while not predicate() and time.monotonic() < until:
        assert not errors, errors
        assert zebkit.pump()
        time.sleep(.001)
    assert not errors, errors
    assert predicate(), label


def evaluate(view, script):
    answers = []
    view.evaluate(script, lambda value, error: answers.append((value, error)))
    wait(lambda: bool(answers), script)
    assert answers[0][1] is None, answers
    return answers[0][0]


def info(handle):
    value = get_info(handle)
    return json.loads(zebkit._string(value)) if value else None


def url(handle):
    return zebkit._string(zebkit._view_get_url(handle))


def load(view, path):
    view.load(base + path)
    wait(lambda: view.url == base + path and evaluate(view, 'document.readyState') == 'complete', path)


@EVENT
def event(handle, payload, data):
    try: events.append((handle, json.loads(payload)))
    except BaseException: errors.append(traceback.format_exc())


@EVENT
def blocked(handle, payload, data):
    try: legacy.append(json.loads(payload))
    except BaseException: errors.append(traceback.format_exc())


@CLOSE
def close(handle, data):
    closed.add(int(data))


@WINDOW
def created(opener, handle, target, data):
    try:
        creation = info(handle)
        assert creation['phase'] == 'created' and creation['url'] == target.decode()
        serial = len(windows) + 1
        zebkit._view_on_close(handle, close, P(serial))
        windows.append((serial, handle, creation))
        if creation['frameName'] == 'close-immediately': zebkit._view_close(handle)
    except BaseException: errors.append(traceback.format_exc())
callbacks.extend([event, blocked, close, created])


assert zebkit.init([sys.argv[0], *sys.argv[1:]], settings={'logging': {'destination': 'none'}}) == 0
try:
    first, other = zebkit.View(), zebkit.View()
    assert info(first._check()) == {} and info(None) is None
    load(first, '/initial')
    on_event(first._check(), event, None)
    wrong_thread = []
    def probe_wrong_thread():
        wrong_thread.append(info(first._handle))
        on_event(first._handle, EVENT(), None)
    probe = threading.Thread(target=probe_wrong_thread)
    probe.start(); probe.join()
    assert wrong_thread == [None]
    on_window(first._check(), created, None)
    # The old three-field event stays exactly compatible.
    zebkit._view_on_popup_blocked(first._check(), ctypes.cast(blocked, zebkit._POPUP_BLOCKED), None)
    first.set_popup_policy('block')
    load(first, '/blocked')
    wait(lambda: bool(events) and bool(legacy))
    decision = events[-1][1]
    assert decision['phase'] == 'decision' and decision['allowed'] is False
    assert decision['url'] == base + '/popup-blocked' and decision['frameName'] == 'blocked'
    assert decision['userGesture'] is False and decision['disposition'] == 'new-popup'
    assert decision['openerUrl'] == decision['topUrl'] == base + '/blocked'
    assert decision['sourceOrigin'] == base and not decision['openerSuppressed']
    features = decision['features']
    assert features['bounds'] == {'x':0,'y':0,'width':420,'height':360}
    assert all(features[k] is True for k in ('hasX','hasY','hasWidth','hasHeight','popup'))
    assert features['alwaysOnTop'] is False
    assert legacy == [{'url':base + '/popup-blocked','frameName':'blocked','userGesture':False}] and not windows
    count = len(events)
    other.set_popup_policy('block')
    load(other, '/blocked')
    assert len(events) == count
    first.set_popup_policy('allow')
    before = len(windows)
    assert evaluate(first, "window.one=window.open('/one','one','left=0,top=0,width=420,height=360,popup=yes'); window.two=window.open('/two','two','left=50,top=60,width=340,height=260,popup=yes'); !!one && !!two") is True
    wait(lambda: len(windows) == before + 2)
    wait(lambda: len([e for _, e in events if e['phase'] == 'created']) == 2)
    by_url = {creation['url']: (serial, handle, creation) for serial, handle, creation in windows}
    assert {base + '/one', base + '/two'} <= by_url.keys()
    first_info = by_url[base + '/one'][2]
    second_info = by_url[base + '/two'][2]
    assert first_info['features']['bounds'] == {'x':0,'y':0,'width':420,'height':360}
    assert second_info['features']['bounds'] == {'x':50,'y':60,'width':340,'height':260}
    assert first_info['targetId'] != second_info['targetId']
    assert first_info['hasOpener'] and first_info['openerOrigin'] == first_info['topOrigin'] == base
    assert first_info['frameName'] == 'one' and second_info['frameName'] == 'two'
    native_events = {e['targetId']: e for _, e in events if e['phase'] == 'created'}
    assert native_events[first_info['targetId']] == first_info
    assert native_events[second_info['targetId']] == second_info
    # Creation snapshots survive later navigation of that exact window.
    child = by_url[base + '/one'][1]
    zebkit._view_load_url(child, (base + '/moved').encode())
    wait(lambda: url(child) == base + '/moved')
    assert info(child) == first_info
    before = len(windows)
    evaluate(first, "window.open('/plain','plain'); true")
    wait(lambda: len(windows) == before + 1)
    plain = windows[-1][2]
    assert not any(plain['features'][key] for key in ('hasX','hasY','hasWidth','hasHeight','popup'))
    before = len(windows)
    evaluate(first, "window.open('/no-opener','_blank','noopener,width=300,height=240'); true")
    wait(lambda: len(windows) == before + 1)
    no_opener = windows[-1][2]
    assert no_opener['hasOpener'] is False and no_opener['openerOrigin'] is None
    wait(lambda: any(e['url'] == base + '/no-opener' and e['phase'] == 'decision' and e['openerSuppressed'] for _, e in events))
    before = len(windows)
    evaluate(first, "window.open('/closed-during-creation','close-immediately'); true")
    wait(lambda: len(windows) == before + 1 and windows[-1][0] in closed, 'close during actual creation callback')
    first.set_popup_policy('block')
    load(first, '/with-frame')
    child_origin = base.replace('127.0.0.1', 'localhost')
    wait(lambda: any(e.get('url') == child_origin + '/from-frame' for _, e in events))
    cross = next(e for _, e in events if e.get('url') == child_origin + '/from-frame')
    assert cross['sourceOrigin'] == child_origin and cross['openerUrl'] == child_origin + '/frame'
    assert cross['topUrl'] == base + '/with-frame' and cross['allowed'] is False
    # Removal affects future reports, while ordinary blocked callbacks continue.
    on_event(first._check(), EVENT(), None)
    count, blocked_count = len(events), len(legacy)
    evaluate(first, "window.open('/removed','removed'); true")
    wait(lambda: len(legacy) > blocked_count)
    assert len(events) == count
    for serial, handle, creation in windows:
        if serial not in closed: zebkit._view_close(handle)
    wait(lambda: len(closed) == len(windows))
    victim = zebkit.View()
    load(victim, '/victim')
    victim.set_popup_policy('block')
    @EVENT
    def close_in_event(handle, payload, data):
        victim.close()
    callbacks.append(close_in_event)
    on_event(victim._check(), close_in_event, None)
    victim.evaluate("window.open('/close','close'); true")
    wait(lambda: victim._closed)
    print('ok new-window metadata: decisions, creation snapshots, explicit zero bounds, multiple windows, noopener, OOPIF, compatibility and closure', flush=True)
finally:
    assert zebkit.shutdown()
    server.shutdown(); server.server_close(); server_thread.join()
assert not errors, errors
