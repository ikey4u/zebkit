"""Runtime settings (<zebkit/hooks.h>) in one language, against a local server, and what
a third party, the disk, the local network and the program saw. Cross-site is 127.0.0.1
and localhost: SameSite=None needs Secure, which Chromium allows on http://localhost,
so no TLS.

WebRTC's IP handling policy: the page's rtc() gathers candidates as the scripts that
read a machine's addresses do, and says "host" or "none". What went out on the local
network is listened to here, on mDNS's group (224.0.0.251:5353), where Chromium
announces the <uuid>.local name of a host candidate: the program writes the times of
its parts to RUNTIME_MARKS ("quiet-begin", "quiet-end", "loud-begin", "loud-end",
milliseconds since the epoch), and in a quiet part nothing of a page's may be heard,
in the loud one something must (or the listening proves nothing).

    python3 tests/engine/runtime_settings.py <language> <command...>

@switches@ in the command is where the engine's switches go. A program reads
RUNTIME_BASE and RUNTIME_OTHER; JavaScript gets them as a JSON argument, at the end.
"""

from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
import pathlib
import re
import socket
import struct
import sys
import tempfile
import threading
import time
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
function rtc() {
  return new Promise(resolve => {
    const found = [];
    const pc = new RTCPeerConnection({ iceServers: [] });
    pc.createDataChannel('x');
    let told = false;
    const done = () => {
      if (told) return;
      told = true;
      pc.close();
      resolve(found.some(candidate => / typ host /.test(candidate)) ? 'host' : 'none');
    };
    pc.onicecandidate = e => e.candidate ? found.push(e.candidate.candidate) : done();
    pc.createOffer().then(offer => pc.setLocalDescription(offer));
    setTimeout(done, 4000);
  });
}
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


# An mDNS name of Chromium's: a version 4 UUID, then .local.
UUID = re.compile(rb'[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[0-9a-f]{4}-[0-9a-f]{12}')


def listen_mdns():
    """When packets naming a UUID came to mDNS's group: a list the thread fills, or
    None when the group cannot be joined here (5353 held without SO_REUSEADDR)."""
    try:
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP)
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        if hasattr(socket, 'SO_REUSEPORT'):
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEPORT, 1)
        sock.bind(('', 5353))
        sock.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP,
                        struct.pack('4s4s', socket.inet_aton('224.0.0.251'), socket.inet_aton('0.0.0.0')))
    except OSError:
        return None
    heard = []

    def run():
        while True:
            try:
                data, _ = sock.recvfrom(9000)
            except OSError:
                return
            if UUID.search(data):
                heard.append(time.time() * 1000)
    threading.Thread(target=run, daemon=True).start()
    return heard


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
        marks_file = pathlib.Path(work) / 'marks'
        heard = listen_mdns()
        if language == 'javascript':
            arguments.append(json.dumps({'RUNTIME_BASE': base, 'RUNTIME_OTHER': server.other,
                                         'RUNTIME_MARKS': str(marks_file)}))
        environment = {**os.environ, 'RUNTIME_BASE': base, 'RUNTIME_OTHER': server.other,
                       'RUNTIME_MARKS': str(marks_file)}
        # It ends with "ok <language>" when all it checked holds.
        if not program(arguments, f'{language}: the program passed', expect=f'^ok {language}$', seconds=180,
                       env=environment):
            return finish()
        # What is on the disk, whatever the program believes.
        downloads = pathlib.Path(work) / 'Downloads'
        names = sorted(path.name for path in downloads.iterdir()) if downloads.is_dir() else []
        check(any(name == 'saved.bin' for name in names), '"save" is on the disk', names)
        check(not any(name.startswith('denied') for name in names), '"deny" left nothing on the disk', names)
        # The local network, whatever the page said.
        marks = {}
        for line in marks_file.read_text().splitlines() if marks_file.exists() else []:
            what, when = line.split()
            marks[what] = float(when)
        if heard is None:
            harness.skip(f'{language}: mDNS on the local network', "mDNS's group cannot be joined here")
        elif not check(all(key in marks for key in ('quiet-begin', 'quiet-end', 'loud-begin', 'loud-end')),
                       'the program marked its parts', marks):
            pass
        else:
            time.sleep(1)  # what was on its way
            quiet = [t for t in heard if marks['quiet-begin'] <= t <= marks['quiet-end']]
            loud = [t for t in heard if marks['loud-begin'] <= t <= marks['loud-end']]
            check(not quiet, 'disable_non_proxied_udp: no mDNS announcement on the local network', quiet)
            check(bool(loud), '"default" on one view: its host name announced (the listening works)', heard)
    return finish()


if __name__ == '__main__':
    sys.exit(main())
