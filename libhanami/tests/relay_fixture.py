#!/usr/bin/env python3
"""Loopback-only Nostr wire fixture; no external services or timing sleeps."""
import base64
import hashlib
import json
import socketserver
import struct
import subprocess
import sys
import threading


class Relay(socketserver.StreamRequestHandler):
    def frame(self, message):
        payload = json.dumps(message).encode()
        header = bytes([0x81, len(payload)]) if len(payload) < 126 else b"\x81\x7e" + struct.pack("!H", len(payload))
        self.wfile.write(header + payload)
        self.wfile.flush()

    def handle(self):
        request = self.rfile.readline().decode().split()
        if len(request) < 2:
            return
        mode = request[1].strip("/")
        headers = {}
        while line := self.rfile.readline().strip():
            key, value = line.decode().split(":", 1)
            headers[key.lower()] = value.strip()
        accept = base64.b64encode(hashlib.sha1((headers["sec-websocket-key"] + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode()).digest()).decode()
        self.wfile.write(("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: " + accept + "\r\n\r\n").encode())
        self.wfile.flush()
        try:
            while header := self.rfile.read(2):
                if len(header) != 2 or header[0] & 15 == 8:
                    return
                length = header[1] & 127
                if length == 126:
                    length = struct.unpack("!H", self.rfile.read(2))[0]
                elif length == 127:
                    length = struct.unpack("!Q", self.rfile.read(8))[0]
                mask = self.rfile.read(4) if header[1] & 128 else None
                payload = self.rfile.read(length)
                if mask:
                    payload = bytes(c ^ mask[i % 4] for i, c in enumerate(payload))
                if header[0] & 15 != 1:
                    continue
                message = json.loads(payload)
                if message[0] == "EVENT":
                    event = message[1]
                    accepted = mode not in ("reject", "publish-auth", "refdb")
                    if accepted:
                        with self.server.lock:
                            self.server.events[mode] = event
                    reason = "auth-required: fixture requires credentials" if mode == "publish-auth" else "blocked: fixture refusal"
                    self.frame(["OK", event["id"], accepted, "" if accepted else reason])
                elif message[0] == "REQ":
                    subid, filters = message[1], message[2]
                    if mode == "refdb":
                        assert filters["kinds"] == [30618]
                        assert filters["authors"] and all(author in ("bbbb", "b" * 64) for author in filters["authors"])
                        assert filters["#d"] == ["test-repo"]
                        with self.server.lock:
                            self.server.refdb_requests += 1
                    else:
                        assert filters["kinds"][0] in (30617, 30618)
                        assert filters["authors"] and filters["#d"] == ["repo"]
                    assert filters["limit"] == 1
                    if mode == "closed":
                        self.frame(["CLOSED", subid, "blocked: fixture refusal"])
                    elif mode == "auth":
                        self.frame(["AUTH", "fixture-challenge"])
                        self.frame(["CLOSED", subid, "auth-required: fixture requires credentials"])
                    else:
                        with self.server.lock:
                            event = self.server.events.get(mode)
                        if event and mode == "invalid":
                            event = dict(event, sig="0" * 128)
                        if event and event["kind"] in filters["kinds"]:
                            self.frame(["EVENT", subid, event])
                            # Replay the same id: snapshot reduction must be idempotent.
                            self.frame(["EVENT", subid, event])
                        if mode != "no-eose":
                            self.frame(["EOSE", subid])
        except (BrokenPipeError, ConnectionResetError):
            return


class Server(socketserver.ThreadingTCPServer):
    daemon_threads = True
    allow_reuse_address = True


with Server(("127.0.0.1", 0), Relay) as server:
    server.events = {}
    server.refdb_requests = 0
    server.lock = threading.Lock()
    worker = threading.Thread(target=server.serve_forever, daemon=True)
    worker.start()
    try:
        mode = sys.argv[2] if len(sys.argv) > 2 else ""
        url = f"ws://127.0.0.1:{server.server_address[1]}"
        if mode:
            url += f"/{mode}"
        result = subprocess.run([sys.argv[1], url], timeout=25 if mode == "refdb" else 40)
    finally:
        server.shutdown()
        worker.join()
    if mode == "refdb" and server.refdb_requests == 0:
        sys.exit("refdb test did not query the loopback relay")
    sys.exit(result.returncode)
