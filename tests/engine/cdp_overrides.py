"""What Chromium's CDP changes at runtime, checked in zebkit by what the page and a
local server saw, not by the call succeeding (the TODO of docs/ROADMAP.md, section
Agents): user agent with client hints, Accept-Language, locale, time zone,
geolocation, device metrics and screen, permissions, cookies, extra headers, blocked
URLs, offline and throttled networks. A thing that does not work says FAIL here.

    python3 tests/engine/cdp_overrides.py <zebkit-run> [engine switches...]

The locale is Japanese: zebkit's ICU has English and CJK only, by its size budget.
"""

from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from cdp_page import Cdp, check, launch_engine  # noqa: E402
import cdp_page  # noqa: E402

seen = []


class Server(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *args):
        pass

    def do_GET(self):
        seen.append((self.path, {name.lower(): value for name, value in self.headers.items()}))
        body = b"<!doctype html><title>page</title>"
        kind = "text/html"
        extra = []
        if self.path.startswith("/big"):
            body, kind = b"z" * 200000, "application/octet-stream"
        elif self.path.startswith("/set-cookie"):
            extra.append(("Set-Cookie", "from_server=1; Path=/"))
        elif self.path.startswith("/data"):
            kind = "text/plain"
        self.send_response(200)
        self.send_header("Content-Type", kind)
        self.send_header("Cache-Control", "no-store")
        # Asked for by the page's first response, as a site asks.
        self.send_header("Accept-CH", "Sec-CH-UA-Arch, Sec-CH-UA-Model, Sec-CH-UA-Platform-Version")
        for name, value in extra:
            self.send_header(name, value)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        try:
            self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError):
            pass


def main():
    run = sys.argv[1]
    switches = sys.argv[2:]
    server = ThreadingHTTPServer(("127.0.0.1", 0), Server)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    base = f"http://127.0.0.1:{server.server_port}"
    engine = launch_engine([run, *switches])
    url = engine.url
    check(url is not None, "the engine serves CDP", url)
    if not url:
        engine.stop(True)
        return 1
    try:
        cdp = Cdp(url)
        target = cdp.call("Target.createTarget", {"url": "about:blank"})["targetId"]
        session = cdp.call("Target.attachToTarget", {"targetId": target, "flatten": True})["sessionId"]
        cdp.call("Page.enable", session=session)
        cdp.call("Network.enable", session=session)

        def evaluate(expression):
            answer = cdp.call("Runtime.evaluate", {"expression": expression, "returnByValue": True, "awaitPromise": True},
                              session=session)
            if "exceptionDetails" in answer:
                return {"thrown": answer["exceptionDetails"].get("text")}
            return answer["result"].get("value")

        def load(path="/page"):
            cdp.events.clear()
            cdp.call("Page.navigate", {"url": base + path}, session=session)
            cdp.event("Page.loadEventFired", 15)

        def last(path):
            return next((headers for at, headers in reversed(seen) if at.startswith(path)), {})

        def fetched(path):
            """What a fetch came to, and how long it took."""
            return evaluate(f"""(async () => {{
              const start = performance.now();
              try {{ const r = await fetch({json.dumps(path)}); await r.arrayBuffer(); return {{ok: r.ok, ms: performance.now() - start}}; }}
              catch (e) {{ return {{failed: true, ms: performance.now() - start}}; }} }})()""")

        # ---- User agent, client hints, Accept-Language
        load("/first")
        load("/before")
        plain = last("/before")
        check("Sec-CH-UA" not in plain and "OverrideBrand" not in plain.get("sec-ch-ua", ""),
              "no override yet: the real client hints", plain.get("sec-ch-ua"))
        metadata = {"brands": [{"brand": "OverrideBrand", "version": "77"}],
                    "fullVersionList": [{"brand": "OverrideBrand", "version": "77.1.2.3"}],
                    "platform": "OverridePlatform", "platformVersion": "9.8", "architecture": "riscv",
                    "model": "OverrideModel", "mobile": False}
        cdp.call("Emulation.setUserAgentOverride", {"userAgent": "OverrideAgent/1", "acceptLanguage": "ja-JP",
                                                    "platform": "OverrideOS", "userAgentMetadata": metadata}, session=session)
        load("/agent")
        headers = last("/agent")
        check(headers.get("user-agent") == "OverrideAgent/1", "the server saw the User-Agent", headers.get("user-agent"))
        check("OverrideBrand" in headers.get("sec-ch-ua", ""), "the server saw Sec-CH-UA", headers.get("sec-ch-ua"))
        check(headers.get("sec-ch-ua-platform") == '"OverridePlatform"', "the server saw Sec-CH-UA-Platform",
              headers.get("sec-ch-ua-platform"))
        load("/agent-again")
        headers = last("/agent-again")
        check(headers.get("sec-ch-ua-arch") == '"riscv"' and headers.get("sec-ch-ua-model") == '"OverrideModel"',
              "the server saw the high-entropy hints it asked for", headers)
        check(headers.get("accept-language", "").startswith("ja-JP"), "the server saw Accept-Language",
              headers.get("accept-language"))
        page = evaluate("""(async () => ({ua: navigator.userAgent, platform: navigator.platform, language: navigator.language,
            brands: navigator.userAgentData.brands.map(b => b.brand),
            high: await navigator.userAgentData.getHighEntropyValues(['architecture', 'model', 'platformVersion'])}))()""")
        check(page.get("ua") == "OverrideAgent/1" and page.get("platform") == "OverrideOS", "the page saw the user agent", page)
        check(page.get("language") == "ja-JP", "the page saw the language", page.get("language"))
        check("OverrideBrand" in page.get("brands", []) and page.get("high", {}).get("architecture") == "riscv"
              and page.get("high", {}).get("model") == "OverrideModel", "the page saw the client hints", page)
        fetched("/data-agent")
        check(last("/data-agent").get("user-agent") == "OverrideAgent/1", "the server saw it on a fetch")
        cdp.call("Emulation.setUserAgentOverride", {"userAgent": ""}, session=session)
        load("/agent-gone")
        check("OverrideAgent" not in last("/agent-gone").get("user-agent", "OverrideAgent"),
              "an empty override gives the real one back", last("/agent-gone").get("user-agent"))

        # ---- Locale and time zone
        before = evaluate("new Intl.DateTimeFormat().resolvedOptions().locale")
        cdp.call("Emulation.setLocaleOverride", {"locale": "ja-JP"}, session=session)
        locale = evaluate("""({locale: new Intl.DateTimeFormat().resolvedOptions().locale,
            weekday: new Intl.DateTimeFormat(undefined, {weekday: 'long', timeZone: 'UTC'}).format(new Date(0))})""")
        check(locale.get("locale") == "ja-JP" and before != "ja-JP", "locale: Intl in the page", {"before": before, **locale})
        check(locale.get("weekday") == "木曜日", "locale: dates in Japanese", locale.get("weekday"))
        cdp.call("Emulation.setLocaleOverride", {}, session=session)
        cleared = evaluate("new Intl.DateTimeFormat().resolvedOptions().locale")
        check(cleared == before, "locale: cleared", cleared)
        cdp.call("Emulation.setTimezoneOverride", {"timezoneId": "Asia/Tokyo"}, session=session)
        zone = evaluate("({zone: new Intl.DateTimeFormat().resolvedOptions().timeZone, hour: new Date(0).getHours(), offset: new Date(0).getTimezoneOffset()})")
        check(zone == {"zone": "Asia/Tokyo", "hour": 9, "offset": -540}, "time zone: the page's dates", zone)
        cdp.call("Emulation.setTimezoneOverride", {"timezoneId": "America/New_York"}, session=session)
        check(evaluate("new Date(0).getTimezoneOffset()") == 300, "time zone: changed again, in the same document")
        cdp.call("Emulation.setTimezoneOverride", {"timezoneId": ""}, session=session)

        # ---- Permissions and geolocation
        load("/geo")
        position = "new Promise(ok => navigator.geolocation.getCurrentPosition(p => ok({lat: p.coords.latitude, lon: p.coords.longitude}), e => ok({code: e.code}), {timeout: 3000}))"
        state = "navigator.permissions.query({name: 'geolocation'}).then(s => s.state)"
        # As in Chrome the state asked of Permissions reads "prompt"; asking is what is refused.
        check(evaluate(position) == {"code": 1}, "geolocation is refused by default (a privacy default)", evaluate(position))
        cdp.call("Browser.setPermission", {"permission": {"name": "geolocation"}, "setting": "granted", "origin": base})
        check(evaluate(state) == "granted", "Browser.setPermission: the page sees granted", evaluate(state))
        cdp.call("Emulation.setGeolocationOverride", {"latitude": 35.5, "longitude": 139.25, "accuracy": 10}, session=session)
        found = evaluate(position)
        check(found == {"lat": 35.5, "lon": 139.25}, "geolocation: the page reads the position set", found)
        cdp.call("Browser.setPermission", {"permission": {"name": "geolocation"}, "setting": "denied", "origin": base})
        found = evaluate(position)
        check(found == {"code": 1}, "Browser.setPermission denied: the page is refused", found)
        cdp.call("Browser.grantPermissions", {"permissions": ["geolocation"], "origin": base})
        found = evaluate(position)
        check(found == {"lat": 35.5, "lon": 139.25}, "Browser.grantPermissions: the position again", found)
        cdp.call("Emulation.clearGeolocationOverride", session=session)
        cdp.call("Browser.resetPermissions")
        check(evaluate(position) == {"code": 1}, "Browser.resetPermissions: refused again", evaluate(position))

        # ---- Device metrics and screen
        cdp.call("Emulation.setDeviceMetricsOverride", {"width": 411, "height": 733, "deviceScaleFactor": 2.5, "mobile": False,
                                                        "screenWidth": 1234, "screenHeight": 987}, session=session)
        metrics = evaluate("[innerWidth, innerHeight, devicePixelRatio, screen.width, screen.height]")
        check(metrics == [411, 733, 2.5, 1234, 987], "device metrics and screen: what the page reads", metrics)
        cdp.call("Emulation.clearDeviceMetricsOverride", session=session)
        # The page is laid out again after the answer, as it is in Chrome.
        for _ in range(100):
            if evaluate("innerWidth") != 411:
                break
            time.sleep(.05)
        check(evaluate("innerWidth") != 411, "device metrics: cleared", evaluate("innerWidth"))

        # ---- Cookies
        cdp.call("Network.setCookie", {"name": "given", "value": "by-cdp", "url": base + "/"}, session=session)
        load("/cookie-sent")
        check("given=by-cdp" in last("/cookie-sent").get("cookie", ""), "Network.setCookie: the server saw it", last("/cookie-sent").get("cookie"))
        check("given=by-cdp" in evaluate("document.cookie"), "Network.setCookie: the page saw it")
        load("/set-cookie")
        names = [c["name"] for c in cdp.call("Network.getCookies", {"urls": [base + "/"]}, session=session)["cookies"]]
        check("from_server" in names and "given" in names, "Network.getCookies: the server's and the client's", names)
        cdp.call("Network.deleteCookies", {"name": "given", "url": base + "/"}, session=session)
        load("/cookie-gone")
        check("given" not in last("/cookie-gone").get("cookie", ""), "Network.deleteCookies: the server no longer sees it")

        # ---- Extra headers
        cdp.call("Network.setExtraHTTPHeaders", {"headers": {"X-Zebkit-Extra": "yes"}}, session=session)
        load("/extra-page")
        fetched("/extra-fetch")
        check(last("/extra-page").get("x-zebkit-extra") == "yes", "extra headers: the server saw them on a document")
        check(last("/extra-fetch").get("x-zebkit-extra") == "yes", "extra headers: the server saw them on a fetch")
        cdp.call("Network.setExtraHTTPHeaders", {"headers": {}}, session=session)
        fetched("/extra-none")
        check("x-zebkit-extra" not in last("/extra-none"), "extra headers: cleared")

        # ---- Blocked URLs
        cdp.call("Network.setBlockedURLs", {"urls": ["*blocked-old*"]}, session=session)
        answer = fetched("/blocked-old")
        check(answer.get("failed") and not last("/blocked-old"), "setBlockedURLs urls: failed, and nothing reached the server", answer)
        cdp.call("Network.setBlockedURLs", {"urlPatterns": [{"urlPattern": "*://*:*/blocked-new*", "block": True}]}, session=session)
        answer = fetched("/blocked-new")
        check(answer.get("failed") and not last("/blocked-new"), "setBlockedURLs urlPatterns: failed, and nothing reached the server", answer)
        answer = fetched("/data-other")
        check(answer.get("ok") and last("/data-other"), "setBlockedURLs: another URL passes", answer)
        cdp.call("Network.setBlockedURLs", {"urls": []}, session=session)
        answer = fetched("/blocked-new")
        check(answer.get("ok") and last("/blocked-new"), "setBlockedURLs: cleared", answer)

        # ---- Offline and throttled
        conditions = {"offline": True, "latency": 0, "downloadThroughput": -1, "uploadThroughput": -1}
        cdp.call("Network.emulateNetworkConditions", conditions, session=session)
        answer = fetched("/data-offline")
        check(answer.get("failed") and not last("/data-offline"), "offline: failed, and nothing reached the server", answer)
        check(evaluate("navigator.onLine") is False, "offline: navigator.onLine", evaluate("navigator.onLine"))
        cdp.call("Network.emulateNetworkConditions", {**conditions, "offline": False}, session=session)
        check(fetched("/data-online").get("ok") and evaluate("navigator.onLine") is True, "offline: over")
        quick = fetched("/data-quick")["ms"]
        cdp.call("Network.emulateNetworkConditions", {"offline": False, "latency": 700, "downloadThroughput": -1, "uploadThroughput": -1}, session=session)
        answer = fetched("/data-slow")
        check(answer.get("ok") and answer["ms"] >= 600, "throttled: latency", f"{quick:.0f} ms, then {answer['ms']:.0f} ms")
        cdp.call("Network.emulateNetworkConditions", {"offline": False, "latency": 0, "downloadThroughput": 100000, "uploadThroughput": -1}, session=session)
        answer = fetched("/big")
        check(answer.get("ok") and answer["ms"] >= 1200, "throttled: 200 kB at 100 kB/s", f"{answer['ms']:.0f} ms")
        cdp.call("Network.emulateNetworkConditions", {"offline": False, "latency": 0, "downloadThroughput": -1, "uploadThroughput": -1}, session=session)
        # What the deprecation of emulateNetworkConditions in Chromium 155 points to.
        cdp.call("Network.emulateNetworkConditionsByRule", {"matchedNetworkConditions": [
            {"urlPattern": "", "latency": 700, "downloadThroughput": -1, "uploadThroughput": -1}]}, session=session)
        answer = fetched("/data-rule")
        check(answer.get("ok") and answer["ms"] >= 600, "emulateNetworkConditionsByRule: latency", f"{answer['ms']:.0f} ms")
        cdp.call("Network.emulateNetworkConditionsByRule", {"matchedNetworkConditions": []}, session=session)
        cdp.call("Network.overrideNetworkState", {"offline": True, "latency": 0, "downloadThroughput": -1, "uploadThroughput": -1}, session=session)
        check(evaluate("navigator.onLine") is False, "overrideNetworkState: navigator.onLine", evaluate("navigator.onLine"))
        cdp.call("Network.overrideNetworkState", {"offline": False, "latency": 0, "downloadThroughput": -1, "uploadThroughput": -1}, session=session)
    except (RuntimeError, TimeoutError, EOFError, KeyError, TypeError) as error:
        # A method the engine does not serve, or an answer of the wrong shape, is said.
        check(False, "the test went on", repr(error))
    finally:
        engine.stop(bool(cdp_page.failures))
    print(f"{cdp_page.checks - len(cdp_page.failures)} of {cdp_page.checks} checks passed")
    return 1 if cdp_page.failures else 0


if __name__ == "__main__":
    sys.exit(main())
