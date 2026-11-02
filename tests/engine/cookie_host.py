"""Host-provided cookie key: no keychain, encrypted restarts and original DB isolation."""
import contextlib
import http.server
import os
import pathlib
import secrets
import sqlite3
import sys
import tempfile
import threading

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from cookies import Fixture, run


def main():
    binary = pathlib.Path(sys.argv[1]).resolve()
    server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Fixture)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    try:
        with tempfile.TemporaryDirectory(prefix='zebkit-host-cookie-') as root:
            root = pathlib.Path(root)
            profile, other = root / 'profile', root / 'other'
            profile.mkdir(); other.mkdir()
            original = profile / 'Network/Cookies'
            original.parent.mkdir()
            sentinel = b'owned previous OS-encrypted database; must stay untouched'
            original.write_bytes(sentinel)
            keyfile = root / 'key'
            keyfile.write_bytes(secrets.token_bytes(32)); keyfile.chmod(0o600)
            env = {**os.environ, 'ZEBKIT_TEST_COOKIE_KEY_FILE': str(keyfile)}
            base = f'http://127.0.0.1:{server.server_port}'
            first = run(binary, sys.argv[2:], profile, base + '/set', write=True, environment=env)
            assert len(first['cookies']) == 3, first
            with contextlib.closing(sqlite3.connect(profile / 'Network/Cookies.host')) as db:
                rows = db.execute('SELECT value, encrypted_value FROM cookies').fetchall()
            assert len(rows) == 2 and all(value == '' and encrypted.startswith(b'zbh') for value, encrypted in rows)
            assert b'owned' not in (profile / 'Network/Cookies.host').read_bytes()
            second = run(binary, sys.argv[2:], profile, base + '/check', logout=True, environment=env)
            assert second == {'cookies': ['zebkit_probe_auth', 'zebkit_probe_js'], 'storage': 'owned', 'sent_auth': True}, second
            assert original.read_bytes() == sentinel
            assert run(binary, sys.argv[2:], other, base + '/other', environment=env) == {'cookies': [], 'storage': None, 'sent_auth': False}
            assert run(binary, sys.argv[2:], profile, base + '/check', environment=env) == {'cookies': [], 'storage': 'owned', 'sent_auth': False}
            print('ok host AES-GCM key: encrypted restart, first-request authentication, session expiry, logout and profile isolation')
            print('ok previous OS-encrypted cookie database stays byte-identical; late key changes rejected')
    finally:
        server.shutdown(); server.server_close()


if __name__ == '__main__':
    main()
