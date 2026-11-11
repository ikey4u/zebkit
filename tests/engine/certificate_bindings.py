"""Run standalone host-language examples against one private HTTPS fixture."""
import os
from pathlib import Path
import subprocess
import sys
from tls_fixture import TLSFixture


def rust_example(variable):
    """The Rust example's path from @variable; without one a visible skip, or a
    failure when ZEBKIT_TEST_REQUIRE_RUST=1 (the suite built the examples)."""
    path=os.environ.get(variable)
    if path:
        return path
    if os.environ.get('ZEBKIT_TEST_REQUIRE_RUST')=='1':
        print('FAIL rust: '+variable+' is not set, and ZEBKIT_TEST_REQUIRE_RUST=1',flush=True)
        sys.exit(1)
    print('skip rust: '+variable+' is not set',flush=True)
    return None


with TLSFixture() as fixture:
    command=[sys.argv[1],'--no-sandbox','--headless','--disable-gpu',
             '--user-data-dir='+fixture.work.name+'/javascript-profile',
             str(Path(__file__).parent/'script/certificate.mjs'),fixture.url]
    result=subprocess.run(command,capture_output=True,text=True,timeout=60)
    print(result.stdout,end='');print(result.stderr,end='',file=sys.stderr)
    assert result.returncode==0,result.returncode
    assert 'ok JavaScript certificate policy:' in result.stdout,'script did not finish'
    rust=rust_example('ZEBKIT_CERTIFICATE_RUST_EXAMPLE')
    if rust:
        result=subprocess.run([rust,'--no-sandbox','--headless','--disable-gpu',
                               '--user-data-dir='+fixture.work.name+'/rust-profile'],
                              env=dict(os.environ,ZEBKIT_CERTIFICATE_TEST_URL=fixture.url),
                              capture_output=True,text=True,timeout=60)
        print(result.stdout,end='');print(result.stderr,end='',file=sys.stderr)
        assert result.returncode==0,result.returncode
        assert 'ok Rust certificate policy:' in result.stdout,'Rust example did not finish'
