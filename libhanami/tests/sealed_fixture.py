#!/usr/bin/env python3
"""Loopback Blossom + Nostr wire fixture for sealed Git; no external services."""
import base64
import hashlib
import http.server
import json
import os
import socketserver
import struct
import subprocess
import sys
import tempfile
import threading
import zlib


class Store:
    def __init__(self):
        self.lock = threading.Lock()
        self.blobs = {}
        self.events = {}
        self.published = []
        self.reject = False
        self.corrupt = False


store = Store()


class Blossom(http.server.BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def reply(self, code, body=b"", content_type="application/octet-stream"):
        self.send_response(code)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Content-Type", content_type)
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(body)

    def do_PUT(self):
        assert self.path == "/upload"
        assert self.headers.get("Authorization", "").startswith("Nostr ")
        blob = self.rfile.read(int(self.headers["Content-Length"]))
        sha = hashlib.sha256(blob).hexdigest()
        auth = json.loads(base64.b64decode(self.headers["Authorization"].split()[1]))
        # Reject ephemeral capability-probe identities, as a private service
        # does. Probe payloads are not Git/vault objects and must not be stored.
        if auth["pubkey"] not in (
            "79be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798",
            "c6047f9441ed7d6d3045406e95c07cd85c778e4b8cef3ca7abac09b95c709ee5",
        ):
            self.reply(401)
            return
        with store.lock:
            store.blobs[sha] = blob
        self.reply(200, json.dumps({"sha256": sha, "size": len(blob), "type": "application/octet-stream", "uploaded": 1}).encode(), "application/json")

    def do_DELETE(self):
        with store.lock:
            store.blobs.pop(self.path.strip("/"), None)
        self.reply(204)

    def do_HEAD(self):
        self.do_GET()

    def do_GET(self):
        if self.path == "/":
            self.reply(200, b'{"private":true}', "application/json")
            return
        if self.path.startswith("/control/"):
            with store.lock:
                if self.path.endswith("reject"): store.reject = True
                elif self.path.endswith("accept"): store.reject = False
                elif self.path.endswith("corrupt"): store.corrupt = True
                elif self.path.endswith("restore"): store.corrupt = False
            self.reply(200)
            return
        if self.path == "/head":
            with store.lock:
                heads = [e for e in store.events.values() if e["kind"] == 30078]
                latest = max(heads, key=lambda e: e["created_at"])
            self.reply(200, json.dumps(latest).encode(), "application/json")
            return
        with store.lock:
            blob = store.blobs.get(self.path.strip("/"))
            if blob and store.corrupt:
                blob = blob[:-1] + bytes([blob[-1] ^ 1])
        self.reply(200 if blob is not None else 404, blob or b"")


class Relay(socketserver.StreamRequestHandler):
    def frame(self, message):
        data = json.dumps(message).encode()
        header = bytes([0x81, len(data)]) if len(data) < 126 else b"\x81\x7e" + struct.pack("!H", len(data))
        self.wfile.write(header + data)
        self.wfile.flush()

    def handle(self):
        self.rfile.readline()
        headers = {}
        while line := self.rfile.readline().strip():
            key, value = line.decode().split(":", 1)
            headers[key.lower()] = value.strip()
        accept = base64.b64encode(hashlib.sha1((headers["sec-websocket-key"] + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode()).digest()).decode()
        self.wfile.write(("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: " + accept + "\r\n\r\n").encode())
        self.wfile.flush()
        try:
            while h := self.rfile.read(2):
                if len(h) != 2 or h[0] & 15 == 8: return
                n = h[1] & 127
                if n == 126: n = struct.unpack("!H", self.rfile.read(2))[0]
                elif n == 127: n = struct.unpack("!Q", self.rfile.read(8))[0]
                mask = self.rfile.read(4) if h[1] & 128 else None
                payload = self.rfile.read(n)
                if mask: payload = bytes(c ^ mask[i % 4] for i, c in enumerate(payload))
                if h[0] & 15 != 1: continue
                message = json.loads(payload)
                if message[0] == "EVENT":
                    event = message[1]
                    d = next(t[1] for t in event["tags"] if t[0] == "d")
                    with store.lock:
                        accepted = not store.reject
                        if accepted:
                            key = (event["pubkey"], event["kind"], d)
                            old = store.events.get(key)
                            if old is None or event["created_at"] > old["created_at"]:
                                store.events[key] = event
                            store.published.append(event)
                    self.frame(["OK", event["id"], accepted, "" if accepted else "blocked: fixture refusal"])
                elif message[0] == "REQ":
                    subid, f = message[1:3]
                    assert f["kinds"] == [30078], f
                    assert len(f["authors"]) == 1 and f["#d"][0].startswith("porthome/v2/")
                    assert f["limit"] == 1
                    with store.lock:
                        events = [e for (author, kind, d), e in store.events.items()
                                  if author in f["authors"] and kind in f["kinds"] and d in f["#d"]]
                    for event in events:
                        self.frame(["EVENT", subid, event])
                        self.frame(["EVENT", subid, event])  # idempotent replay
                    self.frame(["EOSE", subid])
        except (ConnectionResetError, BrokenPipeError):
            pass


class Server(socketserver.ThreadingTCPServer):
    daemon_threads = True
    allow_reuse_address = True


def main():
    with http.server.ThreadingHTTPServer(("127.0.0.1", 0), Blossom) as blossom, Server(("127.0.0.1", 0), Relay) as relay, tempfile.TemporaryDirectory(prefix="hanami-sealed-") as tmp:
        threads = [threading.Thread(target=s.serve_forever, daemon=True) for s in (blossom, relay)]
        for t in threads: t.start()
        args = [sys.argv[1], f"http://127.0.0.1:{blossom.server_port}", f"ws://127.0.0.1:{relay.server_address[1]}", tmp]
        env = dict(os.environ, TMPDIR=tmp)
        try:
            subprocess.run(args + ["private"], check=True, timeout=90, env=env)
            with store.lock:
                assert store.blobs and store.published
                assert all(e["kind"] == 30078 for e in store.published), "private operation published public state"
                for sha, blob in store.blobs.items():
                    assert hashlib.sha256(blob).hexdigest() == sha and blob[0] == 2
                    assert b"only the vault can read this" not in blob
                    try: zlib.decompress(blob)
                    except zlib.error: pass
                    else: raise AssertionError("Blossom contains a plaintext loose Git object")
                private_addresses = set(store.blobs)
            subprocess.run(args + ["lift"], check=True, timeout=90, env=env)
            with store.lock:
                assert any(e["kind"] == 30618 for e in store.published)
                assert any(b"only the vault can read this" in b for b in store.blobs.values())
                assert private_addresses <= set(store.blobs), "lift deleted private history"
            print("sealed wire fixture: private opaque store/no 30618; explicit lift mirrors plaintext + 30618")
        finally:
            for s in (blossom, relay): s.shutdown()
            for t in threads: t.join()


if __name__ == "__main__":
    main()
