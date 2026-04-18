#!/usr/bin/env python3
"""
fake_blob_server.py — Minimal Blossom CAS HTTP server for integration tests.

PUT  /<path>  — store body, respond with SHA-256 CID
GET  /<cid>   — retrieve blob by CID
HEAD /<cid>   — check existence

Environment:
  FAKE_BLOSSOM_PORT  — TCP port (default: 8081)
  FAKE_BLOSSOM_DIR   — storage directory (default: /tmp/fake_blossom)

Usage:
  FAKE_BLOSSOM_PORT=8081 python3 fake_blob_server.py
"""
import hashlib
import http.server
import os
import socketserver
import sys

PORT = int(os.environ.get("FAKE_BLOSSOM_PORT", "8081"))
DATA_DIR = os.environ.get("FAKE_BLOSSOM_DIR", "/tmp/fake_blossom")
os.makedirs(DATA_DIR, exist_ok=True)


class Handler(http.server.BaseHTTPRequestHandler):
    def do_PUT(self):
        length = int(self.headers.get("Content-Length", "0"))
        body = b""
        remaining = length
        while remaining > 0:
            chunk = self.rfile.read(min(65536, remaining))
            if not chunk:
                break
            body += chunk
            remaining -= len(chunk)
        cid = hashlib.sha256(body).hexdigest()
        path = os.path.join(DATA_DIR, cid)
        with open(path, "wb") as f:
            f.write(body)
        self.send_response(201)
        self.send_header("Content-Type", "application/json")
        self.end_headers()
        self.wfile.write(('{"sha256":"' + cid + '"}').encode())

    def do_HEAD(self):
        cid = self.path.strip("/").split("/")[-1] or "unknown"
        path = os.path.join(DATA_DIR, cid)
        if os.path.exists(path):
            self.send_response(200)
            self.send_header("Content-Length", str(os.path.getsize(path)))
        else:
            self.send_response(404)
        self.end_headers()

    def do_GET(self):
        cid = self.path.strip("/").split("/")[-1] or "unknown"
        path = os.path.join(DATA_DIR, cid)
        if os.path.exists(path):
            self.send_response(200)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Length", str(os.path.getsize(path)))
            self.end_headers()
            with open(path, "rb") as f:
                while True:
                    b = f.read(65536)
                    if not b:
                        break
                    self.wfile.write(b)
        else:
            self.send_response(404)
            self.end_headers()

    def log_message(self, fmt, *args):
        return  # quiet


if __name__ == "__main__":
    with socketserver.TCPServer(("127.0.0.1", PORT), Handler) as httpd:
        print(
            f"fake_blob: http://127.0.0.1:{PORT}",
            file=sys.stderr,
            flush=True,
        )
        # Signal readiness on stdout
        print("READY", flush=True)
        httpd.serve_forever()
