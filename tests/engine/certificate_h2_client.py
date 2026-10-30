"""Native certificate policy over the local HTTP/2 fixture."""
import os
import sys
import time
import traceback
import zebkit

base=os.environ['ZEBKIT_CERTIFICATE_TEST_URL']
requests,errors=[],[]
def wait(predicate,label):
    deadline=time.monotonic()+20
    while not predicate() and time.monotonic()<deadline:
        assert not errors,errors
        assert zebkit.pump()
        time.sleep(.001)
    assert not errors,errors
    assert predicate(),label

def evaluate(view,code):
    replies=[]
    view.evaluate(code,lambda value,error:replies.append((value,error)))
    wait(lambda:bool(replies),code)
    assert replies[0][1] is None,replies
    return replies[0][0]

assert zebkit.init([sys.argv[0],*sys.argv[1:]],settings={'logging':{'destination':'none'}})==0
try:
    first,other=zebkit.View(),zebkit.View()
    def certificate(request):
        try:
            requests.append(request.metadata)
            assert request.info==request.metadata and request.metadata['url'].startswith(base)
            assert request.respond(True) and not request.respond(False) and request.info is None
        except BaseException:errors.append(traceback.format_exc())
    first.on_certificate(certificate)
    first.set_certificate_timeout(5000)
    events=[]
    first.on_load(lambda event,url:events.append((event,url)))
    first.load(base+'/allowed')
    wait(lambda:(zebkit.LOAD_FINISHED,base+'/allowed') in events,'HTTP2 permitted navigation')
    assert first.title=='certificate-ok'
    assert 'certificate-ok' in evaluate(first,"fetch('/fetch-allow').then(r=>r.text())")
    assert any(value['url']==base+'/fetch-allow' and not value['mainFrame'] for value in requests)
    denied=[]
    other.on_load(lambda event,url:denied.append((event,url)))
    other.load(base+'/denied')
    wait(lambda:(zebkit.LOAD_FAILED,base+'/denied') in denied,'HTTP2 default denial')
    first.load(base+'/concurrent-allow')
    other.load(base+'/concurrent-deny')
    wait(lambda:(zebkit.LOAD_FINISHED,base+'/concurrent-allow') in events and
                (zebkit.LOAD_FAILED,base+'/concurrent-deny') in denied,'HTTP2 concurrent view policies')
    first.close();other.close()
    print('ok HTTP2 certificate policy: navigation, subresource, concurrent view isolation and single-use replies',flush=True)
finally:
    assert zebkit.shutdown()
