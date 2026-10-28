#!/usr/bin/env python3
"""HTTP refusal followed by a real reconnect upgrade; no timing-based replies."""
import base64
import hashlib
import json
import socketserver
import struct
import subprocess
import sys
import threading


class Relay(socketserver.StreamRequestHandler):
    def handle(self):
        request = self.rfile.readline().split()
        if len(request) < 2:
            return
        headers = {}
        while line := self.rfile.readline().strip():
            key, value = line.decode().split(":", 1)
            headers[key.lower()] = value.strip()
        with self.server.lock:
            attempt = self.server.attempts.get(request[1], 0) + 1
            self.server.attempts[request[1]] = attempt
        try:
            if request[1] == b"/unavailable" or attempt == 2:
                # Keep the HTTP transport open: HTTP establishment must not
                # become WS success even before the peer closes it.
                self.wfile.write(b"HTTP/1.1 503 Service Unavailable\r\nContent-Length: 100\r\nConnection: keep-alive\r\n\r\n")
                self.wfile.flush()
                assert self.rfile.read(1) == b"", "client wrote a REQ before a WebSocket upgrade"
                return
            key = headers["sec-websocket-key"] + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
            accept = base64.b64encode(hashlib.sha1(key.encode()).digest()).decode()
            self.wfile.write(("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: " + accept + "\r\n\r\n").encode())
            self.wfile.flush()
            while header := self.rfile.read(2):
                if len(header) != 2 or header[0] & 15 == 8:
                    return
                length = header[1] & 127
                if length == 126:
                    length = struct.unpack("!H", self.rfile.read(2))[0]
                elif length == 127:
                    length = struct.unpack("!Q", self.rfile.read(8))[0]
                mask = self.rfile.read(4)
                payload = self.rfile.read(length)
                message = json.loads(bytes(c ^ mask[i % 4] for i, c in enumerate(payload)))
                if message[0] == "REQ":
                    response = json.dumps(["EOSE", message[1]]).encode()
                    self.wfile.write(bytes([0x81, len(response)]) + response)
                    self.wfile.flush()
                    if attempt == 1:
                        return
        except (BrokenPipeError, ConnectionResetError):
            pass


class Server(socketserver.ThreadingTCPServer):
    daemon_threads = True
    allow_reuse_address = True

    def handle_error(self, request, client_address):
        self.errors.append(sys.exc_info()[1])


with Server(("127.0.0.1", 0), Relay) as server:
    server.lock = threading.Lock()
    server.attempts = {}
    server.errors = []
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        result = subprocess.run([sys.argv[1], f"ws://127.0.0.1:{server.server_address[1]}"], timeout=30)
    finally:
        server.shutdown()
        thread.join()
    assert not server.errors, server.errors
    if result.returncode == 0:
        assert server.attempts.get(b"/reconnect", 0) == 3, server.attempts
    sys.exit(result.returncode)
