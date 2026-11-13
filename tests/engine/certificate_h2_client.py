"""The client of certificate_h2.py: the Python binding's certificate policy over
its HTTP/2 server (ZEBKIT_CERTIFICATE_TEST_URL).

    python3 tests/engine/certificate_h2_client.py [engine switches...]
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from harness import check, ev, finish, must_wait, part, zebkit  # noqa: E402

BASE = os.environ['ZEBKIT_CERTIFICATE_TEST_URL']


def main():
    if zebkit.init(['certificate-h2', *sys.argv[1:]], settings={'logging': {'destination': 'none'}}) != 0:
        print('FAIL zebkit.init()')
        return 1
    try:
        first, other = zebkit.View(), zebkit.View()
        requests, answers = [], []

        def certificate(request):
            requests.append(request.metadata)
            answers.append(request.info == request.metadata and request.metadata['url'].startswith(BASE)
                           and request.respond(True) and not request.respond(False) and request.info is None)
        first.on_certificate(certificate)
        first.set_certificate_timeout(5000)
        events, denied = [], []
        first.on_load(lambda event, url: events.append((event, url)))
        other.on_load(lambda event, url: denied.append((event, url)))
        with part('a navigation allowed'):
            first.load(BASE + '/allowed')
            must_wait(lambda: (zebkit.LOAD_FINISHED, BASE + '/allowed') in events, 'the allowed page')
            assert first.title == 'certificate-ok'
        with part('a fetch of it, asked for and allowed'):
            answer = ev(first, "fetch('/fetch-allow').then(r=>r.text())")
            assert isinstance(answer, str) and 'certificate-ok' in answer, answer
            assert any(value['url'] == BASE + '/fetch-allow' and not value['mainFrame'] for value in requests), requests
        with part('another view, without a function: refused'):
            other.load(BASE + '/denied')
            must_wait(lambda: (zebkit.LOAD_FAILED, BASE + '/denied') in denied, 'the refusal')
        with part('both views at once, each its own policy'):
            first.load(BASE + '/concurrent-allow')
            other.load(BASE + '/concurrent-deny')
            must_wait(lambda: (zebkit.LOAD_FINISHED, BASE + '/concurrent-allow') in events
                      and (zebkit.LOAD_FAILED, BASE + '/concurrent-deny') in denied, 'the two loads')
        check(bool(answers) and all(answers), 'every request: its metadata, one reply', None if answers and all(answers) else requests)
        first.close()
        other.close()
    finally:
        zebkit.shutdown()
    return finish()


if __name__ == '__main__':
    sys.exit(main())
