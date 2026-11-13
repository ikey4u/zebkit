"""A page's console (zebkit_view_on_console) in every language, against a local
server with a frame of another site (language_modes.py says how):

    python3 tests/engine/console.py python python3 -I -B tests/engine/console_python.py
    python3 tests/engine/console.py javascript <zebkit-js> tests/engine/script/console.mjs
    python3 tests/engine/console.py rust <the console example>

The program is given ZEBKIT_CONSOLE_BASE (the server) and ZEBKIT_CONSOLE_CASE (the
mode); zebkit-js the two as the script's arguments. C's console is
tests/engine/hooks.c's (a view's messages, and not another's), run apart. Exit 0
when every check passes.
"""

import os
import sys
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from harness import finish  # noqa: E402
from language_modes import NAMES, arguments, modes, run  # noqa: E402

C = 'tests/engine/hooks.c has C\'s console, run apart'
# The modes, and the languages each is not written for.
MODES = [
    ('levels', {'c': C}),                    # log, warn, error; Unicode and NUL; source, line, frame
    ('bounds', {'c': C}),                    # 65535 characters, truncated
    ('frame', {'c': C}),                     # a frame of another site, in a process of its own
    ('isolation', {'c': C}),                 # another view's messages are not this one's
    ('replace', {'c': C, 'javascript': 'zebkit-js adds functions, it replaces none'}),
    ('remove', {'c': C, 'javascript': 'zebkit-js removes no function'}),
    ('close', {'c': C}),                     # the view closed from the function
    ('validation', {'c': C, 'javascript': 'not written for JavaScript', 'rust': 'a function is a type in Rust: nothing to refuse'}),  # what is refused
]


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def do_GET(self):
        body = '<title>console hook</title>'
        if self.path == '/frame':
            body += f'<iframe src="http://localhost:{self.server.server_port}/child"></iframe>'
        if self.path == '/child':
            body += '<script>console.warn("cross-site child")</script>'
        data = body.encode()
        self.send_response(200)
        self.send_header('Content-Type', 'text/html; charset=utf-8')
        self.send_header('Content-Length', str(len(data)))
        self.end_headers()
        try:
            self.wfile.write(data)
        except (BrokenPipeError, ConnectionResetError):
            pass


def main():
    language, command = arguments(sys.argv)
    name = NAMES[language]
    server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
    server.daemon_threads = True
    threading.Thread(target=lambda: server.serve_forever(poll_interval=.02), daemon=True).start()
    base = f'http://127.0.0.1:{server.server_port}'
    try:
        for mode in modes(MODES, language):
            expect = r'^\d+ of \d+ checks passed$' if language == 'python' else '^ok %s console %s$' % (name, mode)
            run(language, command, mode, '%s %s' % (name, mode), ['--no-proxy-server'], [base, mode],
                {'ZEBKIT_CONSOLE_BASE': base, 'ZEBKIT_CONSOLE_CASE': mode}, expect)
    finally:
        server.shutdown()
        server.server_close()
    return finish()


if __name__ == '__main__':
    sys.exit(main())
