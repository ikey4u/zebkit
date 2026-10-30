"""Run existing host-language bindings on real loopback HTTP challenges."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
from http_auth_fixture import AuthFixture

script=Path(__file__).parent/'script/http_auth.mjs'
for mode in ('allow','cancel-handler','close-request','close-result'):
    with AuthFixture() as fixture,tempfile.TemporaryDirectory(prefix='zebkit-http-auth-bindings-') as profile:
        result=subprocess.run([sys.argv[1],'--no-sandbox','--headless','--disable-gpu','--no-proxy-server',
                               '--user-data-dir='+profile+'/javascript',str(script),fixture.url,
                               fixture.username,fixture.password,mode],capture_output=True,text=True,timeout=60)
        print(result.stdout,end='');print(result.stderr,end='',file=sys.stderr)
        assert result.returncode==0,result.returncode
        assert 'ok JavaScript HTTP auth '+mode in result.stdout,'JavaScript did not finish'
        rust=os.environ.get('ZEBKIT_HTTP_AUTH_RUST_EXAMPLE')
        if rust:
            result=subprocess.run([rust,'--no-sandbox','--headless','--disable-gpu','--no-proxy-server',
                                   '--user-data-dir='+profile+'/rust'],
                                  env=dict(os.environ,ZEBKIT_HTTP_AUTH_URL=fixture.url,
                                           ZEBKIT_HTTP_AUTH_USER=fixture.username,
                                           ZEBKIT_HTTP_AUTH_PASSWORD=fixture.password,ZEBKIT_HTTP_AUTH_CASE=mode),
                                  capture_output=True,text=True,timeout=60)
            print(result.stdout,end='');print(result.stderr,end='',file=sys.stderr)
            assert result.returncode==0,result.returncode
            assert 'ok Rust HTTP auth '+mode in result.stdout,'Rust did not finish'
