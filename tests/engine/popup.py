"""A page's new windows (zebkit_view_on_window_open, _on_window_open_event, the
popup policy) in every language, against a local server with a frame of another
site (language_modes.py says how):

    python3 tests/engine/popup.py python python3 -I -B tests/engine/popup_python.py
    python3 tests/engine/popup.py rust <the popup example>

The program is given ZEBKIT_POPUP_BASE (the server) and ZEBKIT_POPUP_CASE (the
mode). The other languages' are run apart: C's tests/engine/popup.c (a site's
sign-in through a popup) and hooks.c (the events, every view's own),
JavaScript's tests/engine/script/window_open.mjs (the popup adopted), and the
hooks' popup group (hook_policies.py). Exit 0 when every check passes.
"""

import os
import sys
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlsplit

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from harness import finish  # noqa: E402
from language_modes import NAMES, arguments, modes, run  # noqa: E402

OTHERS = {'c': 'tests/engine/popup.c and hooks.c have C\'s, run apart',
          'javascript': 'tests/engine/script/window_open.mjs has JavaScript\'s, run apart'}
# The modes, and the languages each is not written for.
MODES = [(mode, OTHERS) for mode in (
    'blocked',          # the decision's metadata and features; the older popup-blocked event
    'isolation',        # another view's decisions are not this one's
    'created',          # two windows, each its creation's snapshot
    'snapshot',         # a window's snapshot kept when it loads another page
    'plain',            # no features asked
    'noopener',         # no opener
    'close-creation',   # the window closed from the creation function
    'frame',            # opened by a frame of another site
    'remove',           # the observer removed: no more events, the older event goes on
    'close-event',      # the view closed from the observer
    'adoption',         # the window as a view: its opener, its messages, closing; the functions removed in them
)]


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

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
            run(language, command, mode, '%s %s' % (name, mode), ['--no-proxy-server'], [base, mode],
                {'ZEBKIT_POPUP_BASE': base, 'ZEBKIT_POPUP_CASE': mode}, r'^\d+ of \d+ checks passed$')
    finally:
        server.shutdown()
        server.server_close()
    return finish()


if __name__ == '__main__':
    sys.exit(main())
