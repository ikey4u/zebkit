"""Public Python HTTP-auth objects keep callbacks alive through self-removal."""
import time
import traceback
import tempfile
import zebkit
from http_auth_fixture import AuthFixture

errors=[]
def wait(condition,label):
    deadline=time.monotonic()+15
    while time.monotonic()<deadline:
        assert not errors,errors
        assert zebkit.pump()
        if condition(): return
        time.sleep(.002)
    raise AssertionError(label)

with tempfile.TemporaryDirectory(prefix='zebkit-http-auth-python-') as profile:
    assert zebkit.init(['http-auth-python','--no-sandbox','--headless','--disable-gpu',
                        '--no-proxy-server',f'--user-data-dir={profile}'])==0
    try:
        for mode in ('allow','cancel-handler','close-request','close-result'):
            with AuthFixture() as fixture:
                view=zebkit.View()
                for invalid in (0,300001,True,1.5):
                    try: view.set_http_auth_timeout(invalid)
                    except ValueError: pass
                    else: raise AssertionError('invalid timeout accepted')
                for setter in (view.on_http_auth,view.on_http_auth_result):
                    try: setter(1)
                    except TypeError: pass
                    else: raise AssertionError('invalid callback accepted')
                view.set_http_auth_timeout(5000)
                kept,results,loads=[],[],[]
                view.on_load(lambda phase,url:loads.append(phase))
                def challenge(request):
                    try:
                        assert isinstance(request,zebkit.HttpAuthRequest)
                        kept.append(request)
                        assert request.info==request.metadata and fixture.password not in str(request.metadata)
                        for user,password in ((None,'x'),(1,'x'),('bad\0','x'),('x'*65537,'x')):
                            try: request.respond(user,password)
                            except (TypeError,ValueError): pass
                            else: raise AssertionError('invalid credentials accepted')
                        assert request.info is not None
                        if mode=='cancel-handler':
                            view.on_http_auth(None)
                            assert request.info is None and not request.respond(fixture.username,fixture.password)
                        elif mode=='close-request': view.close()
                        else:
                            assert request.respond(fixture.username,fixture.password)
                            assert request.info is None and not request.respond()
                            view.on_http_auth(None)
                    except BaseException: errors.append(traceback.format_exc())
                def result(metadata):
                    try:
                        results.append(metadata)
                        assert fixture.password not in str(metadata)
                        view.on_http_auth_result(None)
                        if mode=='close-result': view.close()
                    except BaseException: errors.append(traceback.format_exc())
                view.on_http_auth(challenge)
                view.on_http_auth_result(result)
                view.load(fixture.url+'/auth/python-'+mode)
                if mode in ('close-request','close-result'):
                    wait(lambda:view._closed,mode)
                elif mode=='allow':
                    wait(lambda:bool(results) and view.title=='auth-ok','Python authenticated')
                    assert results[0]['provided'] is True
                else:
                    wait(lambda:bool(results) and (view.title=='auth-denied' or zebkit.LOAD_FAILED in loads),'Python cancellation')
                    assert results[0]['provided'] is False
                assert kept and kept[0].info is None and not kept[0].respond()
                if not view._closed:
                    view.close();wait(lambda:view._closed,'Python view closed')
                print('ok Python HTTP auth '+mode,flush=True)
        assert not errors,errors
    finally: assert zebkit.shutdown()
