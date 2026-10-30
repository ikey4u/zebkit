"""Run HTTP/2-only TLS on Node; assert denied requests never reach the server."""
import json
import os
from pathlib import Path
import subprocess
import sys
import threading
from tls_fixture import TLSFixture

node=subprocess.check_output([*sys.argv[1:],'-p','process.execPath'],text=True,timeout=30).strip()
tests=Path(__file__).parent
with TLSFixture() as fixture:
    work=Path(fixture.work.name)
    server=subprocess.Popen([node,str(tests/'certificate_h2_server.mjs'),str(work/'key.pem'),str(work/'certificate.pem')],
                            stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
    thread=None
    try:
        ready=json.loads(server.stdout.readline())
        assert ready['kind']=='ready',ready
        received=[]
        def read():
            for line in server.stdout:
                received.append(json.loads(line))
        thread=threading.Thread(target=read,daemon=True);thread.start()
        result=subprocess.run([sys.executable,str(tests/'certificate_h2_client.py'),'--no-sandbox','--headless','--disable-gpu',
                               '--user-data-dir='+str(work/'h2-profile')],
                              env=dict(os.environ,ZEBKIT_CERTIFICATE_TEST_URL=f"https://localhost:{ready['port']}"),
                              capture_output=True,text=True,timeout=90)
        print(result.stdout,end='');print(result.stderr,end='',file=sys.stderr)
        assert result.returncode==0,result.returncode
        assert 'ok HTTP2 certificate policy:' in result.stdout,'client did not finish'
        paths={item['path']:item['session'] for item in received}
        assert {'/allowed','/fetch-allow','/concurrent-allow'}<=paths.keys(),paths
        assert '/denied' not in paths and '/concurrent-deny' not in paths,paths
        print('ok HTTP2 denied headers/body never sent; server session IDs:',json.dumps(paths),flush=True)
    finally:
        server.terminate()
        try:server.wait(timeout=10)
        except subprocess.TimeoutExpired:server.kill();server.wait(timeout=10)
        if thread:thread.join(timeout=5)
