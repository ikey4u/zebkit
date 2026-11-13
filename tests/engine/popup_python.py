"""A page's new windows through the Python binding (View.on_window_open,
on_window_open_event, on_popup_blocked, set_popup_policy, window_open_info),
one mode an engine; run by popup.py, which has the modes and the server
(ZEBKIT_POPUP_BASE, ZEBKIT_POPUP_CASE).

    python3 tests/engine/popup_python.py [engine switches...]
"""

import os
import sys
import threading

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from harness import check, ev, finish, must_wait, part, refused, wait, zebkit  # noqa: E402

BASE, MODE = os.environ['ZEBKIT_POPUP_BASE'], os.environ['ZEBKIT_POPUP_CASE']
FLAGS = ('hasX', 'hasY', 'hasWidth', 'hasHeight', 'popup')


def load(view, path):
    view.load(BASE + path)
    must_wait(lambda: view.url == BASE + path and ev(view, 'document.readyState') == 'complete', path)


def main():
    if zebkit.init(['popup-python', *sys.argv[1:]], settings={'logging': {'destination': 'none'}}) != 0:
        print('FAIL zebkit.init()')
        return 1
    try:
        first, other = zebkit.View(), zebkit.View()
        closed = []
        first.on_close(lambda: closed.append(True))
        check(first.window_open_info == {}, 'a view of the application\'s: no creation snapshot')
        answers = []
        thread = threading.Thread(target=lambda: answers.append(refused(lambda: first.window_open_info)))
        thread.start()
        thread.join()
        check(bool(answers[0]), 'the snapshot refused off the engine\'s thread', answers)
        load(first, '/initial')
        events, legacy, windows, gone = [], [], [], set()
        first.on_window_open_event(events.append)
        first.on_popup_blocked(legacy.append)

        def created(child, url):
            creation = child.window_open_info
            check(creation['phase'] == 'created' and creation['url'] == url,
                  'the window\'s snapshot, made at its creation: %s' % url, creation)
            serial = len(windows) + 1
            child.on_close(lambda: gone.add(serial))
            windows.append((serial, child, creation))
            if creation['frameName'] == 'close-immediately':
                child.close()
        if MODE != 'adoption':
            first.on_window_open(created)
        with part('popup %s' % MODE):
            if MODE in ('blocked', 'isolation', 'remove', 'frame'):
                first.set_popup_policy('block')
            if MODE == 'blocked':
                load(first, '/blocked')
                must_wait(lambda: events and legacy, 'the decision and the older event')
                decision = events[-1]
                assert decision['phase'] == 'decision' and decision['allowed'] is False, decision
                assert decision['url'] == BASE + '/popup-blocked' and decision['frameName'] == 'blocked', decision
                assert decision['userGesture'] is False and decision['disposition'] == 'new-popup', decision
                assert decision['openerUrl'] == decision['topUrl'] == BASE + '/blocked', decision
                assert decision['sourceOrigin'] == BASE and not decision['openerSuppressed'], decision
                features = decision['features']
                assert features['bounds'] == {'x': 0, 'y': 0, 'width': 420, 'height': 360}, features
                assert all(features[k] is True for k in FLAGS) and features['alwaysOnTop'] is False, features
                assert legacy == [{'url': BASE + '/popup-blocked', 'frameName': 'blocked', 'userGesture': False}], legacy
                assert not windows
            elif MODE == 'isolation':
                other.set_popup_policy('block')
                other_events = []
                other.on_window_open_event(other_events.append)
                load(other, '/blocked')
                must_wait(lambda: other_events, 'the other view\'s decision')
                assert not events and not legacy, (events, legacy)
            elif MODE in ('created', 'snapshot'):
                first.set_popup_policy('allow')
                assert ev(first, "window.one=window.open('/one','one','left=0,top=0,width=420,height=360,popup=yes');"
                                 "window.two=window.open('/two','two','left=50,top=60,width=340,height=260,popup=yes');"
                                 "!!one && !!two") is True
                must_wait(lambda: len(windows) == 2, 'two windows')
                must_wait(lambda: len([e for e in events if e['phase'] == 'created']) == 2, 'two creation events')
                by_url = {creation['url']: (child, creation) for _, child, creation in windows}
                one, two = by_url[BASE + '/one'][1], by_url[BASE + '/two'][1]
                if MODE == 'created':
                    assert one['features']['bounds'] == {'x': 0, 'y': 0, 'width': 420, 'height': 360}, one
                    assert two['features']['bounds'] == {'x': 50, 'y': 60, 'width': 340, 'height': 260}, two
                    assert one['targetId'] != two['targetId'], (one, two)
                    assert one['hasOpener'] and one['openerOrigin'] == one['topOrigin'] == BASE, one
                    assert one['frameName'] == 'one' and two['frameName'] == 'two', (one, two)
                    told = {e['targetId']: e for e in events if e['phase'] == 'created'}
                    assert told[one['targetId']] == one and told[two['targetId']] == two, told
                else:
                    child = by_url[BASE + '/one'][0]
                    child.load(BASE + '/moved')
                    must_wait(lambda: child.url == BASE + '/moved', 'the window\'s next page')
                    assert child.window_open_info == one, child.window_open_info
            elif MODE in ('plain', 'noopener', 'close-creation'):
                first.set_popup_policy('allow')
                script = {'plain': "window.open('/plain','plain'); true",
                          'noopener': "window.open('/no-opener','_blank','noopener,width=300,height=240'); true",
                          'close-creation': "window.open('/closed-during-creation','close-immediately'); true"}[MODE]
                ev(first, script)
                must_wait(lambda: windows, 'the window')
                creation = windows[-1][2]
                if MODE == 'plain':
                    assert not any(creation['features'][key] for key in FLAGS), creation
                elif MODE == 'noopener':
                    assert creation['hasOpener'] is False and creation['openerOrigin'] is None, creation
                    must_wait(lambda: any(e['url'] == BASE + '/no-opener' and e['phase'] == 'decision'
                                          and e['openerSuppressed'] for e in events), 'the decision, opener suppressed')
                else:
                    must_wait(lambda: windows[-1][0] in gone, 'closed from its creation')
            elif MODE == 'frame':
                load(first, '/with-frame')
                child_origin = BASE.replace('127.0.0.1', 'localhost')
                must_wait(lambda: any(e.get('url') == child_origin + '/from-frame' for e in events), 'the frame\'s decision')
                cross = next(e for e in events if e.get('url') == child_origin + '/from-frame')
                assert cross['sourceOrigin'] == child_origin and cross['openerUrl'] == child_origin + '/frame', cross
                assert cross['topUrl'] == BASE + '/with-frame' and cross['allowed'] is False, cross
            elif MODE == 'remove':
                first.on_window_open_event(None)
                ev(first, "window.open('/removed','removed'); true")
                must_wait(lambda: legacy, 'the older event')
                assert not events, events
            elif MODE == 'close-event':
                first.on_window_open_event(lambda metadata: first.close())
                first.evaluate("window.open('/close','close'); true")
                must_wait(lambda: closed, 'closed from the observer')
            elif MODE == 'adoption':
                adoption(first)
        for _, child, _ in windows:
            child.close()
        check(wait(lambda: len(gone) == len(windows)), 'the windows close')
        other.close()
        if not closed:
            first.close()
        check(wait(lambda: closed), 'the views close')
    finally:
        zebkit.shutdown()
    return finish()


def adoption(parent):
    """The window as a view of its own: its opener, a message to it, its snapshot
    when it loads another page, closing it; the functions removed while they run."""
    parent.set_popup_policy('allow')
    children, told = [], []

    def opened(child, url):
        assert child.window_open_info['url'] == url
        children.append(child)
        parent.on_window_open(None)

    def event(metadata):
        told.append(metadata)
        if metadata['phase'] == 'created':
            parent.on_window_open_event(None)
    parent.on_window_open(opened)
    parent.on_window_open_event(event)
    assert ev(parent, "window.child=window.open('/child','child','left=0,top=0,width=320,height=240'); !!child") is True
    must_wait(lambda: children and any(e['phase'] == 'created' for e in told), 'the window and its event')
    child = children[0]
    must_wait(lambda: child.url == BASE + '/child', 'the window\'s page')
    assert ev(child, 'window.opener !== null') is True
    assert ev(child, "window.opener.fromChild='delivered'; true") is True
    assert ev(parent, 'window.fromChild') == 'delivered'
    snapshot = child.window_open_info
    assert snapshot['targetId'] == child.target_id(), snapshot
    child.load(BASE + '/moved')
    must_wait(lambda: child.url == BASE + '/moved', 'the window\'s next page')
    assert child.window_open_info == snapshot
    gone = []
    child.on_close(lambda: gone.append(True))
    child.close()
    must_wait(lambda: gone, 'the window closes')
    # The opener's process hears of it a moment later, as in Chrome.
    must_wait(lambda: ev(parent, 'window.child.closed') is True, 'the opener sees it closed')
    assert refused(lambda: child.window_open_info), 'a closed window\'s snapshot'


if __name__ == '__main__':
    sys.exit(main())
