"""A view's user agent and client hints (zebkit_view_set_user_agent_override, and
CDP's Emulation.setUserAgentOverride) in every language, against a local server
that says what each request carried (language_modes.py says how):

    python3 tests/engine/user_agent.py c <user_agent>
    python3 tests/engine/user_agent.py python python3 -I -B tests/engine/user_agent_python.py
    python3 tests/engine/user_agent.py javascript <zebkit-js> tests/engine/script/user_agent.mjs
    python3 tests/engine/user_agent.py rust <the user_agent example>
    python3 tests/engine/user_agent.py cdp <zebkit-run>

The program is given ZEBKIT_UA_TEST_URL (the page it loads first),
ZEBKIT_UA_BASE (the server) and ZEBKIT_UA_CASE (the mode); zebkit-js the page's
URL. cdp is this driver itself, a CDP client of tools/zebkit-run. Exit 0 when
every check passes.
"""

import json
import os
import sys
import tempfile
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from harness import Cdp, check, finish, launch_engine, part  # noqa: E402
from language_modes import NAMES, arguments, modes, run  # noqa: E402

PYTHON_ONLY = {'c': 'not written for C', 'javascript': 'not written for JavaScript',
               'cdp': 'the binding\'s alone'}
# The modes, and the languages each is not written for.
MODES = [
    ('hints', {}),
    # (the override before the first load: agent, platform, hints, headers)
    ('rejection', {}),                      # invalid overrides refused, all of the state kept
    ('worker', dict(PYTHON_ONLY, cdp='not written for CDP')),   # a worker has the override too
    ('completion', PYTHON_ONLY),            # cleared, a navigation and a close from the completion
    ('cdp-scope', PYTHON_ONLY),             # the binding's override and a CDP client's, each in its scope
]
# Each program's override.
AGENTS = {'c': 'zebkit-binding-UA/1', 'javascript': 'zebkit-binding-UA/1', 'rust': 'zebkit-binding-UA/1',
          'python': 'zebkit-native-hints/1', 'cdp': 'Zebkit-UA-Before/1'}
HINTS = ('Sec-CH-UA-Full-Version-List, Sec-CH-UA-Platform-Version, Sec-CH-UA-Arch, Sec-CH-UA-Model, '
         'Sec-CH-UA-Bitness, Sec-CH-UA-WoW64, Sec-CH-UA-Form-Factors')


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def do_GET(self):
        self.server.received.append((self.path, dict(self.headers)))
        if self.path == '/headers':
            source, kind = json.dumps(dict(self.headers)), 'application/json'
        elif self.path == '/worker.js':
            source = ('navigator.userAgentData.getHighEntropyValues(["architecture","platformVersion","model",'
                      '"bitness","wow64","formFactors","fullVersionList","uaFullVersion"])'
                      '.then(h=>postMessage({ua:navigator.userAgent,hints:h}))')
            kind = 'application/javascript'
        else:
            source = ('<title>native-ua</title><script>window.firstUA={ua:navigator.userAgent,'
                      'brands:navigator.userAgentData.brands,platform:navigator.userAgentData.platform};'
                      'window.firstUserAgent=navigator.userAgent</script>')
            kind = 'text/html'
        body = source.encode()
        self.send_response(200)
        self.send_header('Content-Type', kind)
        self.send_header('Content-Length', str(len(body)))
        self.send_header('Cache-Control', 'no-store')
        self.send_header('Accept-CH', HINTS)
        self.end_headers()
        try:
            self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError):
            pass


class Server:
    def __init__(self):
        self.server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        self.server.daemon_threads = True
        self.received = self.server.received = []
        self.base = f'http://127.0.0.1:{self.server.server_port}'
        threading.Thread(target=lambda: self.server.serve_forever(poll_interval=.02), daemon=True).start()

    def __enter__(self):
        return self

    def __exit__(self, *args):
        self.server.shutdown()
        self.server.server_close()


def cdp_mode(command, mode, server):
    """The CDP client's half: Emulation.setUserAgentOverride on a page of
    tools/zebkit-run's, then (rejection) invalid client hints refused with
    nothing of them in the page or the network."""
    engine = launch_engine([*command, '--no-sandbox', '--headless', '--disable-gpu', '--no-proxy-server',
                            '--user-data-dir=' + server.profile])
    failed = True
    try:
        with part('CDP %s' % mode):
            assert engine.url, 'the engine served no CDP'
            cdp = Cdp(engine.url)
            _, session = cdp.page()
            cdp.call('Page.navigate', {'url': server.base + '/first'}, session=session)
            assert cdp.event('Page.loadEventFired'), 'the first page did not load'
            valid = {'userAgent': AGENTS['cdp'], 'acceptLanguage': 'en-US', 'userAgentMetadata': {
                'brands': [{'brand': 'Before', 'version': '1'}], 'fullVersionList': [{'brand': 'Before', 'version': '1.0'}],
                'platform': 'Linux', 'platformVersion': '1', 'architecture': 'x86', 'model': '', 'mobile': False}}
            cdp.call('Emulation.setUserAgentOverride', valid, session=session)

            def state():
                return cdp.evaluate("Promise.all([fetch('/headers').then(r=>r.json()), navigator.userAgentData"
                                    ".getHighEntropyValues(['platformVersion'])]).then(([h,m])=>({headers:h,"
                                    "ua:navigator.userAgent,metadata:m}))", session)
            before = state()
            assert before['headers']['User-Agent'] == before['ua'] == valid['userAgent'], before
            assert before['metadata']['platformVersion'] == '1', before
            if mode == 'rejection':
                invalid_metadata = [
                    {'platform': 'bad\nplatform'}, {'brands': [{'brand': 'bad\nbrand', 'version': '2'}]},
                    {'fullVersionList': [{'brand': 'After', 'version': 'bad\x00version'}]},
                    {'platformVersion': 'bad\nversion'}, {'architecture': 'bad\narchitecture'},
                    {'model': 'bad\nmodel'}, {'fullVersion': 'bad\nversion'},
                    {'bitness': 'bad\nbitness'}, {'formFactors': ['bad\nfactor']},
                ]
                for index, changes in enumerate(invalid_metadata + [{}]):
                    invalid = {**valid, 'userAgent': 'Zebkit-UA-After/2' if changes else '', 'acceptLanguage': 'fr-FR',
                               'userAgentMetadata': {**valid['userAgentMetadata'], **changes}}
                    try:
                        cdp.call('Emulation.setUserAgentOverride', invalid, session=session)
                    except RuntimeError:
                        pass
                    else:
                        raise AssertionError('invalid client hints accepted: %r' % changes)
                    after = state()
                    assert after['ua'] == before['ua'] and after['metadata'] == before['metadata'], after
                    path = f'/after-rejection-{index}'
                    cdp.call('Page.navigate', {'url': server.base + path}, session=session)
                    assert cdp.event('Page.loadEventFired'), 'navigation after rejection did not load'
                    request = next(headers for p, headers in server.received if p == path)
                    assert (request.get('User-Agent'), request.get('Accept-Language')) == (valid['userAgent'], 'en-US'), \
                        request
                    navigated = state()
                    assert navigated['ua'] == before['ua'] and navigated['metadata'] == before['metadata'], navigated
                    for header in ('User-Agent', 'Accept-Language', 'sec-ch-ua', 'sec-ch-ua-platform', 'sec-ch-ua-mobile'):
                        assert navigated['headers'].get(header) == before['headers'].get(header), (header, navigated)
            cdp.close()
            failed = False
    finally:
        engine.stop(failed)


def main():
    language, command = arguments(sys.argv, ('c', 'python', 'javascript', 'rust', 'cdp'))
    name = NAMES[language]
    for mode in modes(MODES, language):
        with Server() as server:
            page = server.base + '/public'
            if language == 'cdp':
                with tempfile.TemporaryDirectory(prefix='zebkit-ua-cdp-') as work:
                    server.profile = work + '/profile'
                    cdp_mode(command, mode, server)
            else:
                expect = (r'^\d+ of \d+ checks passed$' if language == 'python'
                          else '^ok %s UA/client hints (%s|and atomic rejection)$' % (name, mode))
                env = {'ZEBKIT_UA_TEST_URL': page, 'ZEBKIT_UA_BASE': server.base, 'ZEBKIT_UA_CASE': mode}
                if run(language, command, mode, '%s %s' % (name, mode), ['--no-proxy-server'], [page, mode], env,
                       expect) is None:
                    continue
            # What the server saw: once the override is in, every request carries it.
            agents = [headers.get('User-Agent') for path, headers in server.received if path != '/first']
            if mode in ('hints', 'rejection', 'worker'):
                check(bool(agents) and set(agents) == {AGENTS[language]},
                      '%s %s: every request carried the override' % (name, mode), sorted(set(agents)))
            if mode == 'completion' and language in ('python', 'rust'):
                paths = [path for path, _ in server.received]
                check('/from-completion' in paths and '/superseded-deferred' not in paths,
                      '%s completion: the superseded load never reached the server' % name, paths)
            if mode == 'cdp-scope':
                sent = {path: headers.get('User-Agent') for path, headers in server.received}
                check([sent.get(p) for p in ('/initial', '/external', '/native-while-external', '/detached')]
                      == ['zebkit-native-before/1', 'zebkit-external/2', 'zebkit-external/2', 'zebkit-native-after/3'],
                      '%s cdp-scope: what each page was requested with' % name, sent)
    return finish()


if __name__ == '__main__':
    sys.exit(main())
