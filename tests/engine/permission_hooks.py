"""Native permission decisions use real Chromium requests, with synthetic location."""
import ctypes
import json
import sys
import time
import traceback
import threading
from cdp_page import Cdp
from urllib.parse import urlsplit
import zebkit

base = sys.argv[1]
origin = '{0.scheme}://{0.netloc}'.format(urlsplit(base))
INVALID, PROMPT, GRANTED, DENIED = -1, 0, 1, 2
P, V, S = zebkit._P, zebkit._V, zebkit._S
PERMISSION = ctypes.CFUNCTYPE(None, V, P, S, P)
RESULT = ctypes.CFUNCTYPE(None, V, S, P)
AUTO = ctypes.CFUNCTYPE(None, V, S, S, P)
set_rule = zebkit._declare('zebkit_view_set_permission', ctypes.c_bool, V, S, S, ctypes.c_int, ctypes.POINTER(P))
get_rule = zebkit._declare('zebkit_view_get_permission', ctypes.c_int, V, S, S)
reset_rules = zebkit._declare('zebkit_view_reset_permissions', ctypes.c_bool, V, S, ctypes.POINTER(P))
set_timeout = zebkit._declare('zebkit_view_set_permission_timeout', ctypes.c_bool, V, ctypes.c_uint32)
on_permission = zebkit._declare('zebkit_view_on_permission', None, V, PERMISSION, P)
on_result = zebkit._declare('zebkit_view_on_permission_result', None, V, RESULT, P)
request_info = zebkit._declare('zebkit_permission_request_info', P, P)
respond = zebkit._declare('zebkit_permission_respond', ctypes.c_bool, P, ctypes.c_int, ctypes.c_bool)
automation = zebkit._declare('zebkit_automation_call', None, V, S, S, AUTO, P)
assert zebkit.init([sys.argv[0], *sys.argv[2:]], settings={'logging': {'destination': 'none'}}) == 0
errors, requests, outcomes, callbacks, late = [], [], [], [], []
views = {}
mode = 'hold'
protocol_client = None


def wait(condition, message='condition', timeout=15):
    until = time.monotonic() + timeout
    while not condition() and time.monotonic() < until:
        assert not errors, errors
        assert zebkit.pump()
        time.sleep(.001)
    assert not errors, errors
    assert condition(), message


def evaluate(view, code):
    answers = []
    view.evaluate(code, lambda value, error: answers.append((value, error)))
    wait(lambda: bool(answers), code)
    assert answers[0][1] is None, answers
    return answers[0][0]


def load(view, tag):
    loaded = []
    view.on_load(lambda event, url: loaded.append(True) if event == zebkit.LOAD_FINISHED and url.startswith(base + '/permissions.html') else None)
    view.load(base + '/permissions.html?' + tag)
    wait(lambda: bool(loaded), 'permission document load')


def call(view, method, params):
    answers = []
    @AUTO
    def done(handle, value, error, data):
        answers.append((json.loads(value) if value else None, error.decode() if error else None))
    automation(view._check(), method.encode(), json.dumps(params).encode(), done, None)
    wait(lambda: bool(answers), method)
    assert not answers[0][1], answers
    return answers[0][0]


def cdp(method, params=None):
    answers = []
    def worker():
        global protocol_client
        try:
            if protocol_client is None:
                protocol_client = Cdp(zebkit_cdp_url)
            answers.append((protocol_client.call(method, params), None))
        except BaseException:
            answers.append((None, traceback.format_exc()))
    thread = threading.Thread(target=worker)
    thread.start()
    wait(lambda: not thread.is_alive(), method)
    thread.join()
    assert len(answers) == 1 and answers[0][1] is None, answers
    return answers[0][0]


def info(token):
    value = request_info(token)
    return json.loads(zebkit._string(value)) if value else None


def rule(view, state, name='geolocation', scope=origin, valid=True):
    error = P()
    ok = set_rule(view._check(), scope.encode(), name.encode(), state, ctypes.byref(error))
    message = zebkit._string(error.value)
    assert bool(ok) == valid and (not message if valid else bool(message)), (ok, message)


def reset(view, scope=origin):
    error = P()
    ok = reset_rules(view._check(), scope.encode() if scope is not None else None, ctypes.byref(error))
    assert ok, zebkit._string(error.value)


def state(view):
    return evaluate(view, "permissionState('geolocation')")


def answer_for(token):
    return next((event for handle, event in outcomes if event['id'] == str(token)), None)


def install(view, tag='first'):
    @PERMISSION
    def asked(handle, token, payload, data):
        try:
            target = views[handle]
            event = json.loads(payload)
            assert event['state'] == 'pending' and int(event['id']) == token
            assert info(token) == event
            requests.append((token, event, tag))
            if mode == 'grant-once':
                assert respond(token, GRANTED, False)
            elif mode == 'remember':
                assert respond(token, GRANTED, True)
            elif mode == 'deny':
                assert respond(token, DENIED, False)
            elif mode == 'late':
                time.sleep(.06)  # Deliberately block the host callback past its deadline.
                late.append(bool(respond(token, GRANTED, False)))
            elif mode == 'close':
                target.close()
                # Closing is posted so page callbacks can unwind safely.
        except BaseException:
            errors.append(traceback.format_exc())
    callbacks.append(asked)
    on_permission(view._check(), asked, None)


@RESULT
def completed(handle, payload, data):
    try:
        outcomes.append((handle, json.loads(payload)))
    except BaseException:
        errors.append(traceback.format_exc())
callbacks.append(completed)


def ask(view):
    before = len(requests)
    assert evaluate(view, 'askGeo()') is True
    wait(lambda: len(requests) > before, 'native permission callback')
    return requests[-1][0]


def geo_done(view):
    wait(lambda: evaluate(view, "geoResult!==null && geoResult!=='pending'"), 'geolocation completion')
    return evaluate(view, 'geoResult')


try:
    first, other = zebkit.View(), zebkit.View()
    for view in (first, other):
        views[view._check()] = view
        load(view, 'initial')
        call(view, 'BrowserContext.setGeolocation', {'geolocation': {'latitude': 12.5, 'longitude': 23.75, 'accuracy': 1}})
    assert state(first) == state(other) == 'prompt'
    assert evaluate(first, 'listenGeo()') == 'prompt'
    rule(first, DENIED)
    wait(lambda: evaluate(first, "permissionEvents.includes('denied')"), 'native permission subscription denied')
    assert state(first) == 'denied' and state(other) == 'prompt'
    rule(first, GRANTED)
    assert evaluate(first, 'askGeo()') is True
    assert geo_done(first) == {'latitude': 12.5, 'longitude': 23.75}
    assert get_rule(first._check(), origin.encode(), b'geolocation') == GRANTED
    for scope, name, value in [('data:text/plain,opaque', 'geolocation', DENIED), (origin, 'unknown', GRANTED),
                               (origin, 'geolocation', 99), (origin, 'geolocation-approximate', GRANTED)]:
        rule(first, value, name, scope, valid=False)
        assert state(first) == 'granted'
    reset(first)
    assert state(first) == 'prompt'
    assert not set_timeout(first._check(), 0) and not set_timeout(first._check(), 300001)
    on_result(first._check(), completed, None)
    install(first)
    mode = 'grant-once'
    token = ask(first)
    assert geo_done(first) == {'latitude': 12.5, 'longitude': 23.75}
    wait(lambda: answer_for(token) is not None)
    assert answer_for(token)['reason'] == 'answered' and not answer_for(token)['remembered']
    assert state(first) == state(other) == 'prompt'
    assert info(token) is None and not respond(token, GRANTED, True)
    mode = 'remember'
    token = ask(first)
    assert geo_done(first)['latitude'] == 12.5
    wait(lambda: answer_for(token) is not None)
    assert answer_for(token)['remembered'] and state(first) == 'granted' and state(other) == 'prompt'
    count = len(requests)
    assert evaluate(first, 'askGeo()') is True
    assert geo_done(first)['longitude'] == 23.75 and len(requests) == count
    reset(first)
    mode = 'hold'
    token = ask(first)
    assert not respond(token, 99, False) and info(token) is not None
    wrong_thread = []
    raw_view = first._check()
    worker = threading.Thread(target=lambda: wrong_thread.append((
        bool(respond(token, GRANTED, False)), info(token),
        get_rule(raw_view, origin.encode(), b'geolocation'), bool(set_timeout(raw_view, 1000)))))
    worker.start()
    worker.join(timeout=3)
    assert not worker.is_alive() and wrong_thread == [(False, None, INVALID, False)]
    assert info(token) is not None
    assert not respond(P(-1), GRANTED, False) and request_info(P(-1)) is None
    rule(first, DENIED)
    assert geo_done(first) == {'error': 1}
    wait(lambda: answer_for(token) is not None)
    assert answer_for(token)['reason'] == 'policy-changed' and not respond(token, GRANTED, False)
    reset(first)
    assert set_timeout(first._check(), 40)
    token = ask(first)
    assert geo_done(first) == {'error': 1}
    wait(lambda: answer_for(token) is not None)
    assert answer_for(token)['reason'] == 'timeout' and info(token) is None
    assert not respond(token, GRANTED, False)
    mode = 'late'
    assert set_timeout(first._check(), 20)
    token = ask(first)
    assert geo_done(first) == {'error': 1} and late[-1] is False
    wait(lambda: answer_for(token) is not None)
    assert answer_for(token)['reason'] == 'timeout'
    assert set_timeout(first._check(), 5000)
    mode = 'hold'
    token = ask(first)
    install(first, 'replacement')
    assert geo_done(first) == {'error': 1}
    wait(lambda: answer_for(token) is not None)
    assert answer_for(token)['reason'] == 'handler-changed' and not respond(token, GRANTED, False)
    token = ask(first)
    load(first, 'navigation')
    wait(lambda: answer_for(token) is not None, 'navigation cancels pending permission')
    assert answer_for(token)['reason'] == 'cancelled' and info(token) is None
    replacement = ask(first)
    assert replacement != token and not respond(token, GRANTED, False) and info(replacement)
    assert respond(replacement, DENIED, False) and geo_done(first) == {'error': 1}
    mode = 'deny'
    count = len(requests)
    assert evaluate(first, 'askFrame(false)') is True
    wait(lambda: evaluate(first, '!!frameResult'), 'permissions policy frame denial')
    assert evaluate(first, 'frameResult.error') == 1 and len(requests) == count
    assert evaluate(first, 'askFrame(true)') is True
    wait(lambda: len(requests) > count, 'cross-site permission callback')
    child = requests[-1][1]
    assert child['requestingOrigin'] == origin.replace('127.0.0.1', 'localhost') and child['topOrigin'] == origin
    assert child['userGesture'] is False
    wait(lambda: evaluate(first, '!!frameResult'), 'cross-site denial')
    assert evaluate(first, 'frameResult.error') == 1
    rule(first, GRANTED, scope='*')
    count = len(requests)
    assert evaluate(first, 'askFrame(false)') is True
    wait(lambda: evaluate(first, '!!frameResult'), 'explicit grant still respects frame policy')
    assert evaluate(first, 'frameResult.error') == 1 and len(requests) == count
    rule(first, DENIED)
    assert state(first) == 'denied'
    reset(first)
    assert state(first) == 'granted'
    reset(first, '*')
    assert state(first) == 'prompt'
    rule(first, GRANTED, 'midi-sysex')
    assert evaluate(first, "permissionState('midi')") == evaluate(first, "permissionState('midi',{sysex:true})") == 'granted'
    rule(first, DENIED, 'midi')
    assert evaluate(first, "permissionState('midi')") == evaluate(first, "permissionState('midi',{sysex:true})") == 'denied'
    reset(first)
    rule(first, DENIED)
    call(first, 'BrowserContext.grantPermissions', {'permissions': ['geolocation'], 'origin': origin})
    assert state(first) == state(other) == 'granted'
    assert get_rule(first._check(), origin.encode(), b'geolocation') == GRANTED
    rule(first, DENIED)
    assert state(first) == 'denied' and state(other) == 'granted'
    # A client clearing its overrides does not lift the view's own rule.
    call(first, 'BrowserContext.clearPermissions', {})
    assert state(first) == 'denied' and state(other) == 'prompt'
    # An exact rule superseded by an override must not hide a newer default rule.
    call(first, 'BrowserContext.grantPermissions', {'permissions': ['geolocation'], 'origin': origin})
    assert state(first) == state(other) == 'granted'
    rule(first, DENIED, scope='*')
    assert state(first) == 'denied' and state(other) == 'granted'
    reset(first)
    reset(first, '*')
    call(first, 'BrowserContext.clearPermissions', {})
    assert state(first) == state(other) == 'prompt'
    rule(first, GRANTED, scope='*')
    assert state(first) == 'granted' and state(other) == 'prompt'
    reset(first, '*')
    assert state(first) == 'prompt'
    zebkit.cdp_listen('127.0.0.1', 0)
    zebkit_cdp_url = zebkit.cdp_url()
    rule(first, DENIED)
    cdp('Browser.setPermission', {'permission':{'name':'geolocation'}, 'setting':'granted', 'origin':origin})
    assert state(first) == state(other) == 'granted'
    rule(first, DENIED)
    assert state(first) == 'denied' and state(other) == 'granted'
    cdp('Browser.setPermission', {'permission':{'name':'geolocation'}, 'setting':'denied', 'origin':origin})
    assert state(first) == state(other) == 'denied'
    call(first, 'BrowserContext.setGeolocation', {'geolocation': {'latitude': 12.5, 'longitude': 23.75, 'accuracy': 1}})
    mode = 'hold'
    rule(first, PROMPT)
    token = ask(first)
    cdp('Browser.setPermission', {'permission':{'name':'geolocation'}, 'setting':'granted', 'origin':origin})
    assert geo_done(first) == {'latitude':12.5, 'longitude':23.75}
    wait(lambda: answer_for(token) is not None)
    assert answer_for(token)['reason'] == 'policy-changed' and not respond(token, DENIED, False)
    cdp('Browser.resetPermissions')
    assert state(first) == state(other) == 'prompt'
    zebkit.cdp_stop()
    protocol_client.sock.close()
    protocol_client = None
    victim = zebkit.View()
    views[victim._check()] = victim
    load(victim, 'close-in-handler')
    install(victim, 'victim')
    mode = 'close'
    before = len(requests)
    assert evaluate(victim, 'askGeo()') is True
    wait(lambda: len(requests) > before, 'close callback')
    wait(lambda: victim._closed, 'asynchronous close completes')
    assert not respond(requests[-1][0], GRANTED, False) and info(requests[-1][0]) is None
    # Fake capture devices exercise getUserMedia without granting via fake UI.
    media_script = """(async () => {
      try {
        const stream = await navigator.mediaDevices.getUserMedia({audio:true, video:true});
        const kinds = stream.getTracks().map(t => t.kind).sort();
        stream.getTracks().forEach(t => t.stop());
        return {kinds};
      } catch (error) { return {error:error.name}; }
    })()"""
    # On macOS 27 a fake microphone's getUserMedia never settles, in Chrome 156 as
    # here, so a grant is seen through the camera; a refusal needs no capture.
    granted_script, granted_kinds, asked = media_script, ['audio', 'video'], {'camera', 'microphone'}
    if sys.platform == 'darwin':
        granted_script, granted_kinds, asked = media_script.replace('audio:true, ', ''), ['video'], {'camera'}
    rule(first, GRANTED, 'camera')
    rule(first, GRANTED, 'microphone')
    assert evaluate(first, granted_script) == {'kinds':granted_kinds}
    assert evaluate(other, media_script) == {'error':'NotAllowedError'}
    reset(first)
    mode = 'grant-once'
    before = len(requests)
    assert evaluate(first, granted_script) == {'kinds':granted_kinds}
    assert {event['permission'] for _, event, _ in requests[before:]} == asked
    assert first.get_permission('camera', origin) == first.get_permission('microphone', origin) == 'prompt'
    mode = 'deny'
    assert evaluate(first, media_script) == {'error':'NotAllowedError'}
    # Public Python bindings exercise the same native path without ctypes.
    reset(first, None)
    first.set_permission('geolocation', 'denied', origin)
    assert first.get_permission('geolocation', origin) == 'denied'
    first.reset_permissions(origin)
    first.set_permission_timeout(5000)
    for bad in (0, 300001, -1, True, 1.5):
        try:
            first.set_permission_timeout(bad)
            raise AssertionError('invalid timeout accepted')
        except ValueError:
            pass
    try:
        first.set_permission('geolocation\0notifications', 'granted', origin)
        raise AssertionError('NUL permission accepted')
    except ValueError:
        pass
    public_requests, public_results = [], []
    def public_prompt(request):
        public_requests.append(request)
        assert request.info == request.metadata
        assert request.respond('denied')
        assert not request.respond('granted') and request.info is None
        first.on_permission(None)  # Removing the currently executing trampoline is safe.
    first.on_permission_result(lambda event: public_results.append(event))
    first.on_permission(public_prompt)
    assert evaluate(first, 'askGeo()') is True
    assert geo_done(first) == {'error': 1}
    wait(lambda: bool(public_requests) and bool(public_results), 'public Python permission bindings')
    assert public_results[-1]['id'] == public_requests[-1].metadata['id']
    assert public_results[-1]['reason'] == 'answered'
    first.on_permission_result(None)
    on_permission(first._check(), PERMISSION(), None)
    assert evaluate(first, "permissionState('notifications')") == 'denied'
    print('ok native permission hooks: real requests, fake coordinates, rules/subscriptions, once/remember, deadlines, navigation, OOPIF, handler replacement and actor ordering', flush=True)
finally:
    assert zebkit.shutdown()
assert not errors, errors
assert not respond(None, GRANTED, False) and request_info(None) is None
