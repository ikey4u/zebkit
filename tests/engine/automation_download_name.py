"""The automation's Page.download names the file as Chrome's DevTools does for
Playwright (download.suggestedFilename()): from Content-Disposition when only
that names it, as soon as the download starts.

    python3 tests/engine/automation_download_name.py [engine switches...]

Exit 0 when every check passes.
"""

import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

from harness import ev, finish, must_wait, part, wait, zebkit  # noqa: E402
from hook_fixture import HookFixture  # noqa: E402

CLICK = ("() => { const a = document.createElement('a'); a.href = '%s'; document.body.append(a); a.click(); }")


def main():
    if zebkit.init(['automation-download-name', *sys.argv[1:]], settings={'logging': {'destination': 'none'}}) != 0:
        print('FAIL zebkit.init()')
        return 1
    from zebkit import automation
    fixture = HookFixture()
    try:
        view = zebkit.View()
        loads = []
        view.on_load(lambda phase, url: loads.append(phase))
        view.load(fixture.base + '/page')
        must_wait(lambda: zebkit.LOAD_FINISHED in loads and ev(view, 'document.readyState') == 'complete', 'loaded')
        events = []
        automation.on_event(view, lambda name, params: events.append((name, params)))
        with part("only Content-Disposition names it: Page.download says that name"):
            done = automation.call(view, 'Frame.evaluateExpression',
                                   {'expression': CLICK % '/download.bin', 'isFunction': True})
            must_wait(lambda: done.done(), 'the click')
            must_wait(lambda: any(name == 'Page.download' for name, _ in events), 'Page.download')
            params = next(params for name, params in events if name == 'Page.download')
            assert params.get('suggestedFilename') == 'report.bin', params
            assert params.get('url', '').endswith('/download.bin'), params
        automation.on_event(view, None)
        view.close()
        wait(lambda: False, 0.2)
    finally:
        zebkit.shutdown()
        fixture.close()
    return finish()


if __name__ == '__main__':
    sys.exit(main())
