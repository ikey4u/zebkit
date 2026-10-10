#!/usr/bin/env python3
"""Checks an endpoint started without discovery: private.py CDP_PORT LOG

The browser must not tell anyone where its sockets are. A client that was
given the address it printed (the last "DevTools listening on" line of LOG)
can connect; nobody else can.
"""

import pathlib
import sys
import urllib.error
import urllib.request

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from cdpclient import Connection, ProtocolError  # noqa: E402

port = int(sys.argv[1])
lines = [line for line in pathlib.Path(sys.argv[2]).read_text(errors="replace").splitlines() if line.startswith("DevTools listening on ")]
url = lines[-1].split()[-1]
failures = []

for path in ("/json/version", "/json/list", "/json"):
    try:
        with urllib.request.urlopen(f"http://127.0.0.1:{port}{path}", timeout=10) as response:
            failures.append(f"{path} was served ({response.status})")
    except urllib.error.HTTPError as error:
        if error.code != 404:
            failures.append(f"{path} answered {error.code}, not 404")

try:
    Connection(f"ws://127.0.0.1:{port}/devtools/browser").call("Browser.getVersion", timeout=3)
    failures.append("a socket without the browser id was accepted")
except (ProtocolError, OSError, TimeoutError):
    pass

try:
    browser = Connection(url)
    targets = browser.call("Target.getTargets")["targetInfos"]
    if not targets:
        failures.append("the printed address gave no targets")
    else:
        session = browser.call("Target.attachToTarget", {"targetId": targets[0]["targetId"], "flatten": True})["sessionId"]
        value = browser.call("Runtime.evaluate", {"expression": "6 * 7", "returnByValue": True}, session=session)["result"].get("value")
        if value != 42:
            failures.append(f"evaluation through the printed address gave {value!r}")
except (ProtocolError, OSError, TimeoutError) as error:
    failures.append(f"the printed address did not work: {error!r}")

for failure in failures:
    print(f"FAIL  {failure}")
print("ok    discovery is off and the printed address works" if not failures else f"{len(failures)} failed")
sys.exit(1 if failures else 0)
