#!/usr/bin/env python3
"""
fake_porthome_blossom.py — Content-addressed Blossom BUD-01/BUD-02 fake
sufficient to exercise nh_porthome_blossom against libhanami.

Extends the existing tests/integration/fake_blossom.py with:
  - Store PUT bodies under sha256(body)  (real BUD-01 addressing)
  - GET / HEAD by that sha256
  - Fault-injection knobs read from env: drop / corrupt / truncate
  - Optional 200 mirror endpoint (BUD-04) — TODO for Phase 2

This is a test fake; it does not implement true BUD-02 verification.
Every PUT is accepted; the wrapper's job is to reject responses whose
content-sha256 does not match its request, which is exactly what this
fake will provoke when FAKE_BLOSSOM_MODE=corrupt.

Env:
  FAKE_BLOSSOM_PORT       (default 8081)
  FAKE_BLOSSOM_DIR        (default /tmp/fake_ph_blossom_<port>)
  FAKE_BLOSSOM_MODE       "ok" (default) | "corrupt" (flip a byte on GET)
                          | "drop" (500 on every GET)
                          | "truncate" (return half the bytes)
  FAKE_BLOSSOM_HEAD_MODE  "ok" | "always_missing"
"""
import hashlib
import http.server
import os
import socketserver
import sys

PORT = int(os.environ.get("FAKE_BLOSSOM_PORT", "8081"))
DATA_DIR = os.environ.get("FAKE_BLOSSOM_DIR", f"/tmp/fake_ph_blossom_{PORT}")
MODE = os.environ.get("FAKE_BLOSSOM_MODE", "ok")
HEAD_MODE = os.environ.get("FAKE_BLOSSOM_HEAD_MODE", "ok")

os.makedirs(DATA_DIR, exist_ok=True)


def _url_tail(path: str) -> str:
    return path.strip("/").split("/")[-1] or ""


class Handler(http.server.BaseHTTPRequestHandler):
    def _blob_path(self, cid: str) -> str:
        return os.path.join(DATA_DIR, cid)

    def do_PUT(self):
        length = int(self.headers.get("Content-Length", "0"))
        # libhanami's upload lands at /upload; we compute sha256 ourselves.
        data = b""
        remaining = length
        while remaining > 0:
            chunk = self.rfile.read(min(65536, remaining))
            if not chunk:
                break
            data += chunk
            remaining -= len(chunk)
        sha = hashlib.sha256(data).hexdigest()
        target = self._blob_path(sha)
        with open(target, "wb") as f:
            f.write(data)
            f.flush()
            os.fsync(f.fileno())
        body = ('{"sha256":"%s","size":%d,"type":"application/octet-stream",'
                '"uploaded":0}' % (sha, len(data))).encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_HEAD(self):
        cid = _url_tail(self.path)
        if HEAD_MODE == "always_missing":
            self.send_response(404)
            self.end_headers()
            return
        if os.path.exists(self._blob_path(cid)):
            self.send_response(200)
            self.send_header("Content-Type", "application/octet-stream")
            with open(self._blob_path(cid), "rb") as f:
                data = f.read()
            self.send_header("Content-Length", str(len(data)))
        else:
            self.send_response(404)
        self.end_headers()

    def do_GET(self):
        cid = _url_tail(self.path)
        p = self._blob_path(cid)
        if MODE == "drop":
            self.send_response(500)
            self.end_headers()
            return
        if not os.path.exists(p):
            self.send_response(404)
            self.end_headers()
            return
        with open(p, "rb") as f:
            data = f.read()
        if MODE == "corrupt" and data:
            # Flip a byte so content-sha256 disagrees with the requested hash.
            data = bytearray(data)
            data[len(data) // 2] ^= 0x55
            data = bytes(data)
        if MODE == "truncate":
            data = data[: max(0, len(data) // 2)]
        self.send_response(200)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_DELETE(self):
        cid = _url_tail(self.path)
        p = self._blob_path(cid)
        if os.path.exists(p):
            os.unlink(p)
        self.send_response(200)
        self.end_headers()

    def log_message(self, fmt, *args):
        return  # quiet


if __name__ == "__main__":
    with socketserver.TCPServer(("127.0.0.1", PORT), Handler) as httpd:
        httpd.allow_reuse_address = True
        print("READY", flush=True)
        print(f"fake_porthome_blossom on http://127.0.0.1:{PORT}, dir={DATA_DIR}, mode={MODE}",
              file=sys.stderr, flush=True)
        httpd.serve_forever()
