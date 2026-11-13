"""A page's console through the Python binding's View.on_console, one mode an
engine; run by console.py, which has the modes and the server
(ZEBKIT_CONSOLE_BASE, ZEBKIT_CONSOLE_CASE).

    python3 tests/engine/console_python.py [engine switches...]
"""

import os
import sys
import threading

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from harness import check, ev, finish, must_wait, part, pump, refused, wait, zebkit  # noqa: E402

BASE, MODE = os.environ['ZEBKIT_CONSOLE_BASE'], os.environ['ZEBKIT_CONSOLE_CASE']


def load(view, path):
    view.load(BASE + path)
    must_wait(lambda: view.url == BASE + path and ev(view, 'document.readyState') == 'complete', path)


def main():
    if zebkit.init(['console-python', *sys.argv[1:]], settings={'logging': {'destination': 'none'}}) != 0:
        print('FAIL zebkit.init()')
        return 1
    try:
        first, other = zebkit.View(), zebkit.View()
        closed = []
        first.on_close(lambda: closed.append(True))
        load(first, '/initial')
        load(other, '/other')
        events = []
        first.on_console(events.append)
        messages = lambda: [e['message'] for e in events]  # noqa: E731
        with part('console %s' % MODE):
            if MODE == 'levels':
                ev(first, "console.log('中文😀\\u0000tail'); console.warn('warning-text'); console.error('error-text'); "
                          "true\n//# sourceURL=native-console-test.js")
                must_wait(lambda: len(events) >= 3, 'three messages')
                recorded = {e['message']: e for e in events}
                assert [recorded[m]['level'] for m in ('中文😀\0tail', 'warning-text', 'error-text')] == \
                    ['info', 'warning', 'error'], recorded
                sample = recorded['warning-text']
                assert sample['origin'] == BASE and sample['mainFrame'] is True, sample
                assert sample['frameId'].startswith('f') and sample['line'] >= 1, sample
                assert sample['source'].endswith('native-console-test.js'), sample
                assert sample['stackTrace'] is None or isinstance(sample['stackTrace'], str), sample
                assert not sample['truncated'] and type(sample['dropped']) in (int, float) and sample['dropped'] >= 0, sample
            elif MODE == 'bounds':
                ev(first, "console.log('x'.repeat(65535)+'😀tail'); true")
                must_wait(lambda: any(e['truncated'] for e in events), 'a bounded message')
                assert next(e for e in events if e['truncated'])['message'] == 'x' * 65535
            elif MODE == 'frame':
                ev(first, "console.log('main'); true")
                must_wait(lambda: events, 'the main frame\'s message')
                load(first, '/frame')
                must_wait(lambda: 'cross-site child' in messages(), 'the other site\'s frame\'s message')
                child = next(e for e in events if e['message'] == 'cross-site child')
                assert not child['mainFrame'] and child['origin'] == BASE.replace('127.0.0.1', 'localhost'), child
                assert child['frameId'] != events[0]['frameId'], child
            elif MODE == 'isolation':
                ev(other, "console.warn('other-view'); true")
                ev(first, "console.warn('this-view'); true")
                must_wait(lambda: 'this-view' in messages(), 'this view\'s message')
                assert 'other-view' not in messages(), messages()
                # Set from another thread: refused there, the function stays.
                errors = []
                thread = threading.Thread(target=lambda: errors.append(refused(lambda: first.on_console(None))))
                thread.start()
                thread.join()
                assert errors[0], errors
                ev(first, "console.warn('still'); true")
                must_wait(lambda: 'still' in messages(), 'the function still there')
            elif MODE == 'replace':
                swapped = []

                def swap(metadata):
                    first.on_console(swapped.append)
                first.on_console(swap)
                ev(first, "console.log('swap-now'); true")
                ev(first, "console.log('after-swap'); true")
                must_wait(lambda: swapped, 'the replacement')
                assert [e['message'] for e in swapped] == ['after-swap'], swapped
            elif MODE == 'remove':
                def removed(metadata):
                    events.append(metadata)
                    first.on_console(None)
                first.on_console(removed)
                ev(first, "console.warn('once'); true")
                must_wait(lambda: events, 'the message')
                ev(first, "console.log('after'); true")
                pump()
                assert messages() == ['once'], messages()
            elif MODE == 'close':
                first.on_console(lambda metadata: first.close())
                first.evaluate("console.log('close-now'); true")
                must_wait(lambda: closed, 'closed from the function')
                assert refused(lambda: first.on_console(None)), 'a closed view took a function'
            elif MODE == 'validation':
                for invalid in (False, 1, 'callback'):
                    try:
                        first.on_console(invalid)
                    except TypeError:
                        pass
                    else:
                        raise AssertionError('%r taken as a function' % (invalid,))
                ev(first, "console.warn('kept'); true")
                must_wait(lambda: 'kept' in messages(), 'the function kept')
                assert events[-1]['level'] == 'warning', events
        other.close()
        if not closed:
            first.close()
        check(wait(lambda: closed), 'the views close')
    finally:
        zebkit.shutdown()
    return finish()


if __name__ == '__main__':
    sys.exit(main())
