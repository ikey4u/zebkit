#!/usr/bin/env python3
"""Serve a directory for the test scripts: serve.py DIR PORT [CERT_PEM].

With a certificate (key and cert in one PEM file) the server speaks HTTPS.
Requests are logged to stdout, which is how the scripts read what a page reports.
"""

import functools
import http.server
import pathlib
import ssl
import sys

directory, port = str(pathlib.Path(sys.argv[1]).resolve()), int(sys.argv[2])
handler = functools.partial(http.server.SimpleHTTPRequestHandler, directory=directory)
server = http.server.ThreadingHTTPServer(("127.0.0.1", port), handler)
if len(sys.argv) > 3:
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.load_cert_chain(sys.argv[3])
    server.socket = context.wrap_socket(server.socket, server_side=True)
sys.stderr = sys.stdout
server.serve_forever()
