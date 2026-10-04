#!/usr/bin/env python3
"""Local-only BUD-02 media fixture for two-device Groundhog acceptance runs.

This is not a production Blossom server: it checks upload authorization metadata,
not the Schnorr signature. Bind only to 127.0.0.1 and use throwaway identities.
"""
import argparse
import base64
import hashlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
import pwd
import re
import ssl
from pathlib import Path
import time


class Handler(BaseHTTPRequestHandler):
    def do_PUT(self):
        if self.path != "/upload":
            return self.send_error(404)
        size = int(self.headers.get("Content-Length", "0"))
        if not 0 < size <= 50 * 1024 * 1024:
            return self.send_error(413)
        data = self.rfile.read(size)
        digest = hashlib.sha256(data).hexdigest()
        try:
            auth = self.headers["Authorization"]
            if not auth.startswith("Nostr "):
                raise ValueError("missing Nostr scheme")
            event = json.loads(base64.b64decode(auth[6:], validate=True))
            tags = {tag[0]: tag[1] for tag in event["tags"] if len(tag) > 1}
            if event["kind"] != 24242 or tags.get("t") != "upload" or tags.get("x") != digest:
                raise ValueError("wrong upload authorization")
            if int(tags.get("expiration", "0")) <= time.time():
                raise ValueError("expired upload authorization")
        except (KeyError, ValueError, TypeError, json.JSONDecodeError) as exc:
            return self.send_error(401, str(exc))
        target = self.server.blobs / digest
        if target.is_symlink() or (target.exists() and not target.is_file()):
            return self.send_error(409, "unsafe blob path")
        if target.exists():
            if hashlib.sha256(target.read_bytes()).hexdigest() != digest:
                return self.send_error(409, "existing blob has wrong digest")
        else:
            try:
                flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW
                with os.fdopen(os.open(target, flags, 0o600), "wb") as output:
                    output.write(data)
            except FileExistsError:
                return self.send_error(409, "blob path raced with another writer")
        body = json.dumps({"url": f"https://127.0.0.1:{self.server.server_port}/{digest}",
                           "sha256": digest, "size": size,
                           "type": "application/octet-stream", "uploaded": int(time.time())}).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        digest = self.path.lstrip("/").split(".", 1)[0]
        if len(digest) != 64 or any(c not in "0123456789abcdef" for c in digest):
            return self.send_error(404)
        path = self.server.blobs / digest
        if path.is_symlink() or not path.is_file():
            return self.send_error(404)
        data = path.read_bytes()
        self.send_response(200)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, default=4850)
    parser.add_argument("--run-id", required=True,
                        help="UTC timestamp run component (YYYYMMDDTHHMMSS)")
    args = parser.parse_args()
    if not re.fullmatch(r"[0-9]{8}T[0-9]{6}", args.run_id):
        parser.error("--run-id must be a single UTC timestamp component")
    if "W27_RUN_DIR" in os.environ:
        parser.error("W27_RUN_DIR override is unsupported")
    home = Path(pwd.getpwuid(os.getuid()).pw_dir)
    if not home.is_absolute() or ".." in home.parts or "." in home.parts or home.is_symlink():
        parser.error("unsafe account home")
    home = home.resolve(strict=True)
    run_root = home / "gh-run"
    run_dir = run_root / args.run_id
    blobs = run_dir / "blobs"
    for path in (run_root, run_dir, blobs):
        if path.is_symlink():
            parser.error(f"refusing symlinked run path: {path}")
        path.mkdir(mode=0o700, exist_ok=True)
        if path.is_symlink() or not path.is_dir():
            parser.error(f"unsafe run path: {path}")
        path.chmod(0o700)
    if run_dir.resolve(strict=True).parent != run_root.resolve(strict=True):
        parser.error("run directory escaped the fixed root")
    cert = run_dir / "blossom.crt"
    key = run_dir / "blossom.key"
    if any(path.is_symlink() or not path.is_file() for path in (cert, key)):
        parser.error("put regular blossom.crt and blossom.key files in the run directory")
    server = ThreadingHTTPServer(("127.0.0.1", args.port), Handler)
    server.blobs = blobs
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.load_cert_chain(cert, key)
    server.socket = context.wrap_socket(server.socket, server_side=True)
    print(f"local Blossom fixture listening on 127.0.0.1:{args.port}", flush=True)
    server.serve_forever()


if __name__ == "__main__":
    main()
