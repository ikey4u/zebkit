"""What a page sees of the browser it runs in, and of a CDP client that
drives it, from tools/zebkit-run serving CDP (pages/browser.html):

- nothing of the client: with Runtime enabled, describing the errors a
  page logs runs none of its code (no Error.prepareStackTrace, no more
  reads of a getter), and the client still has every error with its stack;
- window.chrome timing and legacy hosted-app queries without extensions;
- popups blocked without a click, opened for the application's script;
- a screen of a real size under --headless; navigator.webdriver as Chrome
  sets it (true here, since the port is 0).

    python3 tests/engine/cdp_browser.py <zebkit-run> <base URL> [engine switches...]
"""

import os
import re
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from cdp_page import Cdp, check  # noqa: E402
import cdp_page  # noqa: E402


def main():
    run, base = sys.argv[1:3]
    switches = sys.argv[3:]
    engine = subprocess.Popen([run, *switches, "--remote-debugging-port=0", "about:blank"],
                              stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
    url = None
    end = time.time() + 30
    while time.time() < end and url is None:
        found = re.search(r"ws://\S+", engine.stderr.readline())
        if found:
            url = found.group(0)
    check(url is not None, "the engine serves CDP", url)
    if not url:
        engine.kill()
        return 1
    try:
        cdp = Cdp(url)
        target = cdp.call("Target.createTarget", {"url": "about:blank"})["targetId"]
        session = cdp.call("Target.attachToTarget", {"targetId": target, "flatten": True})["sessionId"]
        cdp.call("Page.enable", session=session)

        def results():
            cdp.call("Page.navigate", {"url": f"{base}/browser.html"}, session=session)
            value = None
            end = time.time() + 15
            while time.time() < end and not (value or {}).get("done"):
                time.sleep(0.2)
                value = cdp.call("Runtime.evaluate", {"expression": "window.results", "returnByValue": True},
                                 session=session).get("result", {}).get("value")
            return value or {}

        # The page alone, then with a client listening as Puppeteer does.
        alone = results()
        cdp.call("Runtime.enable", session=session)
        cdp.events.clear()
        watched = results()
        check(watched.get("prepared") == 0 and alone.get("prepared") == 0,
              "a client describing errors runs no Error.prepareStackTrace", watched.get("prepared"))
        check(watched.get("nameReads") == alone.get("nameReads"),
              "a client describing errors reads no getter more",
              f"{alone.get('nameReads')} alone, {watched.get('nameReads')} watched")
        logged = []
        while (event := cdp.event("Runtime.consoleAPICalled", 0.5)) is not None:
            logged += [arg.get("description", "") for arg in event["params"]["args"]]
        check(any(d.startswith("Error: logged\n    at thrower") for d in logged),
              "the client has the error with its stack", logged[:1])
        check(any(d.startswith("Named: named\n    at") for d in logged),
              "and an error whose name is a getter", logged[1:2])

        check(watched.get("chrome") is True, "window.chrome with loadTimes() and csi()", watched.get("chrome"))
        for label, sample in (("alone", alone), ("watched", watched)):
            app = sample.get("app") or {}
            check(app.get("installed") is False and app.get("queriedInstalled") is False
                  and app.get("details") is None and app.get("running") == "cannot_run",
                  f"legacy app queries report no hosted app ({label})", app)
            check(app.get("installState") == "not_installed" and app.get("callbackWasAsync") is True,
                  f"installState returns the actual state asynchronously ({label})", app.get("installState"))
            check(app.get("assignmentIgnored") is True, f"isInstalled cannot be overwritten ({label})")
            check(app.get("installStates") == {"NOT_INSTALLED": "not_installed", "INSTALLED": "installed", "DISABLED": "disabled"}
                  and app.get("runningStates") == {"RUNNING": "running", "CANNOT_RUN": "cannot_run", "READY_TO_RUN": "ready_to_run"},
                  f"hosted app state enums follow Chromium's API ({label})")
            check(app.get("badArguments") == [True] * 6, f"legacy app methods reject invalid arguments ({label})")
            check(app.get("detachedCallback") is False, f"removed documents receive no app callback ({label})")
            check(app.get("callbackExceptionReported") is True,
                  f"app callback exceptions reach the page and leave it running ({label})")
        check(watched.get("popupWithoutClick") is False, "a popup without a click is blocked",
              watched.get("popupWithoutClick"))
        opened = cdp.call("Runtime.evaluate", {"expression": "window.open('about:blank', '_blank') !== null",
                                               "returnByValue": True, "userGesture": True},
                          session=session).get("result", {}).get("value")
        check(opened is True, "one the application opens, opens", opened)
        width, height = watched.get("screen", [0, 0])
        check(width >= 800 and height >= 600, "the screen has a real size", f"{width}x{height}")
        # As in Chrome: an ephemeral port is how ChromeDriver starts a browser,
        # and the WebDriver standard has a browser under automation say so.
        check(watched.get("webdriver") is True, "navigator.webdriver as Chrome sets it with port 0",
              watched.get("webdriver"))
    finally:
        engine.terminate()
        try:
            engine.wait(5)
        except subprocess.TimeoutExpired:
            engine.kill()
    print(f"{cdp_page.checks - len(cdp_page.failures)} of {cdp_page.checks} checks passed")
    return 1 if cdp_page.failures else 0


if __name__ == "__main__":
    sys.exit(main())
