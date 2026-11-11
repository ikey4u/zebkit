"""A service worker's clients.openWindow() from a notification click: opened
under the default popup policy (it carries no user gesture of its own: the
click's activation is the worker's, checked in the renderer), refused when
the page's view blocks every popup.

    python3 tests/engine/sw_open_window.py [engine switches...]

Exit 0 when every check passes.
"""

import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

from harness import Server, check, ev, finish, wait, zebkit  # noqa: E402


PAGE = (b"<title>worker</title><script>window.results = [];"
        b"navigator.serviceWorker.onmessage = e => results.push(e.data);"
        b"window.ready = navigator.serviceWorker.register('/sw.js').then(() => navigator.serviceWorker.ready)"
        b".then(() => 'ready');"
        b"window.note = (title) => navigator.serviceWorker.ready.then(r => r.showNotification(title)).then(() => 1);"
        b"</script>")
WORKER = (b"self.addEventListener('install', () => self.skipWaiting());"
          b"self.addEventListener('activate', e => e.waitUntil(clients.claim()));"
          b"async function report(text) {"
          b"  for (const c of await clients.matchAll({includeUncontrolled: true, type: 'window'})) c.postMessage(text); }"
          b"self.addEventListener('notificationclick', e => e.waitUntil(clients.openWindow('/opened')"
          b"  .then(c => report(c ? 'opened' : 'null'), error => report('refused: ' + error.name))));")


def click(view, events, title):
    """Shows a notification from the worker and clicks it: what openWindow() gave."""
    before = len(ev(view, "results"))
    shown = len(events)
    ev(view, "note(%r)" % title)
    wait(lambda: len(events) > shown, 5)
    show = [e for e in events[shown:] if e.get("event") == "show"]
    if not show:
        return None
    zebkit.notification_clicked(show[0]["id"])
    wait(lambda: len(ev(view, "results")) > before, 10)
    results = ev(view, "results")
    return results[before] if len(results) > before else None


def main():
    server = Server({"/": PAGE, "/sw.js": (WORKER, "text/javascript"), "/opened": b"<title>opened</title>"})
    url = server.base + "/"
    if zebkit.init(["sw-open-window"] + sys.argv[1:]) != 0:
        print("FAIL zebkit.init()")
        return 1
    try:
        events = []
        zebkit.on_notification(events.append)
        view = zebkit.View(url)
        wait(lambda: ev(view, "location.href + document.readyState") == url + "complete")
        check(ev(view, "ready") == "ready", "the service worker is active")
        view.set_permission("notifications", "granted", "*")
        check(view.popup_policy == "block-without-gesture", "the default popup policy")
        opened = click(view, events, "first")
        check(opened == "opened", "openWindow() from a notification click opens under the default policy", opened)
        check(view.set_popup_policy("block"), "the view blocks every popup")
        refused = click(view, events, "second")
        check(refused is not None and refused != "opened", "and then openWindow() is refused", refused)
        zebkit.on_notification(None)
        view.close()
    except Exception as error:  # noqa: BLE001
        check(False, "the test ran to the end", repr(error))
    finally:
        zebkit.shutdown()
        server.close()
    return finish()


if __name__ == "__main__":
    sys.exit(main())
