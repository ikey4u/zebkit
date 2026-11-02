"""Persistent cookies across clean restarts, with the real OS key provider.

Usage: python3 cookies.py <engine-runner> [engine switches...]
Requires an unlocked macOS Keychain or Linux desktop keyring. All profiles,
cookies and HTTP traffic in this test belong to its localhost fixture.
"""
import contextlib
import http.server
import json
import os
import pathlib
import socket
import sqlite3
import subprocess
import sys
import tempfile
import threading
import time
import urllib.parse
import urllib.request

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from cdp_page import Cdp

requests = {}


class Fixture(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        requests[self.path] = 'zebkit_probe_auth=owned' in self.headers.get('Cookie', '')
        body = b'<title>Cookie restart fixture</title>'
        self.send_response(200)
        if self.path == '/set':
            self.send_header('Set-Cookie', 'zebkit_probe_auth=owned; Max-Age=3600; Path=/; HttpOnly; SameSite=Lax')
        elif self.path == '/logout':
            self.send_header('Set-Cookie', 'zebkit_probe_auth=; Max-Age=0; Path=/; HttpOnly; SameSite=Lax')
        self.send_header('Content-Type', 'text/html')
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *args):
        pass


def run(binary, switches, profile, url, write=False, logout=False, environment=None):
    with socket.socket() as available:
        available.bind(('127.0.0.1', 0))
        port = available.getsockname()[1]
    with (pathlib.Path(profile) / 'browser.log').open('a') as log:
        arguments = [*switches, f'--user-data-dir={profile}', f'--remote-debugging-port={port}', url]
        app = os.environ.get('ZEBKIT_COOKIE_TEST_APP')
        command = [str(binary), *arguments]
        if app:
            command = ['open', '-n', '-W', '--stdout', log.name, '--stderr', log.name]
            if environment and 'ZEBKIT_TEST_COOKIE_KEY_FILE' in environment:
                command += ['--env', 'ZEBKIT_TEST_COOKIE_KEY_FILE=' + environment['ZEBKIT_TEST_COOKIE_KEY_FILE']]
            command += [app, '--args', *arguments]
        process = subprocess.Popen(command, stdout=log, stderr=log, env=environment)
        try:
            deadline = time.monotonic() + 30
            while True:
                if process.poll() is not None:
                    raise RuntimeError(f'engine exited: {process.returncode}: ' + pathlib.Path(log.name).read_text()[-800:])
                try:
                    with urllib.request.urlopen(f'http://127.0.0.1:{port}/json/version', timeout=1) as response:
                        endpoint = json.load(response)['webSocketDebuggerUrl']
                    break
                except OSError:
                    if time.monotonic() > deadline:
                        raise
                    time.sleep(0.1)
            cdp = Cdp(endpoint)
            target = next(t['targetId'] for t in cdp.call('Target.getTargets')['targetInfos'] if t['type'] == 'page')
            # The initial page must commit before a renderer evaluation.
            time.sleep(0.5)
            session = cdp.call('Target.attachToTarget', {'targetId': target, 'flatten': True})['sessionId']

            def evaluate(expression):
                return cdp.call('Runtime.evaluate', {'expression': expression, 'returnByValue': True},
                                session=session)['result'].get('value')

            for _ in range(100):
                if evaluate('document.title') == 'Cookie restart fixture':
                    break
                time.sleep(0.1)
            assert evaluate('document.title') == 'Cookie restart fixture'
            if write:
                evaluate("document.cookie='zebkit_probe_js=owned; Max-Age=3600; Path=/';"
                         "document.cookie='zebkit_probe_session=owned; Path=/';"
                         "localStorage.setItem('zebkit_probe','owned')")
            state = {
                'cookies': sorted(cookie['name'] for cookie in cdp.call('Network.getCookies', {'urls': [url]},
                                                                         session=session)['cookies']
                                  if cookie['name'].startswith('zebkit_probe')),
                'storage': evaluate("localStorage.getItem('zebkit_probe')"),
                'sent_auth': requests.get(urllib.parse.urlparse(url).path, False),
            }
            if logout:
                cdp.call('Page.enable', session=session)
                cdp.call('Page.navigate', {'url': url.rsplit('/', 1)[0] + '/logout'}, session=session)
                assert cdp.event('Page.loadEventFired', timeout=15), 'logout did not load'
                evaluate("document.cookie='zebkit_probe_js=; Max-Age=0; Path=/'")
                assert not cdp.call('Network.getCookies', {'urls': [url]}, session=session)['cookies']
            cdp.call('Target.closeTarget', {'targetId': target})
            assert process.wait(timeout=15) == 0, 'the engine must shut down cleanly'
            return state
        finally:
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()


def main():
    binary = pathlib.Path(sys.argv[1]).resolve()
    switches = sys.argv[2:]
    server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Fixture)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    try:
        with tempfile.TemporaryDirectory(prefix='zebkit-cookie-restart-') as profile, \
                tempfile.TemporaryDirectory(prefix='zebkit-cookie-other-') as other:
            base = f'http://127.0.0.1:{server.server_port}'
            first = run(binary, switches, profile, base + '/set', write=True)
            assert first['cookies'] == ['zebkit_probe_auth', 'zebkit_probe_js', 'zebkit_probe_session'], first
            database = pathlib.Path(profile) / 'Network/Cookies'
            assert database.is_file(), 'no persistent cookie database'
            with contextlib.closing(sqlite3.connect(f'file:{database}?mode=ro', uri=True)) as db:
                rows = db.execute("SELECT value, encrypted_value FROM cookies WHERE name LIKE 'zebkit_probe_%'").fetchall()
            assert len(rows) == 2 and all(value == '' and encrypted for value, encrypted in rows), rows
            assert b'owned' not in database.read_bytes(), 'cookie values must not be plaintext on disk'
            print('ok persistent and HttpOnly cookies are encrypted on disk', flush=True)
            second = run(binary, switches, profile, base + '/check', logout=True)
            assert second == {'cookies': ['zebkit_probe_auth', 'zebkit_probe_js'], 'storage': 'owned', 'sent_auth': True}, second
            print('ok persistent cookies authenticate the first request after restart; session cookie expires', flush=True)
            isolated = run(binary, switches, other, base + '/other')
            assert isolated == {'cookies': [], 'storage': None, 'sent_auth': False}, isolated
            print('ok another profile has no session or local storage from the first', flush=True)
            final = run(binary, switches, profile, base + '/check')
            assert final == {'cookies': [], 'storage': 'owned', 'sent_auth': False}, final
            print('ok deleted login cookies do not reappear after another restart', flush=True)
    finally:
        server.shutdown()
        server.server_close()


if __name__ == '__main__':
    main()
