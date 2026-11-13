"""Runtime settings (<zebkit/hooks.h>) in one language, against a local server, and what
a third party, the disk and the program saw. Cross-site is 127.0.0.1 and localhost:
SameSite=None needs Secure, which Chromium allows on http://localhost, so no TLS.

    python3 tests/engine/runtime_settings.py <language> <command...>

@switches@ in the command is where the engine's switches go. A program reads
RUNTIME_BASE and RUNTIME_OTHER; JavaScript gets them as a JSON argument, at the end.
"""

from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
import pathlib
import sys
import tempfile
import threading
from urllib.parse import parse_qs, urlparse

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import harness  # noqa: E402
from harness import finish, program  # noqa: E402

TOP = '''<!doctype html><title>top</title><script>
const OTHER = %s;
function frame(path) {
  return new Promise(resolve => {
    const f = document.createElement('iframe');
    addEventListener('message', function on(e) {
      if (e.source === f.contentWindow) { removeEventListener('message', on); f.remove(); resolve(e.data); }
    });
    f.src = OTHER + path;
    document.body.append(f);
  });
}
function read(name) { return frame('/read?name=' + name); }
function probe(name) { return frame('/set?name=' + name).then(() => read(name)); }
function save(name) {
  const a = document.createElement('a');
  a.href = '/download?name=' + name;
  a.download = '';
  document.body.append(a);
  a.click();
  a.remove();
}
function opens() { const w = window.open('/plain'); const opened = !!w; if (w) w.close(); return opened; }
</script><body></body>'''


class Server(BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'

    def log_message(self, *args):
        pass

    def reply(self, body, kind='text/html', extra=()):
        self.send_response(200)
        self.send_header('Content-Type', kind)
        self.send_header('Cache-Control', 'no-store')
        self.send_header('Content-Length', str(len(body)))
        for name, value in extra:
            self.send_header(name, value)
        self.end_headers()
        try:
            self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError):
            pass

    def do_GET(self):
        url = urlparse(self.path)
        name = parse_qs(url.query).get('name', ['x'])[0]
        post = "<!doctype html><script>parent.postMessage(%s, '*')</script>"
        if url.path == '/top':
            self.reply((TOP % json.dumps(self.server.other)).encode())
        elif url.path == '/set':
            self.reply((post % '"set"').encode(), extra=[('Set-Cookie', f'{name}=1; Path=/; SameSite=None; Secure')])
        elif url.path == '/read':
            # What the third party's server saw, nothing the page could say.
            sent = f'{name}=1' in self.headers.get('Cookie', '')
            self.reply((post % ('"cookie"' if sent else '"none"')).encode())
        elif url.path == '/download':
            self.reply(b'z' * 4096, 'application/x-zebkit', [('Content-Disposition', f'attachment; filename="{name}"')])
        else:
            self.reply(b'<!doctype html><title>plain</title>')


def main():
    language, command = sys.argv[1], sys.argv[2:]
    server = ThreadingHTTPServer(('127.0.0.1', 0), Server)
    server.other = f'http://localhost:{server.server_port}'
    threading.Thread(target=server.serve_forever, daemon=True).start()

    def check(ok, what, detail=None):
        return harness.check(ok, f'{language}: {what}', detail)

    with tempfile.TemporaryDirectory(prefix='zebkit-runtime-settings-') as work:
        switches = ['--no-sandbox', '--headless', f'--user-data-dir={work}']
        base = f'http://127.0.0.1:{server.server_port}'
        arguments = []
        for part in command:
            arguments += switches if part == '@switches@' else [part]
        if language == 'javascript':
            arguments.append(json.dumps({'RUNTIME_BASE': base, 'RUNTIME_OTHER': server.other}))
        environment = {**os.environ, 'RUNTIME_BASE': base, 'RUNTIME_OTHER': server.other}
        # It ends with "ok <language>" when all it checked holds.
        if not program(arguments, f'{language}: the program passed', expect=f'^ok {language}$', seconds=120,
                       env=environment):
            return finish()
        # What is on the disk, whatever the program believes.
        downloads = pathlib.Path(work) / 'Downloads'
        names = sorted(path.name for path in downloads.iterdir()) if downloads.is_dir() else []
        check(any(name == 'saved.bin' for name in names), '"save" is on the disk', names)
        check(not any(name.startswith('denied') for name in names), '"deny" left nothing on the disk', names)
    return finish()


if __name__ == '__main__':
    sys.exit(main())
