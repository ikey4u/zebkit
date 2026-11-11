"""Exercise host JS and optional Rust on a private real HTTP document."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
from http_auth_fixture import AuthFixture


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


with AuthFixture() as fixture, tempfile.TemporaryDirectory(prefix='zebkit-ua-bindings-') as profile:
    switches = ['--no-sandbox','--headless','--disable-gpu','--no-proxy-server']
    child = subprocess.run([sys.argv[1],*switches,'--user-data-dir='+profile+'/js',
                            str(Path(__file__).parent/'script/user_agent.mjs'),fixture.url+'/public'],
                           capture_output=True,text=True,timeout=60)
    print(child.stdout,end='');print(child.stderr,end='',file=sys.stderr)
    assert child.returncode==0,child.returncode
    assert 'ok JavaScript UA/client hints and atomic rejection' in child.stdout
    if rust := rust_example('ZEBKIT_UA_RUST_EXAMPLE'):
        child = subprocess.run([rust,*switches,'--user-data-dir='+profile+'/rust'],
                               env=dict(os.environ,ZEBKIT_UA_TEST_URL=fixture.url+'/public'),
                               capture_output=True,text=True,timeout=60)
        print(child.stdout,end='');print(child.stderr,end='',file=sys.stderr)
        assert child.returncode==0,child.returncode
        assert 'ok Rust UA/client hints and atomic rejection' in child.stdout
