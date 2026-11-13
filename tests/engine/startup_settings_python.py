"""Startup configuration reaches the real profile, proxy, UA and log backend:
zebkit.init(settings=...) refuses an unknown setting, and the settings it takes
win over the matching switches; logging as ZEBKIT_TEST_LOG_MODE (none, file)
says, never into a default content_shell.log.

    ZEBKIT_TEST_LOG_MODE=none|file python3 tests/engine/startup_settings_python.py

Exit 0 when every check passes.
"""
import http.server
import os
import pathlib
import sys
import tempfile
import threading

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from harness import check, finish, wait, zebkit  # noqa: E402

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

def main():
    with tempfile.TemporaryDirectory(prefix="zebkit-startup-") as directory:
        root = pathlib.Path(directory)
        profile, keys, log = root / "profile", root / "keys", root / "engine.log"
        # Where a default log would go: beside the engine (ZEBKIT_DIR, which the
        # suite sets), beside Python, in the working directory.
        parents = [pathlib.Path(sys.executable).resolve().parent, pathlib.Path.cwd()]
        if os.environ.get("ZEBKIT_DIR"):
            parents.append(pathlib.Path(os.environ["ZEBKIT_DIR"]).resolve())
        defaults = {parent / "content_shell.log" for parent in parents}
        before = {path: (path.stat().st_mtime_ns, path.stat().st_size) if path.exists() else None
                  for path in defaults}
        try:
            zebkit.init(settings={"profileDirectory": str(profile), "unknownSetting": True})
            refused = None
        except zebkit.Error as error:
            refused = str(error)
        check(refused is not None, "an unknown setting is refused", refused)
        check(not profile.exists(), "and nothing of the refused settings is made")
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
        if not check(zebkit.init([sys.argv[0], "--no-proxy-server", "--user-agent=argv-agent",
                                  f"--user-data-dir={root / 'unused-profile'}"], settings=config) == 0,
                     "zebkit.init() with the settings"):
            return
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
        wait(lambda: state["value"] is not None, 15)
        check(state == {"value": [agent, "configured proxy"], "error": None},
              "the page through the configured proxy, with the configured user agent", state)
        check(("http://zebkit-startup.invalid/page", agent) in requests, "the proxy saw the user agent", requests)
        check(profile.is_dir() and keys.is_dir() and any(keys.iterdir()), "the profile and the cookies' key directory")
        check(not (root / "unused-profile").exists(), "the settings win over the matching switches")
        check(zebkit.shutdown(), "zebkit.shutdown()")
        check(bool(list(profile.rglob("Cookies.host"))), "file cookie storage selected")
        if mode == "file":
            check(log.read_text(encoding="utf-8", errors="replace").startswith("host-log-sentinel"),
                  "logging=file appends, never truncates the host's file")
        else:
            check(not log.exists(), "logging=none writes no log")
        after = {path: (path.stat().st_mtime_ns, path.stat().st_size) if path.exists() else None
                 for path in defaults}
        check(before == after, "no default content_shell.log made or changed",
              None if before == after else sorted(str(path) for path in defaults if before[path] != after[path]))


server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Proxy)
thread = threading.Thread(target=server.serve_forever, daemon=True)
thread.start()
try:
    main()
finally:
    server.shutdown()
    thread.join()
    server.server_close()
sys.exit(finish())
