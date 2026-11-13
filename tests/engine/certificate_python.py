"""A document's TLS errors through the Python binding's View.on_certificate and
CertificateRequest, one mode an engine; run by certificate.py, which has the
modes and the server (ZEBKIT_CERTIFICATE_TEST_URL, ZEBKIT_CERTIFICATE_CASE).

    python3 tests/engine/certificate_python.py [engine switches...]
"""

import os
import sys
import threading

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from harness import check, finish, must_wait, part, wait, zebkit  # noqa: E402

BASE, MODE = os.environ['ZEBKIT_CERTIFICATE_TEST_URL'], os.environ['ZEBKIT_CERTIFICATE_CASE']
HOLD = ('hold', 'timeout', 'navigation', 'stop')


def rejected(function, kinds):
    try:
        function()
    except kinds:
        return True
    return False


class Watched:
    """A view with what its loads, requests, outcomes and closing were."""

    def __init__(self):
        self.view = zebkit.View()
        self.loads, self.requests, self.outcomes, self.closed = [], [], [], []
        self.view.on_load(lambda event, url: self.loads.append((event, url)))
        self.view.on_close(lambda: self.closed.append(True))

    def failed(self, url):
        return (zebkit.LOAD_FAILED, url) in self.loads

    def finished(self, url):
        return (zebkit.LOAD_FINISHED, url) in self.loads


def main():
    if zebkit.init(['certificate-python', *sys.argv[1:]], settings={'logging': {'destination': 'none'}}) != 0:
        print('FAIL zebkit.init()')
        return 1
    try:
        first, other = Watched(), Watched()
        view = first.view
        check(all(rejected(lambda: view.set_certificate_timeout(bad), ValueError) for bad in (0, 300001, True, 1.5)),
              'set_certificate_timeout refuses 0, 300001, True and 1.5')
        check(all(rejected(lambda: setter(1), TypeError) for setter in (view.on_certificate, view.on_certificate_result)),
              'on_certificate and on_certificate_result refuse what is not callable')
        view.set_certificate_timeout(200 if MODE == 'timeout' else 5000)

        def certificate(request):
            first.requests.append(request)
            metadata = request.metadata
            if len(first.requests) == 1:
                check(isinstance(request, zebkit.CertificateRequest) and request.info == metadata,
                      'the request: its info is its metadata', None if request.info == metadata else metadata)
                check(metadata['state'] == 'pending' and metadata['error'] < 0
                      and metadata['certificateChain'][0].startswith('-----BEGIN CERTIFICATE-----')
                      and metadata['subject'] == 'localhost' and metadata['issuer'] == 'localhost'
                      and metadata['validFromUnixMs'] < metadata['validUntilUnixMs']
                      and not metadata['knownRoot'] and not metadata['chainTruncated'],
                      'the metadata: the error, the chain, its names and dates')
                check(rejected(lambda: request.respond(1), TypeError) and request.info is not None,
                      'a reply that is not a boolean is refused, and the request stays')
            if MODE in HOLD:
                return
            if MODE == 'strict':
                check(metadata['strictEnforcement'] and not metadata['canIgnore'], 'a pinned host: strict', metadata)
                check(not request.respond(True) and request.info is not None and request.respond(False),
                      'its error cannot be allowed, only refused')
            elif MODE == 'cancel-handler':
                view.on_certificate(None)
                check(request.info is None and not request.respond(True), 'removed from inside it: the request is gone')
            elif MODE == 'close-request':
                view.close()
            elif MODE == 'allow' or (MODE == 'cache' and len(first.requests) == 1):
                check(metadata['canIgnore'] and not metadata['strictEnforcement'], 'an error that can be allowed')
                check(request.respond(True) and not request.respond(False) and request.info is None,
                      'allowed once: the request is gone, a second reply refused')
            else:
                check(request.respond(False) and not request.respond(True) and request.info is None,
                      'refused once: the request is gone, a second reply refused')

        def result(metadata):
            first.outcomes.append(metadata)
            if MODE == 'close-result':
                view.on_certificate_result(None)
                view.close()

        if MODE != 'default':
            view.on_certificate(certificate)
        view.on_certificate_result(result)
        url = BASE + '/python-' + MODE
        with part('python %s: the page as the mode makes it' % MODE):
            if MODE == 'post':
                page = ("data:text/html,<form method='post' action='%s'><input name='value' value='private'></form>"
                        "<script>document.forms[0].submit()</script>" % url)
                view.load(page)
                must_wait(lambda: any(r.metadata['url'] == url for r in first.requests), 'the POST asked for')
                must_wait(lambda: first.failed(url), 'the POST refused')
            elif MODE == 'cache':
                view.load(url)
                must_wait(lambda: first.finished(url) and view.title == 'certificate-ok', 'the allowed, cached page')
                # Another view, its own function refusing: asked again, nothing sent.
                asked = []
                other.view.on_certificate(lambda request: (asked.append(request), request.respond(False)))
                other.view.load(url)
                must_wait(lambda: other.failed(url), 'the other view refused')
                assert len(asked) == 1 and asked[0].metadata['url'] == url, [r.metadata for r in asked]
            else:
                view.load(url)
                if MODE in ('close-request', 'close-result'):
                    must_wait(lambda: first.closed, 'the view closed')
                    assert MODE == 'close-request' or first.outcomes[0]['reason'] == 'answered', first.outcomes
                elif MODE == 'allow':
                    must_wait(lambda: view.title == 'certificate-ok', 'the allowed page')
                    must_wait(lambda: any(o['state'] == 'continued' for o in first.outcomes), 'the continued outcome')
                    assert first.requests[0].metadata['url'] == url
                    # Once only: another view of the profile is refused, without being asked.
                    other.view.load(url + '-other')
                    must_wait(lambda: other.failed(url + '-other'), 'the other view refused')
                    assert len(first.requests) == 1
                elif MODE in HOLD:
                    must_wait(lambda: first.requests, 'the request, held')
                    request = first.requests[0]
                    if MODE == 'hold':
                        off_thread = []
                        thread = threading.Thread(target=lambda: off_thread.append(
                            (request.info, request.respond(True))))
                        thread.start()
                        thread.join()
                        assert off_thread == [(None, False)] and request.info is not None, off_thread
                        assert request.respond(False)
                        must_wait(lambda: any(o['id'] == request.metadata['id'] and o['state'] == 'denied'
                                              for o in first.outcomes), 'the later refusal')
                    else:
                        if MODE == 'navigation':
                            view.load('about:blank')
                        elif MODE == 'stop':
                            view.stop()
                        must_wait(lambda: request.info is None, 'the request ended')
                        assert not request.respond(True)
                        if MODE == 'timeout':
                            must_wait(lambda: any(o['id'] == request.metadata['id'] and o['reason'] == 'timeout'
                                                  for o in first.outcomes), 'the timeout outcome')
                else:
                    must_wait(lambda: first.failed(url), 'the page refused', 30)
                    if MODE == 'default':
                        assert not first.requests
                    elif MODE == 'deny':
                        must_wait(lambda: any(o['reason'] == 'answered' for o in first.outcomes), 'the outcome')
                    elif MODE == 'cancel-handler':
                        must_wait(lambda: any(o['reason'] == 'handler-changed' for o in first.outcomes),
                                  'the handler-changed outcome')
        check(all(r.info is None and not r.respond(True) for r in first.requests),
              'python %s: every request settled, none answerable' % MODE)
        for watched in (first, other):
            if not watched.closed:
                watched.view.close()
        check(wait(lambda: first.closed and other.closed), 'the views close')
    finally:
        zebkit.shutdown()
    return finish()


if __name__ == '__main__':
    sys.exit(main())
