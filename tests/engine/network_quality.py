"""Chromium NQE estimates reach existing and newly created renderers.

    python3 tests/engine/network_quality.py <3G|Slow-2G> [engine switches...]

Exit 0 when every check passes.
"""
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from harness import check, ev, finish, settled, wait, zebkit  # noqa: E402

READ = '({rtt:navigator.connection.rtt,downlink:navigator.connection.downlink,type:navigator.connection.effectiveType})'


def page():
    view = zebkit.View()
    loaded = []
    view.on_load(lambda event, url: loaded.append(True) if event == zebkit.LOAD_FINISHED and url.startswith('data:') else None)
    view.load('data:text/html,<title>network quality</title>')
    return view if wait(lambda: loaded) else None


def main():
    kind = sys.argv[1]
    if zebkit.init([sys.argv[0], *sys.argv[2:], '--force-effective-connection-type=' + kind],
                   settings={'logging': {'destination': 'none'}}) != 0:
        print('FAIL zebkit.init()')
        return 1
    try:
        expected = '3g' if kind == '3G' else 'slow-2g'
        first = page()
        if not check(first is not None, 'the first page loaded'):
            return finish()
        got = {}

        def current():
            got['value'] = ev(first, READ)
            return isinstance(got['value'], dict) and got['value'].get('type') == expected
        # The estimate reaches the renderer a little after its first page.
        check(wait(current, 10), 'the existing renderer has effectiveType ' + expected, got.get('value'))
        second = page()
        check(second is not None, 'a new page loaded')
        for name, view in (('the existing renderer', first), ('a new renderer', second)):
            got = ev(view, READ) if view else None
            if kind == '3G':
                good = (isinstance(got, dict) and got['type'] == expected and 300 <= got['rtt'] <= 600
                        and .25 <= got['downlink'] <= .55)
            else:
                # Chromium's typical HTTP RTT is 3600 ms, capped to 3000 for web exposure.
                good = (isinstance(got, dict) and got['type'] == expected and got['rtt'] == 3000
                        and .025 <= got['downlink'] <= .075)
            check(good and settled(got), 'Chromium NQE %s in %s: %s' % (kind, name, json.dumps(got)))
    finally:
        check(zebkit.shutdown(), 'shutdown')
    return finish()


if __name__ == '__main__':
    sys.exit(main())
