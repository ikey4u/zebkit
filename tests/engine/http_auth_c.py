"""Keep the HTTP fixture on the host; only C optionally receives old libc."""
import os
import subprocess
import sys
import tempfile
from http_auth_fixture import AuthFixture

for mode in ('allow', 'cancel'):
    with AuthFixture() as fixture, tempfile.TemporaryDirectory(prefix='zebkit-http-auth-c-') as profile:
        env = dict(os.environ, ZEBKIT_HTTP_AUTH_URL=fixture.url,
                   ZEBKIT_HTTP_AUTH_USER=fixture.username,
                   ZEBKIT_HTTP_AUTH_PASSWORD=fixture.password, ZEBKIT_HTTP_AUTH_CASE=mode)
        env.pop('LD_PRELOAD', None)
        if len(sys.argv) > 2:
            env['LD_LIBRARY_PATH'] = sys.argv[2]
        child = subprocess.run([sys.argv[1], '--no-sandbox', '--headless', '--disable-gpu',
                                '--no-proxy-server', f'--user-data-dir={profile}'],
                               env=env, capture_output=True, text=True, timeout=60)
        print(child.stdout, end='')
        print(child.stderr, end='', file=sys.stderr)
        assert child.returncode == 0, child.returncode
        assert 'ok C HTTP auth '+mode in child.stdout
        assert any(row['accepted'] for row in fixture.received) == (mode == 'allow'), fixture.received
