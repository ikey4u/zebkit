"""HTTP authentication (zebkit_view_on_http_auth) in every language, against real
challenges of a local server, and of a proxy (language_modes.py says how):

    python3 tests/engine/http_auth.py c <http_auth>
    python3 tests/engine/http_auth.py python python3 -I -B tests/engine/http_auth_python.py
    python3 tests/engine/http_auth.py javascript <zebkit-js> tests/engine/script/http_auth.mjs
    python3 tests/engine/http_auth.py rust <the http_auth example>

The program is given ZEBKIT_HTTP_AUTH_URL (the origin it loads /auth/<language>-<mode>
from), _USER, _PASSWORD, _CASE (the mode), _REALM, _PORT (the challenger's) and,
for the proxy, _PROXY=1; zebkit-js the URL, user, password and mode as the script's
arguments. Exit 0 when every check passes.
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from harness import check, finish  # noqa: E402
from http_auth_fixture import AuthFixture  # noqa: E402
from language_modes import NAMES, arguments, modes, run  # noqa: E402

# The modes, and the languages each is not written for. The engine's sign-in
# dialog, without a function, is sign_in.py's; C's NULL token and re-set of the
# same callback are its "invalid" mode alone.
MODES = [
    ('default', {}),                         # no function: the challenge is cancelled
    ('allow', {}),                           # the credentials, once
    ('deny', {}),                            # answered with none: cancelled
    ('retry', {'c': 'not written for C', 'javascript': 'not written for JavaScript'}),
    ('cancel-handler', {}),                  # the function removed from inside it
    ('close-request', {}),                   # the view closed from inside it
    ('close-result', {}),                    # the view closed from the outcome
    ('invalid', {'javascript': 'its invalid replies are checked in every mode of http_auth.mjs'}),
                                                 # replies refused, then a valid one
    ('timeout', {}),                         # nobody answers: the deadline
    ('stop', {}),                            # the view stopped while it waits
    ('navigation', {}),                      # another page loaded while it waits
    ('remove', {}),                          # the function removed while it waits
    ('replace', {}),                         # another function while it waits
    ('fetch', {'c': 'not written for C', 'javascript': 'not written for JavaScript'}),
    ('frame', {'c': 'not written for C', 'javascript': 'not written for JavaScript'}),
    ('proxy', {}),                           # a proxy's challenge, allowed
]
# What the server must have accepted, or not, whatever the program believes.
ACCEPTED = {'allow', 'retry', 'close-result', 'invalid', 'fetch', 'frame', 'proxy'}


def main():
    language, command = arguments(sys.argv)
    name = NAMES[language]
    for mode in modes(MODES, language):
        proxy = mode == 'proxy'
        with AuthFixture(proxy=proxy) as fixture:
            base = 'http://auth-proxy.invalid' if proxy else fixture.url
            switches = ([f'--proxy-server={fixture.url}', '--proxy-bypass-list=<-loopback>',
                         '--host-resolver-rules=MAP auth-proxy.invalid ~NOTFOUND'] if proxy else ['--no-proxy-server'])
            env = {'ZEBKIT_HTTP_AUTH_URL': base, 'ZEBKIT_HTTP_AUTH_USER': fixture.username,
                   'ZEBKIT_HTTP_AUTH_PASSWORD': fixture.password, 'ZEBKIT_HTTP_AUTH_CASE': mode,
                   'ZEBKIT_HTTP_AUTH_REALM': fixture.realm, 'ZEBKIT_HTTP_AUTH_PORT': str(fixture.server.server_port)}
            if proxy:
                env['ZEBKIT_HTTP_AUTH_PROXY'] = '1'
            expect = r'^\d+ of \d+ checks passed$' if language == 'python' else '^ok %s HTTP auth %s$' % (name, mode)
            done = run(language, command, mode, '%s %s' % (name, mode), switches,
                       [base, fixture.username, fixture.password, mode], env, expect)
            if done is None:
                continue
            accepted = [row for row in fixture.received if row['accepted']]
            if mode in ACCEPTED:
                check(bool(accepted), '%s %s: the server accepted the credentials' % (name, mode), fixture.received)
            else:
                check(not accepted and bool(fixture.received),
                      '%s %s: the server was asked and accepted nothing' % (name, mode), fixture.received)
            if mode == 'retry':
                check(any(row['provided'] and not row['accepted'] for row in fixture.received),
                      '%s retry: the wrong credentials reached the server first' % name, fixture.received)
            if mode == 'frame':
                check(any(row['path'] == '/auth/frame' and row['accepted'] for row in fixture.received),
                      '%s frame: the frame authenticated' % name, fixture.received)
    return finish()


if __name__ == '__main__':
    sys.exit(main())
