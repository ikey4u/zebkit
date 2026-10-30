"""Startup configuration reaches the real profile, proxy, UA and log backend."""
import http.server
import os
import pathlib
import sys
import tempfile
import threading
import time

import zebkit

mode = os.environ.get("ZEBKIT_TEST_LOG_MODE", "none")
requests = []

class Proxy(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        requests.append((self.path, self.headers.get("User-Agent")))
        body = b"<!doctype html><title>configured proxy</title><body>via local proxy</body>"
        self.send_response(200)
        self.send_header("Content-Type", "text/html")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Set-Cookie", "startup=kept; Path=/; Max-Age=3600; HttpOnly")
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *args):
        pass

server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Proxy)
thread = threading.Thread(target=server.serve_forever, daemon=True)
thread.start()
try:
    with tempfile.TemporaryDirectory(prefix="zebkit-startup-") as directory:
        root = pathlib.Path(directory)
        profile, keys, log = root / "profile", root / "keys", root / "engine.log"
        defaults = {parent / "content_shell.log" for parent in (
            pathlib.Path(zebkit._lib._name).resolve().parent,
            pathlib.Path(sys.executable).resolve().parent, pathlib.Path.cwd())}
        before = {path: (path.stat().st_mtime_ns, path.stat().st_size) if path.exists() else None
                  for path in defaults}
        try:
            zebkit.init(settings={"profileDirectory": str(profile), "unknownSetting": True})
            raise AssertionError("unknown setting was accepted")
        except zebkit.Error:
            pass
        assert not profile.exists()
        logging = {"destination": mode, "level": 0}
        if mode == "file":
            log.write_text("host-log-sentinel\n")
            logging["path"] = str(log)
        agent = "Zebkit configured host"
        config = {
            "profileDirectory": str(profile), "proxy": f"http://127.0.0.1:{server.server_port}",
            "userAgent": agent, "headless": True, "sandbox": False, "gpu": "disabled",
            "logging": logging, "cookies": {"storage": "file", "keyDirectory": str(keys)},
            "applicationIntegration": False,
        }
        assert zebkit.init([sys.argv[0], "--no-proxy-server", "--user-agent=argv-agent",
                            f"--user-data-dir={root / 'unused-profile'}"], settings=config) == 0
        state = {"value": None, "error": None}
        view = zebkit.View()

        def evaluated(value, error):
            state.update(value=value, error=error)

        def loaded(event, url):
            if url != "http://zebkit-startup.invalid/page":
                return
            if event == zebkit.LOAD_FINISHED:
                view.evaluate("[navigator.userAgent, document.title]", evaluated)
            elif event == zebkit.LOAD_FAILED:
                state.update(value=False, error="proxy navigation failed")

        view.on_load(loaded)
        view.load("http://zebkit-startup.invalid/page")
        deadline = time.monotonic() + 15
        while state["value"] is None and time.monotonic() < deadline:
            assert zebkit.pump()
            time.sleep(0.001)
        assert state == {"value": [agent, "configured proxy"], "error": None}, state
        assert ("http://zebkit-startup.invalid/page", agent) in requests, requests
        assert profile.is_dir() and keys.is_dir() and any(keys.iterdir())
        assert not (root / "unused-profile").exists(), "API settings must override matching switches"
        assert zebkit.shutdown()
        assert list(profile.rglob("Cookies.host")), "file cookie storage must be selected"
        if mode == "file":
            assert log.read_text().startswith("host-log-sentinel\n"), "logging must append, never truncate the host's file"
        else:
            assert not log.exists()
        after = {path: (path.stat().st_mtime_ns, path.stat().st_size) if path.exists() else None
                 for path in defaults}
        assert before == after, "startup must not create or modify a default content_shell.log"
        print(f"ok startup settings: strict validation, real proxy/UA/profile/cookies, logging={mode}", flush=True)
finally:
    server.shutdown()
    thread.join()
    server.server_close()
