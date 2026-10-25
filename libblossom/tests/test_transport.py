#!/usr/bin/env python3
"""Bounded real-HTTP fixture; not live server or release acceptance."""
import base64
import hashlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import subprocess
import sys
import threading
from urllib.parse import urlsplit

stored = {}
requests = []
class Handler(BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass
    def reply(self, code, body=b"", **headers):
        self.send_response(code)
        self.send_header("Content-Length", str(len(body)))
        for key, value in headers.items():
            self.send_header(key.replace("_", "-"), value)
        self.end_headers()
        if self.command != "HEAD":
            try:
                self.wfile.write(body)
            except (BrokenPipeError, ConnectionResetError):
                pass
    def auth(self, action, hash_value=None):
        try:
            scheme, data = self.headers.get("Authorization", "").split(" ", 1)
            event = json.loads(base64.b64decode(data))
            tags = event["tags"]
            assert scheme == "Nostr" and event["kind"] == 24242
            assert ["t", action] in tags and len(event["sig"]) == 128
            if hash_value:
                assert ["x", hash_value] in tags
            return True
        except (AssertionError, KeyError, ValueError, TypeError):
            return False
    def do_GET(self):
        self.dispatch()
    def do_HEAD(self):
        self.dispatch()
    def do_PUT(self):
        self.dispatch()
    def do_DELETE(self):
        self.dispatch()
    def dispatch(self):
        scenario, _, path = self.path.lstrip("/").partition("/")
        requests.append((scenario, self.command, path))
        if not path and self.command == "GET":
            return self.reply(200, json.dumps({"private": scenario == "private", "max_size": 2048 if scenario == "private" else 16*1024*1024}).encode())
        if scenario == "guard":
            if path == "0"*64:
                return self.reply(200, b"a"*2048)
            if path == "1"*64:
                return self.reply(200, b"wrong hash")
            if path == "2"*64:
                self.send_response(200)
                self.send_header("Transfer-Encoding", "chunked")
                self.end_headers()
                try:
                    self.wfile.write(b"800\r\n" + b"a"*2048 + b"\r\n0\r\n\r\n")
                except (BrokenPipeError, ConnectionResetError):
                    pass
                return
            return self.reply(302, b"", Location=f"http://127.0.0.1:{self.server.server_port}/trap/")
        if scenario == "trap":
            raise AssertionError("redirect must not be followed")
        if scenario == "private" and self.command in ("GET", "HEAD") and not self.auth("get", path):
            return self.reply(401, b"auth required", WWW_Authenticate="Nostr")
        if self.command == "HEAD":
            return self.reply(200 if (scenario, path) in stored else 404)
        if self.command == "GET" and path.startswith("list/"):
            assert self.auth("list")
            return self.reply(200, b"[]")
        if self.command == "GET":
            body = stored.get((scenario, path))
            return self.reply(404 if body is None else 200, body or b"")
        if self.command == "DELETE":
            if not self.auth("delete", path):
                return self.reply(401)
            stored.pop((scenario, path), None)
            return self.reply(200, b"{}")
        assert self.command == "PUT" and path in ("upload", "mirror")
        body = self.rfile.read(int(self.headers.get("Content-Length", 0)))
        if path == "mirror":
            source = urlsplit(json.loads(body)["url"]).path.lstrip("/")
            source_scenario, hash_value = source.split("/", 1)
            if not self.auth("upload", hash_value):
                return self.reply(401)
            body = stored[source_scenario, hash_value]
            stored[scenario, hash_value] = body
            return self.reply(200, json.dumps({"sha256": hash_value, "size": len(body)}).encode())
        hash_value = hashlib.sha256(body).hexdigest()
        if not self.auth("upload", hash_value):
            return self.reply(401)
        if scenario == "png" and not body.startswith(b"\x89PNG\r\n\x1a\n"):
            return self.reply(415)
        if scenario == "png":
            assert self.headers["Content-Type"] == "image/png"
        stored[scenario, hash_value] = body
        return self.reply(201, json.dumps({"sha256": hash_value, "size": len(body)}).encode())

def main():
    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    thread = threading.Thread(target=server.serve_forever)
    thread.start()
    base = f"http://127.0.0.1:{server.server_port}"
    try:
        for mode, suffix in (("roundtrip", "raw"), ("roundtrip", "png"), ("private", "private"), ("guards", "guard")):
            subprocess.run([sys.argv[1], mode, base + "/" + suffix], check=True, timeout=90)
        subprocess.run([sys.argv[1], "pool", base + "/raw", base + "/png"], check=True, timeout=90)
        assert not any(scenario == "trap" for scenario, _, _ in requests)
        assert not stored, f"unexpected stored fixture blobs: {list(stored)}"
        print("OK raw/png/private/pool/body-limit/hash/redirect HTTP fixture")
    finally:
        server.shutdown()
        thread.join()
        server.server_close()
if __name__ == "__main__":
    main()
