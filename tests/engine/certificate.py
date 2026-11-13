"""A document's TLS errors (zebkit_view_on_certificate) in every language, against a
local HTTPS server with a certificate nobody trusts (language_modes.py says how):

    python3 tests/engine/certificate.py c <certificate>
    python3 tests/engine/certificate.py python python3 -I -B tests/engine/certificate_python.py
    python3 tests/engine/certificate.py javascript <zebkit-js> tests/engine/script/certificate.mjs
    python3 tests/engine/certificate.py rust <the certificate_policy example>

The program is given ZEBKIT_CERTIFICATE_TEST_URL (the origin; for the strict mode
mail.google.com's, which Chromium enforces, mapped to the server) and
ZEBKIT_CERTIFICATE_CASE (the mode); zebkit-js the two as the script's arguments.
It loads <origin>/<language>-<mode>, a second view's page named with -other.
HTTP/2 is certificate_h2.py's. Exit 0 when every check passes.
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from harness import check, finish  # noqa: E402
from language_modes import NAMES, arguments, modes, run  # noqa: E402
from tls_fixture import TLSFixture  # noqa: E402

PYTHON_ONLY = {'c': 'not written for C', 'javascript': 'not written for JavaScript'}
# The modes, and the languages each is not written for. Invalid deadlines and
# replies are refused in every mode of every program; C's NULL request too.
MODES = [
    ('default', {}),                         # no function: the error page, nothing sent
    ('allow', {}),                           # allowed once: the page; another view still refused
    ('deny', {}),                            # refused by the function
    ('post', PYTHON_ONLY),                   # a refused form's POST never sent
    ('cache', PYTHON_ONLY),                  # a page cached by an allowed view is asked for again
    ('hold', PYTHON_ONLY),                   # answered later; not from another thread
    ('timeout', PYTHON_ONLY),                # nobody answers: the deadline
    ('navigation', PYTHON_ONLY),             # another page loaded while it waits
    ('stop', PYTHON_ONLY),                   # the view stopped while it waits
    ('cancel-handler', {}),                  # the function removed from inside it
    ('strict', {}),                          # a pinned host's error cannot be allowed
    ('close-request', {}),                   # the view closed from inside it
    ('close-result', {}),                    # the outcome removes itself and closes the view
]


def main():
    language, command = arguments(sys.argv)
    name = NAMES[language]
    for mode in modes(MODES, language):
        with TLSFixture() as fixture:
            port = fixture.server.server_port
            base = f'https://mail.google.com:{port}' if mode == 'strict' else fixture.url
            env = {'ZEBKIT_CERTIFICATE_TEST_URL': base, 'ZEBKIT_CERTIFICATE_CASE': mode}
            expect = (r'^\d+ of \d+ checks passed$' if language == 'python'
                      else '^ok %s certificate policy' % name)
            done = run(language, command, mode, '%s %s' % (name, mode),
                       ['--no-proxy-server', '--host-resolver-rules=MAP mail.google.com 127.0.0.1'],
                       [base, mode], env, expect)
            if done is None:
                continue
            received = list(fixture.received)
            # What reached the server, whatever the program believes.
            if mode == 'allow':
                check(bool(received) and not any('other' in path or 'denied' in path for path in received),
                      '%s allow: the allowed page reached the server, the other view\'s did not' % name, received)
            elif mode == 'cache':
                check(received == ['/%s-cache' % language], '%s cache: one request, the allowed view\'s' % name, received)
            else:
                check(not received, '%s %s: nothing reached the server' % (name, mode), received)
    return finish()


if __name__ == '__main__':
    sys.exit(main())
