"""The library's built-in cookie key backend; no host key generation or storage.

    python3 tests/engine/cookie_file.py <cookie-host> [engine switches...]
"""
import contextlib
import http.server
import os
import pathlib
import secrets
import subprocess
import sqlite3
import sys
import tempfile
import threading

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from cookies import Fixture, run  # noqa: E402
from harness import finish, part  # noqa: E402


def main():
    binary = pathlib.Path(sys.argv[1]).resolve()
    switches = [*sys.argv[2:], '--test-builtin-cookie-storage']
    server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Fixture)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    try:
        with tempfile.TemporaryDirectory(prefix='zebkit-builtin-cookie-') as parent:
            root = pathlib.Path(parent)
            profile, other = root / 'profile', root / 'other'
            profile.mkdir(); other.mkdir()
            original = profile / 'Network/Cookies'
            original.parent.mkdir()
            original.write_bytes(b'owned OS-cookie database must stay unchanged')
            old = original.read_bytes()
            base = f'http://127.0.0.1:{server.server_port}'
            with part('built-in API: automatic private key, encrypted cookies, restart, logout and profile isolation'):
                first = run(binary, switches, profile, base + '/set', write=True)
                keyfile = profile / 'Secrets/cookie.key'
                key = keyfile.read_bytes()
                assert len(key) == 53 and key.startswith(b'Zebkit Cookie Key v1\n')
                if os.name == 'posix':
                    assert keyfile.stat().st_mode & 0o777 == 0o600
                    assert keyfile.parent.stat().st_mode & 0o777 == 0o700
                assert len(first['cookies']) == 3
                with contextlib.closing(sqlite3.connect(profile / 'Network/Cookies.host')) as db:
                    rows = db.execute('SELECT value, encrypted_value FROM cookies').fetchall()
                assert len(rows) == 2 and all(value == '' and encrypted.startswith(b'zbh') for value, encrypted in rows)
                assert b'owned' not in (profile / 'Network/Cookies.host').read_bytes()
                second = run(binary, switches, profile, base + '/check', logout=True)
                assert second == {'cookies': ['zebkit_probe_auth', 'zebkit_probe_js'], 'storage': 'owned', 'sent_auth': True}
                assert keyfile.read_bytes() == key and original.read_bytes() == old
                isolated = run(binary, switches, other, base + '/other')
                assert isolated == {'cookies': [], 'storage': None, 'sent_auth': False}
                assert (other / 'Secrets/cookie.key').read_bytes() != key
                assert run(binary, switches, profile, base + '/check')['cookies'] == []
            with part('former host key format and host-encrypted login survive backend move without rotation'):
                # Feed the former host-managed key into a legacy cookie store, then
                # select the built-in backend. The existing login must be restored.
                legacy = root / 'legacy'; legacy.mkdir()
                raw = root / 'raw-key'; raw.write_bytes(secrets.token_bytes(32))
                env = {**os.environ, 'ZEBKIT_TEST_COOKIE_KEY_FILE': str(raw)}
                run(binary, sys.argv[2:], legacy, base + '/set', write=True, environment=env)
                # Python's mode 0o700 on Windows lists SYSTEM and Administrators too: not private.
                folder = legacy / 'Secrets'; folder.mkdir(**({} if os.name == 'nt' else {'mode': 0o700}))
                legacy_key = folder / 'cookie.key'
                legacy_key.write_bytes(b'Zebkit Cookie Key v1\n' + raw.read_bytes()); legacy_key.chmod(0o600)
                if os.name == 'nt':
                    # chmod does not make it private there: only the owner in the list.
                    user = os.environ['USERNAME']
                    for path in (legacy_key, folder):
                        subprocess.run(['icacls', str(path), '/setowner', user], check=True, stdout=subprocess.DEVNULL)
                        subprocess.run(['icacls', str(path), '/inheritance:r', '/grant:r', f'{user}:F'],
                                       check=True, stdout=subprocess.DEVNULL)
                        # Python's temporary directories list SYSTEM and Administrators (SIDs: their
                        # names are in the system's language), and these stay.
                        subprocess.run(['icacls', str(path), '/remove:g', '*S-1-5-18', '*S-1-5-32-544'],
                                       check=True, stdout=subprocess.DEVNULL)
                restored = run(binary, switches, legacy, base + '/check')
                assert restored['sent_auth'] and restored['cookies'] == ['zebkit_probe_auth', 'zebkit_probe_js']
    finally:
        server.shutdown(); server.server_close()
    return finish()


if __name__ == '__main__':
    sys.exit(main())
