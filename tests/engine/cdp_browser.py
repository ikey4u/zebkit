"""What a page sees of the browser it runs in, and of a CDP client that
drives it, from tools/zebkit-run serving CDP (pages/browser.html):

- nothing of the client: with Runtime enabled, describing the errors a
  page logs runs none of its code (no Error.prepareStackTrace, no more
  reads of a getter), and the client still has every error with its stack;
- window.chrome timing and legacy hosted-app queries without extensions;
- the real engine version in the user agent, client hints in JavaScript
  and HTTP, and languages consistent with Accept-Language;
- popups blocked without a click, opened for the application's script;
- a screen of a real size under --headless; navigator.webdriver as Chrome
  sets it (true here, since the port is 0).

    python3 tests/engine/cdp_browser.py <zebkit-run> <base URL> [engine switches...]
"""

import os
import re
import socket
import sys
import time
import urllib.parse

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import Cdp, check, failures, finish, launch_engine  # noqa: E402


def upgrade_status(url, origin=None):
    """The status of a WebSocket upgrade to @url with an Origin header of @origin."""
    parsed = urllib.parse.urlparse(url)
    with socket.create_connection((parsed.hostname, parsed.port), timeout=10) as connection:
        headers = [f"GET {parsed.path} HTTP/1.1", f"Host: {parsed.netloc}", "Upgrade: websocket",
                   "Connection: Upgrade", "Sec-WebSocket-Version: 13", "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ=="]
        if origin:
            headers.append(f"Origin: {origin}")
        connection.sendall(("\r\n".join(headers) + "\r\n\r\n").encode())
        return int(connection.recv(64).split()[1])


def check_origins(run, switches, url):
    """Chrome's --remote-allow-origins: without it no web page's origin connects."""
    check(upgrade_status(url, "https://evil.example") == 403, "CDP refuses an Origin, as Chrome without --remote-allow-origins")
    check(upgrade_status(url) == 101, "CDP takes a client without an Origin")
    other = [s + "-origins" if "user-data-dir" in s else s for s in switches]
    engine = launch_engine([run, *other, "--remote-allow-origins=https://good.example"])
    try:
        allowed = engine.url
        check(allowed and upgrade_status(allowed, "https://good.example") == 101, "--remote-allow-origins lets that origin in")
        check(allowed and upgrade_status(allowed, "https://evil.example") == 403, "and no other")
    finally:
        engine.stop(not allowed)


def main():
    run, base = sys.argv[1:3]
    switches = sys.argv[3:]
    engine = launch_engine([run, *switches])
    url = engine.url
    check(url is not None, "the engine serves CDP", url)
    if not url:
        engine.stop(True)
        return finish()
    try:
        check_origins(run, switches, url)
        cdp = Cdp(url)
        target = cdp.call("Target.createTarget", {"url": "about:blank"})["targetId"]
        session = cdp.call("Target.attachToTarget", {"targetId": target, "flatten": True})["sessionId"]
        cdp.call("Page.enable", session=session)
        product = cdp.call("Browser.getVersion")["product"]

        def results():
            cdp.call("Page.navigate", {"url": f"{base}/browser.html"}, session=session)
            value = None
            end = time.time() + 15
            while time.time() < end and not (value or {}).get("done"):
                time.sleep(0.01)
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
        major = re.search(r"/(\d+)\.", product).group(1)
        agent = watched.get("userAgent", "")
        check(f"Chrome/{major}." in agent and "HeadlessChrome/" not in agent,
              "the user agent names the real Chromium version", agent)
        hints = watched.get("userAgentData") or {}
        brands = hints.get("brands", [])
        check(any(brand.get("brand") == "Chromium" and brand.get("version") == major for brand in brands),
              "client hints name Chromium of the same version", brands)
        platform = {"darwin": "macOS", "linux": "Linux", "win32": "Windows"}.get(sys.platform)
        check(platform is not None and hints.get("platform") == platform,
              "client hints name the actual desktop platform", hints.get("platform"))
        check(not watched.get("headersError"), "the server reports the actual request headers",
              watched.get("headersError"))
        headers = {name.lower(): value for name, value in watched.get("headers", {}).items()}
        check(f'"Chromium";v="{major}"' in headers.get("sec-ch-ua", ""),
              "requests send Chromium client hints", headers.get("sec-ch-ua"))
        check(headers.get("sec-ch-ua-platform") == f'"{hints.get("platform")}"',
              "HTTP and JavaScript agree on the platform", headers.get("sec-ch-ua-platform"))
        languages = watched.get("languages") or []
        check(bool(languages) and headers.get("accept-language", "").split(",")[0] == languages[0],
              "Accept-Language agrees with navigator.languages", headers.get("accept-language"))
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
        engine.stop(bool(failures))
    return finish()


if __name__ == "__main__":
    sys.exit(main())
