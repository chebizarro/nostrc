#!/usr/bin/env python3
import http.server
import socketserver
import sys
import os

PORT = int(os.environ.get("FAKE_BLOSSOM_PORT", "8081"))
DATA_DIR = os.environ.get("FAKE_BLOSSOM_DIR", "/tmp/fake_blossom")
os.makedirs(DATA_DIR, exist_ok=True)

class Handler(http.server.BaseHTTPRequestHandler):
    def do_PUT(self):
        # Accept any path; write body to file named after requested path tail
        length = int(self.headers.get('Content-Length', '0'))
        cid = self.path.strip('/').split('/')[-1] or 'unknown'
        path = os.path.join(DATA_DIR, cid)
        with open(path, 'wb') as f:
            remaining = length
            while remaining > 0:
                chunk = self.rfile.read(min(65536, remaining))
                if not chunk:
                    break
                f.write(chunk)
                remaining -= len(chunk)
            f.flush()
            os.fsync(f.fileno())
        self.send_response(201)
        self.send_header('Content-Type', 'text/plain')
        self.end_headers()
        self.wfile.write(cid.encode('utf-8'))

    def do_HEAD(self):
        cid = self.path.strip('/').split('/')[-1] or 'unknown'
        path = os.path.join(DATA_DIR, cid)
        if os.path.exists(path):
            self.send_response(200)
        else:
            self.send_response(404)
        self.end_headers()

    def do_GET(self):
        cid = self.path.strip('/').split('/')[-1] or 'unknown'
        path = os.path.join(DATA_DIR, cid)
        if os.path.exists(path):
            self.send_response(200)
            self.send_header('Content-Type', 'application/octet-stream')
            self.end_headers()
            with open(path, 'rb') as f:
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

if __name__ == '__main__':
    with socketserver.TCPServer(("127.0.0.1", PORT), Handler) as httpd:
        httpd.serve_forever()
