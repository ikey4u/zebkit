"""Native permission decisions use real Chromium requests, with synthetic location:
View.set_permission()'s rules and the page's subscriptions, View.on_permission()
answered once or remembered, denied, held past its deadline, replaced, cancelled
by a navigation, asked from a frame of another site; against the rules of
Playwright's BrowserContext and of a CDP client (Browser.setPermission); camera
and microphone with fake devices.

    python3 tests/engine/permission_hooks.py <base URL> [engine switches...]  (--use-fake-device-for-media-stream)

Exit 0 when every check passes. The C API's own cases of a request that is none
are tried on the C functions themselves, at the end; the rule that a view's
functions do nothing from another thread is tests/engine/view_lifetime.py's.
"""
import ctypes
import os
import sys
import time
import traceback
import threading
from urllib.parse import urlsplit

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from harness import Cdp, check, ev, finish, must_wait, part, zebkit  # noqa: E402
from zebkit import automation  # noqa: E402

base = sys.argv[1]
origin = '{0.scheme}://{0.netloc}'.format(urlsplit(base))
if zebkit.init([sys.argv[0], *sys.argv[2:]], settings={'logging': {'destination': 'none'}}) != 0:
    print('FAIL zebkit.init()')
    sys.exit(1)
requests, outcomes, late = [], [], []
pages = {}
mode = 'hold'
protocol_client = None
zebkit_cdp_url = None


def wait(condition, message='condition', timeout=15):
    must_wait(condition, message, timeout)


def evaluate(view, code):
    """ev(), raising when the script threw or did not answer."""
    value = ev(view, code)
    assert not (isinstance(value, tuple) and value[:1] in (('error',), ('timeout',))), (code, value)
    return value


def load(view, tag):
    loaded = []
    view.on_load(lambda event, url: loaded.append(True) if event == zebkit.LOAD_FINISHED and url.startswith(base + '/permissions.html') else None)
    view.load(base + '/permissions.html?' + tag)
    wait(lambda: bool(loaded), 'permission document load')


def page(view):
    """The view as Playwright's Page (zebkit.automation), once."""
    if view not in pages:
        pages[view] = automation.Page(view)
    return pages[view]


def done(coroutine, what):
    """The value of a coroutine of zebkit.automation, run to its end."""
    future = automation.run(coroutine)
    wait(future.done, what)
    return future.result()


def geolocation(view):
    done(page(view).set_geolocation({'latitude': 12.5, 'longitude': 23.75, 'accuracy': 1}), 'setGeolocation')


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


def rule(view, state, name='geolocation', scope=origin, valid=True):
    try:
        view.set_permission(name, state, scope)
        taken = True
    except (zebkit.Error, ValueError):
        taken = False
    assert taken == valid, (scope, name, state, taken)


def reset(view, scope=origin):
    view.reset_permissions(scope)


def state(view):
    return evaluate(view, "permissionState('geolocation')")


def answer_for(request):
    return next((event for event in outcomes if event['id'] == request.metadata['id']), None)


def install(view, tag='first'):
    def asked(request):
        event = request.metadata
        assert event['state'] == 'pending', event
        assert request.info == event, (request.info, event)
        requests.append((request, event, tag))
        if mode == 'grant-once':
            assert request.respond('granted', False)
        elif mode == 'remember':
            assert request.respond('granted', True)
        elif mode == 'deny':
            assert request.respond('denied', False)
        elif mode == 'late':
            time.sleep(.06)  # Deliberately block the host callback past its deadline.
            late.append(request.respond('granted', False))
        elif mode == 'close':
            view.close()
            # Closing is posted so page callbacks can unwind safely.
    view.on_permission(asked)


def ask(view):
    before = len(requests)
    assert evaluate(view, 'askGeo()') is True
    wait(lambda: len(requests) > before, 'native permission callback')
    return requests[-1][0]


def geo_done(view):
    wait(lambda: evaluate(view, "geoResult!==null && geoResult!=='pending'"), 'geolocation completion')
    return evaluate(view, 'geoResult')


def c_api():
    """The C functions of a permission request, from the engine's library itself
    (ZEBKIT_DIR): for what only C can pass them, a request that is none."""
    names = {'darwin': 'Zebkit.framework/Zebkit', 'win32': 'zebkit.dll'}
    library = ctypes.CDLL(os.path.join(os.environ['ZEBKIT_DIR'], names.get(sys.platform, 'libzebkit.so.1')))
    library.zebkit_permission_respond.restype = ctypes.c_bool
    library.zebkit_permission_respond.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_bool]
    library.zebkit_permission_request_info.restype = ctypes.c_void_p
    library.zebkit_permission_request_info.argtypes = [ctypes.c_void_p]
    return library


try:
    first, other = zebkit.View(), zebkit.View()
    with part('the pages loaded, with a location of the test\'s (BrowserContext.setGeolocation)', fatal=True):
        for view in (first, other):
            load(view, 'initial')
            geolocation(view)
        assert state(first) == state(other) == 'prompt'
    with part('a rule of the view\'s: told to the page\'s subscription, the view\'s alone'):
        assert evaluate(first, 'listenGeo()') == 'prompt'
        rule(first, 'denied')
        wait(lambda: evaluate(first, "permissionEvents.includes('denied')"), 'native permission subscription denied')
        assert state(first) == 'denied' and state(other) == 'prompt'
    with part('a granted rule: the page has the location, and get_permission() says granted'):
        rule(first, 'granted')
        assert evaluate(first, 'askGeo()') is True
        assert geo_done(first) == {'latitude': 12.5, 'longitude': 23.75}
        assert first.get_permission('geolocation', origin) == 'granted'
    with part('invalid rules refused (an opaque origin, an unknown name or state), the rule kept'):
        for scope, name, value in [('data:text/plain,opaque', 'geolocation', 'denied'), (origin, 'unknown', 'granted'),
                                   (origin, 'geolocation', 'maybe'), (origin, 'geolocation-approximate', 'granted')]:
            rule(first, value, name, scope, valid=False)
            assert state(first) == 'granted'
        reset(first)
        assert state(first) == 'prompt'
    with part('set_permission_timeout() refuses 0 and 300001'):
        for bad in (0, 300001):
            try:
                first.set_permission_timeout(bad)
                raise AssertionError('accepted %d' % bad)
            except ValueError:
                pass
    first.on_permission_result(lambda event: outcomes.append(event))
    install(first)
    with part('granted once: the page has it, nothing remembered'):
        mode = 'grant-once'
        token = ask(first)
        assert geo_done(first) == {'latitude': 12.5, 'longitude': 23.75}
        wait(lambda: answer_for(token) is not None)
        assert answer_for(token)['reason'] == 'answered' and not answer_for(token)['remembered']
        assert state(first) == state(other) == 'prompt'
        assert token.info is None and not token.respond('granted', True)
    with part('granted and remembered: a rule of the view\'s, not asked again'):
        mode = 'remember'
        token = ask(first)
        assert geo_done(first)['latitude'] == 12.5
        wait(lambda: answer_for(token) is not None)
        assert answer_for(token)['remembered'] and state(first) == 'granted' and state(other) == 'prompt'
        count = len(requests)
        assert evaluate(first, 'askGeo()') is True
        assert geo_done(first)['longitude'] == 23.75 and len(requests) == count
        reset(first)
    with part('a held request: an unknown state refused, and from another thread nothing', fatal=True):
        mode = 'hold'
        token = ask(first)
        try:
            token.respond('maybe', False)
            raise AssertionError('an unknown state accepted')
        except ValueError:
            pass
        assert token.info is not None
        wrong_thread = []
        worker = threading.Thread(target=lambda: wrong_thread.append((token.respond('granted', False), token.info)))
        worker.start()
        worker.join(timeout=3)
        assert not worker.is_alive() and wrong_thread == [(False, None)], wrong_thread
        assert token.info is not None
    with part('a rule set while it is held settles it: policy-changed'):
        rule(first, 'denied')
        assert geo_done(first) == {'error': 1}
        wait(lambda: answer_for(token) is not None)
        assert answer_for(token)['reason'] == 'policy-changed' and not token.respond('granted', False)
        reset(first)
    with part('the deadline: an unanswered request is refused at it, told as timeout'):
        first.set_permission_timeout(40)
        token = ask(first)
        assert geo_done(first) == {'error': 1}
        wait(lambda: answer_for(token) is not None)
        assert answer_for(token)['reason'] == 'timeout' and token.info is None
        assert not token.respond('granted', False)
    with part('a reply after the deadline, from a callback that took longer, is refused'):
        mode = 'late'
        first.set_permission_timeout(20)
        token = ask(first)
        assert geo_done(first) == {'error': 1} and late[-1] is False
        wait(lambda: answer_for(token) is not None)
        assert answer_for(token)['reason'] == 'timeout'
        first.set_permission_timeout(5000)
    with part('a new function settles the held request: handler-changed'):
        mode = 'hold'
        token = ask(first)
        install(first, 'replacement')
        assert geo_done(first) == {'error': 1}
        wait(lambda: answer_for(token) is not None)
        assert answer_for(token)['reason'] == 'handler-changed' and not token.respond('granted', False)
    with part('a navigation cancels the held request; the next one is another'):
        token = ask(first)
        load(first, 'navigation')
        wait(lambda: answer_for(token) is not None, 'navigation cancels pending permission')
        assert answer_for(token)['reason'] == 'cancelled' and token.info is None
        replacement = ask(first)
        assert replacement.metadata['id'] != token.metadata['id'] and not token.respond('granted', False)
        assert replacement.info
        assert replacement.respond('denied', False) and geo_done(first) == {'error': 1}
    with part('a frame the permissions policy does not allow is refused without asking'):
        mode = 'deny'
        count = len(requests)
        assert evaluate(first, 'askFrame(false)') is True
        wait(lambda: evaluate(first, '!!frameResult'), 'permissions policy frame denial')
        assert evaluate(first, 'frameResult.error') == 1 and len(requests) == count
    with part('a frame of another site that may ask: its origin and the top\'s told, denied'):
        assert evaluate(first, 'askFrame(true)') is True
        wait(lambda: len(requests) > count, 'cross-site permission callback')
        child = requests[-1][1]
        assert child['requestingOrigin'] == origin.replace('127.0.0.1', 'localhost') and child['topOrigin'] == origin
        assert child['userGesture'] is False
        wait(lambda: evaluate(first, '!!frameResult'), 'cross-site denial')
        assert evaluate(first, 'frameResult.error') == 1
    with part('a default rule granting still respects the frame\'s policy'):
        rule(first, 'granted', scope='*')
        count = len(requests)
        assert evaluate(first, 'askFrame(false)') is True
        wait(lambda: evaluate(first, '!!frameResult'), 'explicit grant still respects frame policy')
        assert evaluate(first, 'frameResult.error') == 1 and len(requests) == count
    with part('an origin\'s rule over the default rule; each reset by its scope'):
        rule(first, 'denied')
        assert state(first) == 'denied'
        reset(first)
        assert state(first) == 'granted'
        reset(first, '*')
        assert state(first) == 'prompt'
    with part('MIDI with and without sysex follow midi-sysex and midi'):
        rule(first, 'granted', 'midi-sysex')
        assert evaluate(first, "permissionState('midi')") == evaluate(first, "permissionState('midi',{sysex:true})") == 'granted'
        rule(first, 'denied', 'midi')
        assert evaluate(first, "permissionState('midi')") == evaluate(first, "permissionState('midi',{sysex:true})") == 'denied'
        reset(first)
    with part('Playwright\'s grantPermissions and clearPermissions against the view\'s rules'):
        rule(first, 'denied')
        done(page(first).grant_permissions(['geolocation'], origin), 'grantPermissions')
        assert state(first) == state(other) == 'granted'
        assert first.get_permission('geolocation', origin) == 'granted'
        rule(first, 'denied')
        assert state(first) == 'denied' and state(other) == 'granted'
        # A client clearing its overrides does not lift the view's own rule.
        done(page(first).clear_permissions(), 'clearPermissions')
        assert state(first) == 'denied' and state(other) == 'prompt'
        # An exact rule superseded by an override must not hide a newer default rule.
        done(page(first).grant_permissions(['geolocation'], origin), 'grantPermissions')
        assert state(first) == state(other) == 'granted'
        rule(first, 'denied', scope='*')
        assert state(first) == 'denied' and state(other) == 'granted'
        reset(first)
        reset(first, '*')
        done(page(first).clear_permissions(), 'clearPermissions')
        assert state(first) == state(other) == 'prompt'
        rule(first, 'granted', scope='*')
        assert state(first) == 'granted' and state(other) == 'prompt'
        reset(first, '*')
        assert state(first) == 'prompt'
    with part('a CDP client\'s Browser.setPermission against the view\'s rules, and settling a held request'):
        zebkit.cdp_listen('127.0.0.1', 0)
        zebkit_cdp_url = zebkit.cdp_url()
        rule(first, 'denied')
        cdp('Browser.setPermission', {'permission':{'name':'geolocation'}, 'setting':'granted', 'origin':origin})
        assert state(first) == state(other) == 'granted'
        rule(first, 'denied')
        assert state(first) == 'denied' and state(other) == 'granted'
        cdp('Browser.setPermission', {'permission':{'name':'geolocation'}, 'setting':'denied', 'origin':origin})
        assert state(first) == state(other) == 'denied'
        geolocation(first)
        mode = 'hold'
        rule(first, 'prompt')
        token = ask(first)
        cdp('Browser.setPermission', {'permission':{'name':'geolocation'}, 'setting':'granted', 'origin':origin})
        assert geo_done(first) == {'latitude':12.5, 'longitude':23.75}
        wait(lambda: answer_for(token) is not None)
        assert answer_for(token)['reason'] == 'policy-changed' and not token.respond('denied', False)
        cdp('Browser.resetPermissions')
        assert state(first) == state(other) == 'prompt'
    zebkit.cdp_stop()
    if protocol_client is not None:
        protocol_client.close()
        protocol_client = None
    with part('the view closed from its own permission function: the request settled'):
        victim = zebkit.View()
        victim_closed = []
        victim.on_close(lambda: victim_closed.append(True))
        load(victim, 'close-in-handler')
        install(victim, 'victim')
        mode = 'close'
        before = len(requests)
        assert evaluate(victim, 'askGeo()') is True
        wait(lambda: len(requests) > before, 'close callback')
        wait(lambda: victim_closed, 'asynchronous close completes')
        assert not requests[-1][0].respond('granted', False) and requests[-1][0].info is None
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
    with part('camera and microphone by rule: this view\'s, not the other\'s'):
        rule(first, 'granted', 'camera')
        rule(first, 'granted', 'microphone')
        assert evaluate(first, granted_script) == {'kinds':granted_kinds}
        assert evaluate(other, media_script) == {'error':'NotAllowedError'}
        reset(first)
    with part('camera and microphone asked, granted once, denied'):
        mode = 'grant-once'
        before = len(requests)
        assert evaluate(first, granted_script) == {'kinds':granted_kinds}
        assert {event['permission'] for _, event, _ in requests[before:]} == asked
        assert first.get_permission('camera', origin) == first.get_permission('microphone', origin) == 'prompt'
        mode = 'deny'
        assert evaluate(first, media_script) == {'error':'NotAllowedError'}
    with part('the public rules: set, read, reset; invalid deadlines and names refused'):
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
    with part('a function that denies and removes itself; its result told'):
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
        first.on_permission(None)
    # Notifications are undecided without a decision (docs/SYSTEM_INTEGRATION.md, 12).
    check(evaluate(first, "permissionState('notifications')") == 'prompt', 'notifications are undecided without a decision')
finally:
    check(zebkit.shutdown(), 'zebkit.shutdown()')
with part('the C API: a request that is none (NULL, or no request) has no info and takes no reply'):
    library = c_api()
    for nothing in (None, ctypes.c_void_p(-1)):
        assert not library.zebkit_permission_respond(nothing, 1, False), nothing
        assert library.zebkit_permission_request_info(nothing) is None, nothing
sys.exit(finish())
