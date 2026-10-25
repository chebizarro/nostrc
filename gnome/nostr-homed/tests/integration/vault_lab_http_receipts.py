#!/usr/bin/env python3
"""Loopback-only transparent HTTP fixture for actual VSP lab payload receipts.
No response/auth/signature is synthesized: all requests go to the real service.
Used by porthome_jmx0_two_npub.py to prove the old sealed blob is not re-uploaded.
"""
import argparse
import hashlib
import http.client
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import threading

p = argparse.ArgumentParser()
p.add_argument("--upstream", required=True, help="Disposable lab HTTP host:port, e.g. nginx:80")
p.add_argument("--port", type=int, default=18080)
p.add_argument("--receipts", required=True)
a = p.parse_args()
host, port = a.upstream.rsplit(":", 1)
lock = threading.Lock()

class Proxy(BaseHTTPRequestHandler):
    def log_message(self, *_args):
        pass

    def request(self):
        length = int(self.headers.get("Content-Length", 0))
        if length < 0 or length > 64 * 1024 * 1024 or self.headers.get("Transfer-Encoding"):
            self.send_error(413)
            return
        body = self.rfile.read(length)
        connection = http.client.HTTPConnection(host, int(port), timeout=30)
        try:
            headers = {k: v for k, v in self.headers.items() if k.lower() not in ("host", "connection")}
            connection.request(self.command, self.path, body, headers)
            response = connection.getresponse()
            data = response.read()
            with lock, open(a.receipts, "a", encoding="utf8") as receipt:
                receipt.write(json.dumps(dict(method=self.command, path=self.path,
                    sha=hashlib.sha256(body).hexdigest() if body else None,
                    status=response.status, size=len(body))) + "\n")
                receipt.flush()
            self.send_response(response.status)
            for key, value in response.getheaders():
                if key.lower() not in ("connection", "transfer-encoding", "content-length"):
                    self.send_header(key, value)
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)
        finally:
            connection.close()

    do_GET = request
    do_HEAD = request
    do_PUT = request
    do_POST = request

ThreadingHTTPServer(("127.0.0.1", a.port), Proxy).serve_forever()
