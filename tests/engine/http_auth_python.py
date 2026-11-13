"""HTTP authentication through the Python binding's View.on_http_auth and
HttpAuthRequest, one mode an engine; run by http_auth.py, which has the modes
and the server (ZEBKIT_HTTP_AUTH_*).

    python3 tests/engine/http_auth_python.py [engine switches...]
"""

import os
import sys
import threading

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from harness import check, ev, finish, must_wait, part, wait, zebkit  # noqa: E402

env = os.environ
BASE, USER, PASSWORD = env['ZEBKIT_HTTP_AUTH_URL'], env['ZEBKIT_HTTP_AUTH_USER'], env['ZEBKIT_HTTP_AUTH_PASSWORD']
MODE, REALM, PORT = env['ZEBKIT_HTTP_AUTH_CASE'], env['ZEBKIT_HTTP_AUTH_REALM'], int(env['ZEBKIT_HTTP_AUTH_PORT'])
PROXY = env.get('ZEBKIT_HTTP_AUTH_PROXY') == '1'
HOLD = ('invalid', 'timeout', 'stop', 'navigation', 'remove', 'replace')


def rejected(function, kinds):
    try:
        function()
    except kinds:
        return True
    return False


def main():
    if zebkit.init(['http-auth-python', *sys.argv[1:]], settings={'logging': {'destination': 'none'}}) != 0:
        print('FAIL zebkit.init()')
        return 1
    try:
        view = zebkit.View()
        requests, outcomes, loads, closed = [], [], [], []
        view.on_load(lambda event, url: loads.append(event))
        view.on_close(lambda: closed.append(True))
        check(all(rejected(lambda: view.set_http_auth_timeout(bad), ValueError) for bad in (0, 300001, True, 1.5)),
              'set_http_auth_timeout refuses 0, 300001, True and 1.5')
        check(all(rejected(lambda: setter(1), TypeError) for setter in (view.on_http_auth, view.on_http_auth_result)),
              'on_http_auth and on_http_auth_result refuse what is not callable')
        view.set_http_auth_timeout(50 if MODE == 'timeout' else 5000)

        def challenge(request):
            requests.append(request)
            metadata = request.metadata
            first = len(requests) == 1
            if first:
                check(isinstance(request, zebkit.HttpAuthRequest) and request.info == metadata,
                      'the request: its info is its metadata', None if request.info == metadata else metadata)
                check(metadata['realm'] == REALM and metadata['scheme'] == 'basic' and metadata['proxy'] == PROXY
                      and metadata['port'] == PORT, 'the metadata: realm, scheme, proxy and the challenger\'s port',
                      None if metadata['realm'] == REALM and metadata['port'] == PORT else metadata)
                check(USER not in str(metadata) and PASSWORD not in str(metadata), 'no credentials in the metadata')
                bad = [(None, 'x'), (1, 'x'), ('user', None), ('bad\0', 'x'), ('x' * 65537, 'x')]
                check(all(rejected(lambda: request.respond(u, p), (TypeError, ValueError)) for u, p in bad)
                      and request.info is not None, 'invalid credentials are refused, and the request stays')
            if MODE in HOLD:
                return
            if MODE == 'cancel-handler':
                view.on_http_auth(None)
                check(request.info is None and not request.respond(USER, PASSWORD),
                      'removed from inside it: the request is gone')
                return
            if MODE == 'close-request':
                view.close()
                return
            if MODE == 'deny':
                check(request.respond(), 'cancelled with no credentials')
                return
            user, password = USER, PASSWORD
            if MODE == 'retry' and metadata['firstAttempt']:
                user, password = 'wrong-user', 'wrong-password'
            ok = request.respond(user, password)
            if first or MODE != 'retry':
                check(ok and request.info is None and not request.respond(),
                      'answered once: the request is gone, a second reply refused')

        def result(metadata):
            outcomes.append(metadata)
            check(PASSWORD not in str(metadata), 'no password in the outcome')
            if MODE == 'close-result':
                view.on_http_auth_result(None)
                view.close()

        if MODE != 'default':
            view.on_http_auth(challenge)
        view.on_http_auth_result(result)
        path = {'fetch': '/public', 'frame': '/public-frame'}.get(MODE, '/auth/python-' + MODE)
        view.load(BASE + path)
        denied = lambda: zebkit.LOAD_FAILED in loads or view.title == 'auth-denied'  # noqa: E731

        with part('python %s: the page as the mode makes it' % MODE):
            if MODE in ('close-request', 'close-result'):
                must_wait(lambda: closed, 'the view closed from the callback')
            elif MODE in ('default', 'deny', 'cancel-handler'):
                must_wait(denied, 'the challenge cancelled')
                if MODE == 'default':
                    assert not requests, requests
                else:
                    must_wait(lambda: outcomes, 'the outcome')
                    assert outcomes[0]['state'] == 'cancelled', outcomes
            elif MODE in ('allow', 'retry', 'proxy'):
                must_wait(lambda: view.title == 'auth-ok' and outcomes, 'authenticated')
                assert outcomes[-1]['state'] == 'provided', outcomes
                if MODE == 'retry':
                    assert any(not r.metadata['firstAttempt'] for r in requests), [r.metadata for r in requests]
                if MODE == 'proxy':
                    assert any(r.metadata['proxy'] and r.metadata['mainFrame'] and r.metadata['navigation']
                               for r in requests), [r.metadata for r in requests]
            elif MODE == 'fetch':
                must_wait(lambda: view.title == 'public', 'the public page')
                answer = ev(view, "fetch('/auth/fetch').then(r => r.text())")
                assert isinstance(answer, str) and 'auth-ok' in answer, answer
                assert any(not r.metadata['navigation'] and not r.metadata['mainFrame'] for r in requests), \
                    [r.metadata for r in requests]
            elif MODE == 'frame':
                must_wait(lambda: view.title == 'public', 'the public page')
                must_wait(lambda: ev(view, 'childAuthenticated') is True, 'the frame authenticated')
                assert any(r.metadata['navigation'] and not r.metadata['mainFrame'] for r in requests), \
                    [r.metadata for r in requests]
            else:
                must_wait(lambda: requests, 'the request, held')
                request = requests[0]
                if MODE == 'invalid':
                    off_thread = []
                    thread = threading.Thread(target=lambda: off_thread.append((request.info, request.respond())))
                    thread.start()
                    thread.join()
                    assert off_thread == [(None, False)] and request.info, off_thread
                    assert request.respond(USER, PASSWORD)
                    must_wait(lambda: view.title == 'auth-ok', 'a valid reply after invalid ones')
                elif MODE == 'timeout':
                    must_wait(lambda: request.info is None, 'the deadline')
                    assert not request.respond(USER, PASSWORD)
                    must_wait(lambda: any(o['id'] == request.metadata['id'] and o['reason'] == 'timeout'
                                          for o in outcomes), 'the timeout outcome')
                else:
                    if MODE == 'stop':
                        view.stop()
                    elif MODE == 'navigation':
                        view.load('about:blank')
                    elif MODE == 'remove':
                        view.on_http_auth(None)
                    else:
                        view.on_http_auth(lambda other: None)
                    must_wait(lambda: request.info is None, 'the request cancelled')
                    if MODE in ('remove', 'replace'):
                        must_wait(lambda: any(o['reason'] == 'handler-changed' for o in outcomes),
                                  'the handler-changed outcome')
        check(all(r.info is None and not r.respond() for r in requests),
              'python %s: every request settled, none answerable' % MODE)
        if not closed:
            view.close()
            check(wait(lambda: closed), 'the view closes')
    finally:
        zebkit.shutdown()
    return finish()


if __name__ == '__main__':
    sys.exit(main())
