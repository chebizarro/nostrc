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
        (self.server.blobs / digest).write_bytes(data)
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
        if not path.is_file():
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
    parser.add_argument("--blobs", type=Path, required=True)
    parser.add_argument("--cert", type=Path, required=True)
    parser.add_argument("--key", type=Path, required=True)
    args = parser.parse_args()
    args.blobs.mkdir(parents=True, exist_ok=True)
    server = ThreadingHTTPServer(("127.0.0.1", args.port), Handler)
    server.blobs = args.blobs
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.load_cert_chain(args.cert, args.key)
    server.socket = context.wrap_socket(server.socket, server_side=True)
    print(f"local Blossom fixture listening on 127.0.0.1:{args.port}", flush=True)
    server.serve_forever()


if __name__ == "__main__":
    main()
