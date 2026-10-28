#!/usr/bin/env python3
"""Loopback relay + public Blossom + GRASP-style Git Smart HTTP fixture.

Exercises hanami_clone discovery/cold clone with no external services and no
timing sleeps. The GRASP server only speaks Git Smart HTTP; it counts every
request so tests can prove clone URLs are not used as Blossom APIs.
"""
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


class Store:
    def __init__(self):
        self.lock = threading.Lock()
        self.blobs = {}
        self.events = {}
        self.grasp_requests = []
        self.blob_gets = 0
        self.gets = {}
        self.reject_manifest = False  # PUT of a manifest segment -> 500
        self.relay_reject = False     # relay EVENT -> OK false


store = Store()


class Blossom(http.server.BaseHTTPRequestHandler):
    """Public BUD-01/BUD-02 server: any signed kind-24242 upload is accepted."""

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
        if self.path != "/upload":
            self.reply(404)
            return
        auth = self.headers.get("Authorization", "")
        blob = self.rfile.read(int(self.headers.get("Content-Length", "0")))
        if not auth.startswith("Nostr "):
            self.reply(401)
            return
        event = json.loads(base64.b64decode(auth.split()[1]))
        if event.get("kind") != 24242:
            self.reply(401)
            return
        sha = hashlib.sha256(blob).hexdigest()
        with store.lock:
            if store.reject_manifest and blob.startswith(b'{"prev"'):
                self.reply(500)
                return
            store.blobs[sha] = blob
        self.reply(200, json.dumps({"sha256": sha, "size": len(blob),
                                    "type": "application/octet-stream",
                                    "uploaded": 1}).encode(), "application/json")

    def do_DELETE(self):
        with store.lock:
            store.blobs.pop(self.path.strip("/"), None)
        self.reply(204)

    def do_HEAD(self):
        self.do_GET()

    def do_GET(self):
        if self.path == "/control/grasp-hits":
            with store.lock:
                body = str(len(store.grasp_requests)).encode()
            self.reply(200, body, "text/plain")
            return
        if self.path == "/control/blob-gets":
            with store.lock:
                body = str(store.blob_gets).encode()
            self.reply(200, body, "text/plain")
            return
        if self.path.startswith("/control/set/"):
            flag, value = self.path.split("/")[3:5]
            with store.lock:
                setattr(store, flag.replace("-", "_"), value == "on")
            self.reply(200)
            return
        if self.path.startswith("/control/blob/"):
            with store.lock:
                blob = store.blobs.get(self.path.rsplit("/", 1)[-1])
            self.reply(200 if blob is not None else 404, blob or b"")
            return
        if self.path.startswith("/control/gets/"):
            with store.lock:
                body = str(store.gets.get(self.path.rsplit("/", 1)[-1], 0)).encode()
            self.reply(200, body, "text/plain")
            return
        key = self.path.strip("/")
        with store.lock:
            blob = store.blobs.get(key)
            if self.command == "GET" and len(key) == 64:
                store.blob_gets += 1
                store.gets[key] = store.gets.get(key, 0) + 1
        self.reply(200 if blob is not None else 404, blob or b"")


class Grasp(http.server.BaseHTTPRequestHandler):
    """Git Smart HTTP only (what a NIP-34 `clone` URL points at)."""

    def log_message(self, *args):
        pass

    def handle_any(self):
        with store.lock:
            store.grasp_requests.append((self.command, self.path))
        length = int(self.headers.get("Content-Length", "0") or "0")
        if length:
            self.rfile.read(length)
        if self.command == "GET" and self.path.endswith("/info/refs?service=git-upload-pack"):
            body = b"001e# service=git-upload-pack\n0000" + b"0000"
            self.send_response(200)
            self.send_header("Content-Type", "application/x-git-upload-pack-advertisement")
        else:
            body = b"not found"
            self.send_response(404)
            self.send_header("Content-Type", "text/plain")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(body)

    do_GET = do_HEAD = do_PUT = do_POST = do_DELETE = handle_any


def matches(event, f):
    if "kinds" in f and event["kind"] not in f["kinds"]:
        return False
    if "authors" in f and event["pubkey"] not in f["authors"]:
        return False
    for key, values in f.items():
        if key.startswith("#"):
            name = key[1:]
            if not any(t[0] == name and len(t) > 1 and t[1] in values for t in event["tags"]):
                return False
    return True


def replace_key(event):
    kind = event["kind"]
    if 10000 <= kind < 20000:
        return (event["pubkey"], kind, "")
    if 30000 <= kind < 40000:
        d = next((t[1] for t in event["tags"] if t[0] == "d" and len(t) > 1), "")
        return (event["pubkey"], kind, d)
    return (event["id"],)


class Relay(socketserver.StreamRequestHandler):
    def frame(self, message):
        data = json.dumps(message).encode()
        if len(data) < 126:
            header = bytes([0x81, len(data)])
        elif len(data) < 65536:
            header = b"\x81\x7e" + struct.pack("!H", len(data))
        else:
            header = b"\x81\x7f" + struct.pack("!Q", len(data))
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
                if len(h) != 2 or h[0] & 15 == 8:
                    return
                n = h[1] & 127
                if n == 126:
                    n = struct.unpack("!H", self.rfile.read(2))[0]
                elif n == 127:
                    n = struct.unpack("!Q", self.rfile.read(8))[0]
                mask = self.rfile.read(4) if h[1] & 128 else None
                payload = self.rfile.read(n)
                if mask:
                    payload = bytes(c ^ mask[i % 4] for i, c in enumerate(payload))
                if h[0] & 15 != 1:
                    continue
                message = json.loads(payload)
                if message[0] == "EVENT":
                    event = message[1]
                    key = replace_key(event)
                    with store.lock:
                        accepted = not store.relay_reject
                        old = store.events.get(key)
                        if accepted and (old is None or (event["created_at"], old["id"]) >= (old["created_at"], event["id"])):
                            store.events[key] = event
                    self.frame(["OK", event["id"], accepted, "" if accepted else "blocked: fixture refusal"])
                elif message[0] == "REQ":
                    subid, filters = message[1], message[2:]
                    with store.lock:
                        events = list(store.events.values())
                    for f in filters:
                        found = sorted((e for e in events if matches(e, f)),
                                       key=lambda e: e["created_at"], reverse=True)
                        for event in found[:f.get("limit", len(found))]:
                            self.frame(["EVENT", subid, event])
                            self.frame(["EVENT", subid, event])  # idempotent replay
                    self.frame(["EOSE", subid])
                elif message[0] == "CLOSE":
                    pass
        except (ConnectionResetError, BrokenPipeError):
            pass


class Server(socketserver.ThreadingTCPServer):
    daemon_threads = True
    allow_reuse_address = True


def main():
    program, modes = sys.argv[1], sys.argv[2:] or ["discovery"]
    with http.server.ThreadingHTTPServer(("127.0.0.1", 0), Blossom) as blossom, \
         http.server.ThreadingHTTPServer(("127.0.0.1", 0), Grasp) as grasp, \
         Server(("127.0.0.1", 0), Relay) as relay, \
         tempfile.TemporaryDirectory(prefix="hanami-clone-") as tmp:
        servers = (blossom, grasp, relay)
        threads = [threading.Thread(target=s.serve_forever, daemon=True) for s in servers]
        for t in threads:
            t.start()
        blossom_url = f"http://127.0.0.1:{blossom.server_port}"
        grasp_url = f"http://127.0.0.1:{grasp.server_port}/npub1hanamifixture/repo.git"
        relay_url = f"ws://127.0.0.1:{relay.server_address[1]}"
        try:
            for mode in modes:
                # Every mode is a fresh process with its own empty TMPDIR: no
                # in-process caches or prior local state carry across.
                mode_tmp = os.path.join(tmp, "tmp-" + mode)
                os.mkdir(mode_tmp)
                env = dict(os.environ, TMPDIR=mode_tmp)
                env.pop("HANAMI_ENDPOINT", None)
                env.pop("HANAMI_RELAYS", None)
                subprocess.run([program, blossom_url, relay_url, grasp_url, tmp, mode],
                               check=True, timeout=120, env=env)
            with store.lock:
                bad = [r for r in store.grasp_requests
                       if r[0] == "GET" and len(r[1].rsplit("/", 1)[-1]) == 64]
            assert not bad, f"clone URL used as a Blossom blob endpoint: {bad}"
            print(f"clone fixture: {' '.join(modes)} ok; grasp requests={len(store.grasp_requests)}")
        finally:
            for s in servers:
                s.shutdown()
            for t in threads:
                t.join()


if __name__ == "__main__":
    main()
