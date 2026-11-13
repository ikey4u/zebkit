"""A view's user agent and client hints through the Python binding's
View.set_user_agent_override and View.set_user_agent, one mode an engine; run
by user_agent.py, which has the modes and the server (ZEBKIT_UA_*).

    python3 tests/engine/user_agent_python.py [engine switches...]
"""

import os
import sys
from concurrent.futures import ThreadPoolExecutor

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from harness import Cdp, check, ev, finish, must_wait, part, wait, zebkit  # noqa: E402

PAGE, BASE, MODE = os.environ['ZEBKIT_UA_TEST_URL'], os.environ['ZEBKIT_UA_BASE'], os.environ['ZEBKIT_UA_CASE']
VALID = {'userAgent': 'zebkit-native-hints/1', 'acceptLanguage': 'en-US', 'platform': 'NativePlatform',
         'userAgentMetadata': {'brands': [{'brand': 'Native', 'version': '1'}],
                               'fullVersionList': [{'brand': 'Native', 'version': '1.0'}],
                               'platform': 'Linux', 'platformVersion': '1', 'architecture': 'x86',
                               'model': 'NativeModel', 'mobile': False, 'fullVersion': '1.0', 'bitness': '64',
                               'wow64': False, 'formFactors': ['Desktop']}}
STATE = ("Promise.all([fetch('/headers').then(r=>r.json()),navigator.userAgentData.getHighEntropyValues("
         "['platformVersion','architecture','model','bitness','wow64','formFactors','fullVersionList','uaFullVersion'])])"
         ".then(([headers,hints])=>({headers,hints,ua:navigator.userAgent,platform:navigator.platform}))")


class Watched:
    def __init__(self):
        self.view = zebkit.View(None, 320, 200)
        self.closed, self.loads = [], []
        self.view.on_close(lambda: self.closed.append(True))
        self.view.on_load(lambda event, url: self.loads.append((event, url)))

    def override(self, options, then=None):
        """set_user_agent_override(): whether it was queued, and its answers."""
        answers = []

        def done(value, error):
            answers.append((value, error))
            if then:
                then(value, error)
        return self.view.set_user_agent_override(options, done), answers

    def at(self, url):
        must_wait(lambda: (zebkit.LOAD_FINISHED, url) in self.loads, url)


def state(view):
    value = ev(view, STATE)
    assert isinstance(value, dict), value
    # The Referer changes with each page; only what the agent decides is compared.
    value['headers'] = {key: content for key, content in value['headers'].items()
                        if key.lower() in ('user-agent', 'accept-language') or key.lower().startswith('sec-ch-ua')}
    return value


def hinted(view):
    """The override, in the page and in what it sends: the first script, the
    network, the high-entropy hints."""
    first = ev(view, 'firstUA')
    check(first['ua'] == VALID['userAgent'] and first['brands'] == VALID['userAgentMetadata']['brands'],
          'the first script has the override', first)
    before = state(view)
    check(before['ua'] == before['headers']['User-Agent'] == VALID['userAgent'] and before['platform'] == 'NativePlatform',
          'navigator and the network: its agent and platform', before)
    hints = before['hints']
    check(hints['architecture'] == 'x86' and hints['platformVersion'] == '1' and hints['uaFullVersion'] == '1.0'
          and all(hints[key] == VALID['userAgentMetadata'][key]
                  for key in ('model', 'bitness', 'wow64', 'formFactors', 'fullVersionList')),
          'its high-entropy hints', hints)
    headers = {key.lower(): value for key, value in before['headers'].items()}
    check(headers.get('sec-ch-ua-arch') == '"x86"' and headers.get('sec-ch-ua-model') == '"NativeModel"'
          and headers.get('sec-ch-ua-bitness') == '"64"' and headers.get('sec-ch-ua-wow64') == '?0',
          'its client hints headers', headers)
    return before


def main():
    if zebkit.init(['user-agent-python', *sys.argv[1:]], settings={'logging': {'destination': 'none'}}) != 0:
        print('FAIL zebkit.init()')
        return 1
    try:
        if MODE == 'cdp-scope':
            return scope()
        watched = Watched()
        view = watched.view
        if MODE != 'completion':
            queued, answers = watched.override(VALID)
            check(queued, 'the override queued')
            # The load waits for it.
            view.load(PAGE)
            with part('the first load, after the override completed', fatal=True):
                watched.at(PAGE)
                assert answers == [({}, None)], answers
            before = hinted(view)
        if MODE == 'worker':
            worker = ev(view, "new Promise((resolve,reject)=>{const w=new Worker('/worker.js');"
                              "w.onmessage=e=>{w.terminate();resolve(e.data)};w.onerror=reject})")
            check(worker['ua'] == VALID['userAgent'] and all(before['hints'][k] == v for k, v in worker['hints'].items()),
                  'a worker has the agent and the hints', worker)
        elif MODE == 'rejection':
            invalid = [{**VALID, 'userAgent': 'bad\r\nagent'}, {**VALID, 'acceptLanguage': 'bad\nlanguage'},
                       {**VALID, 'userAgent': 'changed',
                        'userAgentMetadata': {**VALID['userAgentMetadata'], 'platform': 'bad\nplatform'}},
                       {**VALID, 'userAgent': '', 'userAgentMetadata': VALID['userAgentMetadata']},
                       {'userAgent': 42}, {'userAgent': 'changed', 'unknown': True}, '{', 'x' * 65537]
            for index, options in enumerate(invalid):
                # zebkit.h's rule: a live view takes the call (True) and tells
                # exactly one error, later; nothing of it is kept.
                with part('invalid override %d: True, exactly one error answer, nothing of it kept' % index):
                    queued, answers = watched.override(options)
                    assert queued is True, 'refused at once (%r) on a live view' % (queued,)
                    must_wait(lambda: answers, 'the error answer')
                    wait(lambda: len(answers) > 1, .3)
                    assert len(answers) == 1 and answers[0][0] is None and answers[0][1], answers
                    assert state(view) == before, state(view)
                    url = BASE + '/after-rejection-%d' % index
                    view.load(url)
                    watched.at(url)
                    assert ev(view, 'navigator.userAgent') == VALID['userAgent']
            # False, and never told, only where the engine cannot tell: a closed
            # view (the binding may refuse it itself, with its Error).
            closed = Watched()
            closed.view.close()
            # The closing is posted: the view is closed once on_close is told.
            must_wait(lambda: closed.closed, 'the other view closed')
            told = []
            try:
                queued = closed.view.set_user_agent_override(VALID, lambda value, error: told.append((value, error)))
            except zebkit.Error:
                queued = False
            wait(lambda: told, .3)
            check(queued is False and not told, 'a closed view: False, never told', (queued, told))
        elif MODE == 'completion':
            completion(watched)
        for each in [watched]:
            if not each.closed:
                each.view.close()
            check(wait(lambda: each.closed), 'the view closes')
    finally:
        zebkit.shutdown()
    return finish()


def completion(watched):
    queued, answers = watched.override(VALID)
    check(queued and wait(lambda: answers) and answers == [({}, None)], 'the override completes with {}', answers)
    queued, cleared = watched.override({'userAgent': ''})
    check(queued and wait(lambda: cleared) and cleared == [({}, None)], 'cleared with an empty agent', cleared)
    agent = ev(watched.view, 'navigator.userAgent')
    check(isinstance(agent, str) and agent != VALID['userAgent'], 'the engine\'s agent again', agent)
    # A load from the completion supersedes the one waiting for it.
    redirected = Watched()
    errors = []
    queued, _ = redirected.override(VALID, lambda value, error: errors.append(error) if error
                                    else redirected.view.load(BASE + '/from-completion'))
    redirected.view.load(BASE + '/superseded-deferred')
    with part('a load from the completion supersedes the waiting one'):
        redirected.at(BASE + '/from-completion')
        assert queued and not errors, errors
        assert not any(url == BASE + '/superseded-deferred' for event, url in redirected.loads
                       if event == zebkit.LOAD_FINISHED), redirected.loads
    redirected.view.close()
    closing = Watched()
    queued, closing_answers = closing.override(VALID, lambda value, error: closing.view.close())
    check(queued and wait(lambda: closing.closed) and closing_answers == [({}, None)],
          'the view closed from the completion', closing_answers)
    cancelled = Watched()
    queued, cancelled_answers = cancelled.override(VALID)
    cancelled.view.close()
    check(queued and wait(lambda: cancelled.closed) and len(cancelled_answers) == 1
          and (cancelled_answers[0] == ({}, None) or (cancelled_answers[0][0] is None and cancelled_answers[0][1])),
          'closed while one is pending: answered once, before the close', cancelled_answers)
    check(wait(lambda: redirected.closed), 'the other views close')


def scope():
    """The binding's set_user_agent and a CDP client's override: Chromium keeps
    the client's in its session, the binding's comes back once it detaches."""
    with ThreadPoolExecutor(max_workers=1) as pool:
        def external(function):
            future = pool.submit(function)
            must_wait(future.done, 'the CDP command', 30)
            return future.result()
        check(zebkit.cdp_listen('127.0.0.1', 0), 'CDP served')
        watched = Watched()
        view = watched.view
        view.set_user_agent('zebkit-native-before/1')
        view.load(BASE + '/initial')
        cdp = None
        with part('the binding\'s and the CDP client\'s overrides, each in its scope'):
            watched.at(BASE + '/initial')
            endpoint, target = zebkit.cdp_url(), view.target_id()
            cdp = external(lambda: Cdp(endpoint))
            session = external(lambda: cdp.call('Target.attachToTarget', {'targetId': target, 'flatten': True}))['sessionId']
            external(lambda: cdp.call('Emulation.setUserAgentOverride', {'userAgent': 'zebkit-external/2'},
                                      session=session))

            def navigate(path, expected):
                view.load(BASE + path)
                watched.at(BASE + path)
                snapshot = ev(view, '({current:navigator.userAgent,first:window.firstUserAgent})')
                assert snapshot == {'current': expected, 'first': expected}, (path, snapshot)
            assert ev(view, 'navigator.userAgent') == 'zebkit-external/2'
            navigate('/external', 'zebkit-external/2')
            view.set_user_agent('zebkit-native-after/3')
            # A later call of the binding's does not remove the session's override.
            navigate('/native-while-external', 'zebkit-external/2')
            external(lambda: cdp.call('Target.detachFromTarget', {'sessionId': session}))
            navigate('/detached', 'zebkit-native-after/3')
        if cdp:
            cdp.close()
        view.close()
        check(wait(lambda: watched.closed), 'the view closes')
        zebkit.cdp_stop()
    return finish()


if __name__ == '__main__':
    sys.exit(main())
