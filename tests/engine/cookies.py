"""Persistent cookies across clean restarts, with the real OS key provider.

    python3 tests/engine/cookies.py <engine-runner> [engine switches...]

Requires an unlocked macOS Keychain or Linux desktop keyring
(scripts/engine-test.sh runs it on macOS and Windows, and on Linux with
ENGINE_TEST_PERSISTENT_COOKIES=1). All profiles, cookies and HTTP traffic in
this test belong to its localhost fixture. run() is cookie_host.py's,
cookie_file.py's and cookie_file_options.py's too.
"""
import contextlib
import http.server
import pathlib
import sqlite3
import subprocess
import sys
import tempfile
import threading
import time
import urllib.parse

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from harness import Cdp, Engine, finish, part  # noqa: E402

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
    """One start of the engine on profile at url, ended by closing its page: what
    it had then (its cookies, the page's storage, whether the server saw the login
    cookie). It must end by itself, with 0."""
    engine = Engine([str(binary), *switches, f'--user-data-dir={profile}'], page=url, env=environment)
    try:
        if not engine.url:
            raise RuntimeError(f'the engine served no CDP (exit {engine.poll()}): ' + engine.tail(10))
        cdp = Cdp(engine.url)
        # The page attached once it has committed to url: before, an evaluation
        # has no context to run in.
        deadline = time.monotonic() + 30
        while True:
            pages = [t for t in cdp.call('Target.getTargets')['targetInfos'] if t['type'] == 'page']
            if pages and pages[0]['url'] == url:
                break
            if time.monotonic() > deadline:
                raise RuntimeError(f'the page did not load {url}: {pages}')
            time.sleep(.01)
        target = pages[0]['targetId']
        session = cdp.call('Target.attachToTarget', {'targetId': target, 'flatten': True})['sessionId']

        def evaluate(expression):
            return cdp.call('Runtime.evaluate', {'expression': expression, 'returnByValue': True},
                            session=session)['result'].get('value')

        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            try:
                if evaluate('document.title') == 'Cookie restart fixture':
                    break
            except RuntimeError:
                pass  # Its document is not there yet.
            time.sleep(.01)
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
        try:
            code = engine.process.wait(timeout=15)
        except subprocess.TimeoutExpired:
            code = None
        assert code == 0, f'the engine must shut down cleanly: {code}'
        return state
    finally:
        engine.stop()


def main():
    binary = pathlib.Path(sys.argv[1]).resolve()
    switches = sys.argv[2:]
    server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Fixture)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    try:
        with tempfile.TemporaryDirectory(prefix='zebkit-cookie-restart-') as profile, \
                tempfile.TemporaryDirectory(prefix='zebkit-cookie-other-') as other:
            base = f'http://127.0.0.1:{server.server_port}'
            # Each start goes on from what the one before left.
            with part('persistent and HttpOnly cookies are encrypted on disk', fatal=True):
                first = run(binary, switches, profile, base + '/set', write=True)
                assert first['cookies'] == ['zebkit_probe_auth', 'zebkit_probe_js', 'zebkit_probe_session'], first
                database = pathlib.Path(profile) / 'Network/Cookies'
                assert database.is_file(), 'no persistent cookie database'
                with contextlib.closing(sqlite3.connect(f'file:{database}?mode=ro', uri=True)) as db:
                    rows = db.execute("SELECT value, encrypted_value FROM cookies WHERE name LIKE 'zebkit_probe_%'").fetchall()
                assert len(rows) == 2 and all(value == '' and encrypted for value, encrypted in rows), rows
                assert b'owned' not in database.read_bytes(), 'cookie values must not be plaintext on disk'
            with part('persistent cookies authenticate the first request after restart; session cookie expires', fatal=True):
                second = run(binary, switches, profile, base + '/check', logout=True)
                assert second == {'cookies': ['zebkit_probe_auth', 'zebkit_probe_js'], 'storage': 'owned', 'sent_auth': True}, second
            with part('another profile has no session or local storage from the first'):
                isolated = run(binary, switches, other, base + '/other')
                assert isolated == {'cookies': [], 'storage': None, 'sent_auth': False}, isolated
            with part('deleted login cookies do not reappear after another restart'):
                final = run(binary, switches, profile, base + '/check')
                assert final == {'cookies': [], 'storage': 'owned', 'sent_auth': False}, final
    except Exception:  # noqa: BLE001
        pass  # Said by its part: the rest goes on from it.
    finally:
        server.shutdown()
        server.server_close()
    return finish()


if __name__ == '__main__':
    sys.exit(main())
