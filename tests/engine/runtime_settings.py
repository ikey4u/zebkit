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
import subprocess
import sys
import tempfile
import threading
from urllib.parse import parse_qs, urlparse

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
    checks = failures = 0

    def check(ok, what, detail=''):
        nonlocal checks, failures
        checks += 1
        failures += not ok
        print(f"{'ok  ' if ok else 'FAIL'} {language}: {what}{'' if ok else f': {detail}'}", flush=True)

    with tempfile.TemporaryDirectory(prefix='zebkit-runtime-settings-') as work:
        switches = ['--no-sandbox', '--headless', f'--user-data-dir={work}']
        base = f'http://127.0.0.1:{server.server_port}'
        arguments = []
        for part in command:
            arguments += switches if part == '@switches@' else [part]
        if language == 'javascript':
            arguments.append(json.dumps({'RUNTIME_BASE': base, 'RUNTIME_OTHER': server.other}))
        environment = {**os.environ, 'RUNTIME_BASE': base, 'RUNTIME_OTHER': server.other}
        try:
            run = subprocess.run(arguments, env=environment, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, timeout=120)
        except subprocess.TimeoutExpired as error:
            print(error.stdout, error.stderr)
            check(False, 'the program ended')
            print(f'{checks - failures} of {checks} checks passed')
            return 1
        check(run.returncode == 0 and f'ok {language}\n' in run.stdout, 'the program passed',
              f'exit {run.returncode}\n{run.stdout}\n{run.stderr[-3000:]}')
        # What is on the disk, whatever the program believes.
        downloads = pathlib.Path(work) / 'Downloads'
        names = sorted(path.name for path in downloads.iterdir()) if downloads.is_dir() else []
        check(any(name == 'saved.bin' for name in names), '"save" is on the disk', names)
        check(not any(name.startswith('denied') for name in names), '"deny" left nothing on the disk', names)
    print(f'{checks - failures} of {checks} checks passed')
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
