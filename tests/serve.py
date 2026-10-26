#!/usr/bin/env python3
"""Serve a directory for the test scripts: serve.py DIR PORT [CERT_PEM].

With a certificate (key and cert in one PEM file) the server speaks HTTPS.
Requests are logged to stdout, which is how the scripts read what a page reports.
Files are served in the range a Range header asks for, as media players
seek in them.
"""

import functools
import http.server
import os
import pathlib
import re
import ssl
import sys


class Handler(http.server.SimpleHTTPRequestHandler):
    def do_GET(self):
        if self.path == "/__zebkit_oversize_body__":
            self.send_response(200)
            self.send_header("Content-Length", str(257 * 1024 * 1024))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            block = b"x" * (1024 * 1024)
            try:
                for _ in range(257):
                    self.wfile.write(block)
            except (BrokenPipeError, ConnectionResetError):
                pass
            return
        if self.path == "/__zebkit_large_body__":
            body = b"x" * (6 * 1024 * 1024)
            self.send_response(200)
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(body)
            return
        super().do_GET()

    def send_head(self):
        match = re.fullmatch(r"bytes=(\d*)-(\d*)", self.headers.get("Range", ""))
        path = self.translate_path(self.path)
        if not match or not os.path.isfile(path):
            return super().send_head()
        size = os.path.getsize(path)
        start, end = match.groups()
        if start:
            start, end = int(start), min(int(end) if end else size - 1, size - 1)
        else:
            start, end = max(0, size - int(end or 0)), size - 1
        if start > end:
            self.send_error(416)
            return None
        file = open(path, "rb")
        file.seek(start)
        self.send_response(206)
        self.send_header("Content-Type", self.guess_type(path))
        self.send_header("Content-Range", f"bytes {start}-{end}/{size}")
        self.send_header("Content-Length", str(end - start + 1))
        self.range_left = end - start + 1
        self.end_headers()
        return file

    def copyfile(self, source, outputfile):
        left = getattr(self, "range_left", None)
        if left is None:
            return super().copyfile(source, outputfile)
        while left > 0:
            chunk = source.read(min(65536, left))
            if not chunk:
                break
            outputfile.write(chunk)
            left -= len(chunk)

    def end_headers(self):
        if not hasattr(self, "range_left"):
            self.send_header("Accept-Ranges", "bytes")
        super().end_headers()


directory, port = str(pathlib.Path(sys.argv[1]).resolve()), int(sys.argv[2])
handler = functools.partial(Handler, directory=directory)
server = http.server.ThreadingHTTPServer(("127.0.0.1", port), handler)
if len(sys.argv) > 3:
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.load_cert_chain(sys.argv[3])
    server.socket = context.wrap_socket(server.socket, server_side=True)
sys.stderr = sys.stdout
server.serve_forever()
