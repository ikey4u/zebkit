"""Run standalone host-language examples against one private HTTPS fixture."""
import os
from pathlib import Path
import subprocess
import sys
from tls_fixture import TLSFixture

with TLSFixture() as fixture:
    command=[sys.argv[1],'--no-sandbox','--headless','--disable-gpu',
             '--user-data-dir='+fixture.work.name+'/javascript-profile',
             str(Path(__file__).parent/'script/certificate.mjs'),fixture.url]
    result=subprocess.run(command,capture_output=True,text=True,timeout=60)
    print(result.stdout,end='');print(result.stderr,end='',file=sys.stderr)
    assert result.returncode==0,result.returncode
    assert 'ok JavaScript certificate policy:' in result.stdout,'script did not finish'
    rust=os.environ.get('ZEBKIT_CERTIFICATE_RUST_EXAMPLE')
    if rust:
        result=subprocess.run([rust,'--no-sandbox','--headless','--disable-gpu',
                               '--user-data-dir='+fixture.work.name+'/rust-profile'],
                              env=dict(os.environ,ZEBKIT_CERTIFICATE_TEST_URL=fixture.url),
                              capture_output=True,text=True,timeout=60)
        print(result.stdout,end='');print(result.stderr,end='',file=sys.stderr)
        assert result.returncode==0,result.returncode
        assert 'ok Rust certificate policy:' in result.stdout,'Rust example did not finish'
