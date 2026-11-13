"""A shared or service worker's WebSocket is decided by its own context's
policy (zebkit_view_set_network_filter_for_workers): with two contexts that
each have one, each socket goes to the view of its own context, with
workerType "shared" or "service"; a context without a policy lets its
workers' sockets through, asking no one.

    python3 tests/engine/worker_websocket.py [engine switches...]

Exit 0 when every check passes.
"""

import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

from harness import check, ev, finish, must_wait, part, zebkit  # noqa: E402
from hook_fixture import HookFixture  # noqa: E402

SHARED = ("new Promise(ok => { const w = new SharedWorker('/net-shared.js'); w.onerror = () => ok('refused'); "
          "w.port.onmessage = e => ok(e.data); w.port.postMessage('socket'); })")
SERVICE = ("navigator.serviceWorker.register('/net-sw.js').then(r => new Promise(ok => { "
           "const w = r.installing || r.waiting || r.active; if (w.state === 'activated') return ok(w); "
           "w.addEventListener('statechange', () => { if (w.state === 'activated') ok(w); }); }))"
           ".then(w => new Promise(ok => { navigator.serviceWorker.onmessage = e => ok(e.data); "
           "w.postMessage('socket'); }))")


def opened(url, context=None):
    view = zebkit.View(context=context)
    loads = []
    view.on_load(lambda phase, u: loads.append(phase))
    view.load(url)
    must_wait(lambda: zebkit.LOAD_FINISHED in loads and ev(view, 'document.readyState') == 'complete',
              'loaded ' + url)
    return view


def policy(view, asked, answer):
    """view decides for its context's workers, recording what it is asked."""
    view.set_network_filter([{'url': '**/ws'}])

    def decide(request):
        asked.append(request.metadata)
        request.respond(answer)
    view.on_network(decide)
    return view.set_network_filter_for_workers(True)


def main():
    if zebkit.init(['worker-websocket', *sys.argv[1:]], settings={'logging': {'destination': 'none'}}) != 0:
        print('FAIL zebkit.init()')
        return 1
    fixture = HookFixture()
    second, third = zebkit.Context(), zebkit.Context()
    try:
        page = fixture.base + '/page'
        first_view, second_view = opened(page), opened(page, second)
        first_asked, second_asked = [], []
        check(policy(first_view, first_asked, 'continue') and policy(second_view, second_asked, 'block'),
              'a policy for the workers of each of two contexts')
        with part("the second context's shared worker: its own view, which blocks"):
            assert ev(second_view, SHARED) == 'failed'
            assert len(second_asked) == 1 and not first_asked, (first_asked, second_asked)
            asked = second_asked[0]
            assert asked['resourceType'] == 'websocket' and asked['worker'] and asked['workerType'] == 'shared', asked
            assert asked['frameId'] == '' and asked['url'].endswith('/ws'), asked
        with part("the default context's shared worker: the first view, which lets it through"):
            assert ev(first_view, SHARED) == 'hello'
            assert len(first_asked) == 1 and len(second_asked) == 1, (first_asked, second_asked)
            assert first_asked[0]['workerType'] == 'shared', first_asked
        with part("the second context's service worker: its own view, as a service worker's"):
            assert ev(second_view, SERVICE) == 'failed'
            assert len(second_asked) == 2 and len(first_asked) == 1, (first_asked, second_asked)
            assert second_asked[1]['workerType'] == 'service' and second_asked[1]['worker'], second_asked
        with part('a context without a policy: through, asking no one'):
            third_view = opened(page, third)
            before = fixture.received.count('/ws')
            assert ev(third_view, SHARED) == 'hello'
            assert fixture.received.count('/ws') == before + 1, fixture.received
            assert len(first_asked) == 1 and len(second_asked) == 2, (first_asked, second_asked)
        with part("a policy whose view has gone refuses that context's workers only"):
            second_view.close()
            again = opened(page, second)
            before = fixture.received.count('/ws')
            # The worker's own script is refused with it, unasked.
            refused = ev(again, SHARED.replace('/net-shared.js', '/net-shared.js?again'))
            assert refused == 'refused' and len(second_asked) == 2, (refused, second_asked)
            assert fixture.received.count('/ws') == before, fixture.received
            through = ev(first_view, SHARED.replace('/net-shared.js', '/net-shared.js?again'))
            assert through == 'hello' and len(first_asked) == 2, (through, first_asked)
            again.set_network_filter([])
            check(again.set_network_filter_for_workers(True), 'another view of the context takes over')
    finally:
        second.close()
        third.close()
        zebkit.shutdown()
        fixture.close()
    return finish()


if __name__ == '__main__':
    sys.exit(main())
