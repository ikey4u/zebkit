"""Built-in file backend through a normal runner's flags, including startup errors."""
import http.server
import pathlib
import subprocess
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
        with tempfile.TemporaryDirectory(prefix='zebkit-cookie-options-') as parent:
            root = pathlib.Path(parent)
            profile = root / 'profile'; profile.mkdir()
            directory = root / '密钥 folder'
            flags = [*sys.argv[2:], '--zebkit-cookie-storage=file', f'--zebkit-cookie-key-directory={directory}']
            base = f'http://127.0.0.1:{server.server_port}'
            run(binary, flags, profile, base + '/set', write=True)
            assert (directory / 'cookie.key').is_file() and not (profile / 'Secrets').exists()
            assert run(binary, flags, profile, base + '/check')['sent_auth']
            database = profile / 'Network/Cookies.host'
            original = database.read_bytes()
            keyfile = directory / 'cookie.key'
            keyfile.write_bytes(b'invalid-format')
            failure = subprocess.run([str(binary), *flags, f'--user-data-dir={profile}', 'about:blank'], capture_output=True, timeout=15)
            assert failure.returncode == 1 and keyfile.read_bytes() == b'invalid-format'
            assert database.read_bytes() == original
            keyfile.unlink()
            failure = subprocess.run([str(binary), *flags, f'--user-data-dir={profile}', 'about:blank'], capture_output=True, timeout=15)
            assert failure.returncode == 1 and not keyfile.exists() and database.read_bytes() == original
            failure = subprocess.run([str(binary), '--zebkit-cookie-storage=unknown', f'--user-data-dir={profile}', 'about:blank'], capture_output=True, timeout=15)
            assert failure.returncode == 1
            print('ok normal-runner flags: Unicode custom directory and restart, invalid/missing key fail before touching database, unknown mode rejects')
    finally:
        server.shutdown(); server.server_close()


if __name__ == '__main__':
    main()
