"""A small Chrome DevTools Protocol client on the standard library only.

Enough WebSocket to talk to a local endpoint: text frames, masking,
fragmentation, ping and close. Used by tests/cdp/protocol.py; the Puppeteer
and Playwright tests bring their own clients.
"""

import base64
import json
import os
import socket
import struct
import time
import urllib.request

from schema import Schema

SCHEMA = Schema()
# Where the endpoint or a test departs from the standard definition:
# problem -> how many times it was seen. Reported by the test that ran.
NONSTANDARD = {}


def note(problems):
    for problem in problems:
        NONSTANDARD[problem] = NONSTANDARD.get(problem, 0) + 1


class ProtocolError(Exception):
    pass


def http_json(port, path, method="GET"):
    request = urllib.request.Request(f"http://127.0.0.1:{port}{path}", method=method)
    with urllib.request.urlopen(request, timeout=10) as response:
        return json.loads(response.read().decode())


class Connection:
    def __init__(self, url, origin=None):
        assert url.startswith("ws://")
        host_port, _, path = url[5:].partition("/")
        host, _, port = host_port.partition(":")
        self.socket = socket.create_connection((host, int(port or 80)), timeout=30)
        key = base64.b64encode(os.urandom(16)).decode()
        headers = [
            f"GET /{path} HTTP/1.1",
            f"Host: {host_port}",
            "Upgrade: websocket",
            "Connection: Upgrade",
            f"Sec-WebSocket-Key: {key}",
            "Sec-WebSocket-Version: 13",
        ]
        if origin:
            headers.append(f"Origin: {origin}")
        self.socket.sendall(("\r\n".join(headers) + "\r\n\r\n").encode())
        self.buffer = b""
        while b"\r\n\r\n" not in self.buffer:
            chunk = self.socket.recv(4096)
            if not chunk:
                raise ProtocolError("connection closed during the handshake")
            self.buffer += chunk
        head, _, self.buffer = self.buffer.partition(b"\r\n\r\n")
        status = head.split(b"\r\n")[0].decode()
        if " 101 " not in status:
            raise ProtocolError(f"handshake refused: {status}")
        self.next_id = 1
        self.methods = {}
        self.events = []
        self.closed = False

    def close(self):
        try:
            self._send_frame(0x8, b"")
            self.socket.close()
        except OSError:
            pass
        self.closed = True

    # -- frames ---------------------------------------------------------

    def _send_frame(self, opcode, payload):
        mask = os.urandom(4)
        header = bytes([0x80 | opcode])
        length = len(payload)
        if length < 126:
            header += bytes([0x80 | length])
        elif length < 65536:
            header += bytes([0x80 | 126]) + struct.pack(">H", length)
        else:
            header += bytes([0x80 | 127]) + struct.pack(">Q", length)
        masked = bytes(byte ^ mask[index % 4] for index, byte in enumerate(payload))
        self.socket.sendall(header + mask + masked)

    def _read(self, count, deadline):
        while len(self.buffer) < count:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError()
            self.socket.settimeout(remaining)
            try:
                chunk = self.socket.recv(65536)
            except socket.timeout:
                raise TimeoutError()
            if not chunk:
                self.closed = True
                raise ProtocolError("connection closed")
            self.buffer += chunk
        data, self.buffer = self.buffer[:count], self.buffer[count:]
        return data

    def _read_message(self, deadline):
        message = b""
        while True:
            first, second = self._read(2, deadline)
            opcode = first & 0x0F
            length = second & 0x7F
            if length == 126:
                (length,) = struct.unpack(">H", self._read(2, deadline))
            elif length == 127:
                (length,) = struct.unpack(">Q", self._read(8, deadline))
            payload = self._read(length, deadline)
            if opcode == 0x9:
                self._send_frame(0xA, payload)
                continue
            if opcode == 0x8:
                self.closed = True
                raise ProtocolError("connection closed by the endpoint")
            if opcode in (0x0, 0x1):
                message += payload
                if first & 0x80:
                    message = json.loads(message.decode())
                    self._compare(message)
                    return message

    def _compare(self, message):
        if "id" in message:
            method = self.methods.pop(message["id"], None)
            if method and "result" in message:
                note(SCHEMA.result(method, message["result"]))
        elif "method" in message:
            note(SCHEMA.event(message["method"], message.get("params", {})))

    # -- protocol -------------------------------------------------------

    def post(self, method, params=None, session=None, standard=True):
        """Send a command; standard=False for one that is meant to be wrong."""
        message = {"id": self.next_id, "method": method, "params": params or {}}
        if standard:
            note(SCHEMA.command(method, message["params"]))
            self.methods[message["id"]] = method
        if session:
            message["sessionId"] = session
        self.next_id += 1
        self._send_frame(0x1, json.dumps(message).encode())
        return message["id"]

    def reply(self, command_id, timeout=30):
        # It may have arrived while something else was being waited for.
        for index, message in enumerate(self.events):
            if message.get("id") == command_id:
                del self.events[index]
                return message
        deadline = time.monotonic() + timeout
        while True:
            message = self._read_message(deadline)
            if message.get("id") == command_id:
                return message
            self.events.append(message)

    def call(self, method, params=None, session=None, timeout=30):
        """Send a command and return its result; raise if it failed."""
        message = self.reply(self.post(method, params, session), timeout)
        if "error" in message:
            raise ProtocolError(f"{method}: {message['error'].get('message')}")
        return message.get("result", {})

    def error(self, method, params=None, session=None, standard=True):
        """Send a command that is expected to fail; return the message."""
        message = self.reply(self.post(method, params, session, standard))
        if "error" not in message:
            raise ProtocolError(f"{method} was expected to fail but returned {message.get('result')}")
        return message["error"].get("message", "")

    def failure(self, method, params=None, session=None, standard=True):
        """Send a command that is expected to fail; return (code, message)."""
        message = self.reply(self.post(method, params, session, standard))
        if "error" not in message:
            raise ProtocolError(f"{method} was expected to fail but returned {message.get('result')}")
        return message["error"].get("code"), message["error"].get("message", "")

    def send_raw(self, text):
        """Send text that may not be a command at all; return the answer."""
        self._send_frame(0x1, text.encode())
        deadline = time.monotonic() + 10
        while True:
            message = self._read_message(deadline)
            if "error" in message:
                return message
            self.events.append(message)

    def wait_event(self, method, predicate=None, session=None, timeout=30):
        """Return the first event that matches, received already or not."""
        deadline = time.monotonic() + timeout

        def matches(message):
            if message.get("method") != method:
                return False
            if session is not None and message.get("sessionId") != session:
                return False
            return predicate is None or predicate(message.get("params", {}))

        while True:
            for index, message in enumerate(self.events):
                if matches(message):
                    del self.events[index]
                    return message.get("params", {})
            try:
                self.events.append(self._read_message(deadline))
            except TimeoutError:
                raise TimeoutError(f"no {method} event within {timeout} s")

    def drain(self, seconds=0.2):
        """Collect whatever arrives in the next moment."""
        deadline = time.monotonic() + seconds
        try:
            while True:
                self.events.append(self._read_message(deadline))
        except TimeoutError:
            pass

    def take_events(self, method):
        taken = [message.get("params", {}) for message in self.events if message.get("method") == method]
        self.events = [message for message in self.events if message.get("method") != method]
        return taken
