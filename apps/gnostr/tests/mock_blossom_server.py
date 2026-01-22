#!/usr/bin/env python3
"""
Mock Blossom Server for gnostr E2E Testing

Implements BUD-01 (Server Specification) and BUD-02 (Blob Retrieval)
for testing Blossom file upload functionality.

Features:
- PUT /upload - Upload files with NIP-98 auth
- GET /<sha256> - Retrieve uploaded files
- GET /list/<pubkey> - List files by pubkey
- DELETE /<sha256> - Delete files with auth
- HEAD /<sha256> - Check file existence

Test modes:
- MOCK_BLOSSOM_MODE=normal - Standard operation
- MOCK_BLOSSOM_MODE=auth_fail - Return 401 on all auth requests
- MOCK_BLOSSOM_MODE=server_error - Return 500 on uploads
- MOCK_BLOSSOM_MODE=size_limit - Reject files > 1KB
- MOCK_BLOSSOM_MODE=slow - Add 2 second delay to uploads
"""

import http.server
import socketserver
import sys
import os
import json
import hashlib
import base64
import time
import threading
from urllib.parse import urlparse, parse_qs
from datetime import datetime

PORT = int(os.environ.get("MOCK_BLOSSOM_PORT", "8765"))
DATA_DIR = os.environ.get("MOCK_BLOSSOM_DIR", "/tmp/mock_blossom")
MODE = os.environ.get("MOCK_BLOSSOM_MODE", "normal")
LOG_REQUESTS = os.environ.get("MOCK_BLOSSOM_LOG", "1") == "1"

# Ensure data directory exists
os.makedirs(DATA_DIR, exist_ok=True)

# Store metadata separately from file content
METADATA_DIR = os.path.join(DATA_DIR, ".meta")
os.makedirs(METADATA_DIR, exist_ok=True)

# Track uploads by pubkey for listing
UPLOAD_LOG = os.path.join(DATA_DIR, ".uploads.jsonl")


def log(msg):
    """Log message to stderr if logging enabled."""
    if LOG_REQUESTS:
        timestamp = datetime.now().isoformat()
        sys.stderr.write(f"[mock-blossom {timestamp}] {msg}\n")
        sys.stderr.flush()


def compute_sha256(data):
    """Compute SHA-256 hash of binary data."""
    return hashlib.sha256(data).hexdigest()


def parse_nostr_auth(auth_header):
    """
    Parse the Nostr authorization header.
    Format: "Nostr <base64-encoded-signed-event>"

    Returns the decoded event JSON or None on error.
    """
    if not auth_header or not auth_header.startswith("Nostr "):
        return None

    try:
        b64_part = auth_header[6:]  # Strip "Nostr "
        event_json = base64.b64decode(b64_part).decode('utf-8')
        event = json.loads(event_json)
        return event
    except Exception as e:
        log(f"Failed to parse Nostr auth: {e}")
        return None


def validate_blossom_auth(event, expected_action, sha256=None, server_url=None):
    """
    Validate a kind 24242 Blossom auth event.

    Returns (valid, pubkey, error_msg).
    """
    if not event:
        return False, None, "No auth event provided"

    # Check kind
    if event.get("kind") != 24242:
        return False, None, f"Invalid kind: expected 24242, got {event.get('kind')}"

    pubkey = event.get("pubkey")
    if not pubkey:
        return False, None, "Missing pubkey in auth event"

    tags = event.get("tags", [])

    # Check 't' tag for action
    t_tag = None
    x_tag = None
    expiration_tag = None
    server_tag = None

    for tag in tags:
        if len(tag) >= 2:
            if tag[0] == "t":
                t_tag = tag[1]
            elif tag[0] == "x":
                x_tag = tag[1]
            elif tag[0] == "expiration":
                expiration_tag = tag[1]
            elif tag[0] == "server":
                server_tag = tag[1]

    # Validate action
    if t_tag != expected_action:
        return False, None, f"Action mismatch: expected {expected_action}, got {t_tag}"

    # Validate hash for upload/delete
    if expected_action in ("upload", "delete") and sha256:
        if x_tag and x_tag != sha256:
            return False, None, f"Hash mismatch: expected {sha256}, got {x_tag}"

    # Check expiration (allow 5 minute window)
    if expiration_tag:
        try:
            exp_time = int(expiration_tag)
            now = int(time.time())
            if exp_time < now:
                return False, None, f"Auth event expired at {exp_time}, current time {now}"
        except ValueError:
            pass  # Ignore invalid expiration

    # Note: We skip signature verification in the mock server
    # A real server would verify the signature here

    return True, pubkey, None


def save_upload_record(sha256, pubkey, mime_type, size):
    """Record an upload for the list endpoint."""
    record = {
        "sha256": sha256,
        "pubkey": pubkey,
        "type": mime_type,
        "size": size,
        "uploaded": int(time.time())
    }
    with open(UPLOAD_LOG, "a") as f:
        f.write(json.dumps(record) + "\n")


def get_uploads_for_pubkey(pubkey):
    """Get all uploads for a given pubkey."""
    if not os.path.exists(UPLOAD_LOG):
        return []

    uploads = []
    with open(UPLOAD_LOG, "r") as f:
        for line in f:
            try:
                record = json.loads(line.strip())
                if record.get("pubkey") == pubkey:
                    # Check if file still exists
                    file_path = os.path.join(DATA_DIR, record["sha256"])
                    if os.path.exists(file_path):
                        uploads.append(record)
            except json.JSONDecodeError:
                continue

    return uploads


class MockBlossomHandler(http.server.BaseHTTPRequestHandler):
    """HTTP request handler implementing BUD-01/BUD-02."""

    def log_message(self, format, *args):
        """Custom logging."""
        if LOG_REQUESTS:
            log(f"{self.address_string()} - {format % args}")

    def send_json_response(self, status, data):
        """Send a JSON response."""
        body = json.dumps(data).encode('utf-8')
        self.send_response(status)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', len(body))
        self.send_header('Access-Control-Allow-Origin', '*')
        self.end_headers()
        self.wfile.write(body)

    def send_error_json(self, status, message):
        """Send a JSON error response."""
        self.send_json_response(status, {"error": message})

    def do_OPTIONS(self):
        """Handle CORS preflight."""
        self.send_response(200)
        self.send_header('Access-Control-Allow-Origin', '*')
        self.send_header('Access-Control-Allow-Methods', 'GET, PUT, DELETE, HEAD, OPTIONS')
        self.send_header('Access-Control-Allow-Headers', 'Authorization, Content-Type')
        self.end_headers()

    def do_PUT(self):
        """Handle file upload (BUD-01)."""
        log(f"PUT {self.path}")

        # Check for /upload endpoint
        if not self.path.startswith('/upload'):
            self.send_error_json(404, "Not found")
            return

        # Check test modes
        if MODE == "server_error":
            self.send_error_json(500, "Internal server error (test mode)")
            return

        if MODE == "auth_fail":
            self.send_error_json(401, "Unauthorized (test mode)")
            return

        if MODE == "slow":
            time.sleep(2)

        # Get content length
        content_length = int(self.headers.get('Content-Length', 0))
        if content_length == 0:
            self.send_error_json(400, "No content provided")
            return

        # Check size limit in test mode
        if MODE == "size_limit" and content_length > 1024:
            self.send_error_json(413, "File too large (max 1KB in test mode)")
            return

        # Read file data
        file_data = self.rfile.read(content_length)

        # Compute hash
        sha256 = compute_sha256(file_data)

        # Validate auth header
        auth_header = self.headers.get('Authorization')
        event = parse_nostr_auth(auth_header)

        valid, pubkey, error = validate_blossom_auth(event, "upload", sha256)
        if not valid and MODE == "normal":
            # In normal mode, we still allow uploads without valid auth for testing
            # Real servers would reject here
            log(f"Auth validation warning: {error}")
            pubkey = pubkey or "test-pubkey"

        # Get content type
        content_type = self.headers.get('Content-Type', 'application/octet-stream')

        # Save file
        file_path = os.path.join(DATA_DIR, sha256)
        with open(file_path, 'wb') as f:
            f.write(file_data)

        # Save metadata
        meta_path = os.path.join(METADATA_DIR, f"{sha256}.json")
        meta = {
            "sha256": sha256,
            "type": content_type,
            "size": content_length,
            "uploaded": int(time.time()),
            "pubkey": pubkey
        }
        with open(meta_path, 'w') as f:
            json.dump(meta, f)

        # Record upload
        save_upload_record(sha256, pubkey, content_type, content_length)

        # Build URL
        host = self.headers.get('Host', f'localhost:{PORT}')
        url = f"http://{host}/{sha256}"

        # Send response
        response = {
            "sha256": sha256,
            "url": url,
            "type": content_type,
            "size": content_length,
            "uploaded": meta["uploaded"]
        }

        log(f"Upload successful: {sha256} ({content_length} bytes)")
        self.send_json_response(201, response)

    def do_GET(self):
        """Handle file retrieval and listing (BUD-02)."""
        log(f"GET {self.path}")

        path_parts = self.path.strip('/').split('/')

        # Handle /list/<pubkey> endpoint
        if len(path_parts) == 2 and path_parts[0] == 'list':
            pubkey = path_parts[1]
            uploads = get_uploads_for_pubkey(pubkey)

            # Build response
            blobs = []
            for record in uploads:
                file_path = os.path.join(DATA_DIR, record["sha256"])
                if os.path.exists(file_path):
                    host = self.headers.get('Host', f'localhost:{PORT}')
                    blobs.append({
                        "sha256": record["sha256"],
                        "url": f"http://{host}/{record['sha256']}",
                        "type": record.get("type", "application/octet-stream"),
                        "size": record.get("size", 0),
                        "uploaded": record.get("uploaded", 0)
                    })

            self.send_json_response(200, blobs)
            return

        # Handle file retrieval by hash
        if len(path_parts) >= 1:
            # Extract hash (may have extension)
            sha256 = path_parts[0].split('.')[0]

            # Validate hash format (64 hex chars)
            if len(sha256) != 64 or not all(c in '0123456789abcdef' for c in sha256.lower()):
                self.send_error_json(400, "Invalid hash format")
                return

            file_path = os.path.join(DATA_DIR, sha256)
            meta_path = os.path.join(METADATA_DIR, f"{sha256}.json")

            if not os.path.exists(file_path):
                self.send_error_json(404, "Blob not found")
                return

            # Load metadata for content type
            content_type = "application/octet-stream"
            if os.path.exists(meta_path):
                with open(meta_path, 'r') as f:
                    meta = json.load(f)
                    content_type = meta.get("type", content_type)

            # Send file
            with open(file_path, 'rb') as f:
                data = f.read()

            self.send_response(200)
            self.send_header('Content-Type', content_type)
            self.send_header('Content-Length', len(data))
            self.send_header('Access-Control-Allow-Origin', '*')
            self.end_headers()
            self.wfile.write(data)
            return

        self.send_error_json(404, "Not found")

    def do_HEAD(self):
        """Check if file exists."""
        log(f"HEAD {self.path}")

        path_parts = self.path.strip('/').split('/')
        if len(path_parts) >= 1:
            sha256 = path_parts[0].split('.')[0]
            file_path = os.path.join(DATA_DIR, sha256)

            if os.path.exists(file_path):
                meta_path = os.path.join(METADATA_DIR, f"{sha256}.json")
                content_type = "application/octet-stream"
                file_size = os.path.getsize(file_path)

                if os.path.exists(meta_path):
                    with open(meta_path, 'r') as f:
                        meta = json.load(f)
                        content_type = meta.get("type", content_type)

                self.send_response(200)
                self.send_header('Content-Type', content_type)
                self.send_header('Content-Length', file_size)
                self.send_header('Access-Control-Allow-Origin', '*')
                self.end_headers()
            else:
                self.send_response(404)
                self.end_headers()
        else:
            self.send_response(404)
            self.end_headers()

    def do_DELETE(self):
        """Handle file deletion."""
        log(f"DELETE {self.path}")

        if MODE == "auth_fail":
            self.send_error_json(401, "Unauthorized (test mode)")
            return

        path_parts = self.path.strip('/').split('/')
        if len(path_parts) >= 1:
            sha256 = path_parts[0].split('.')[0]

            # Validate auth
            auth_header = self.headers.get('Authorization')
            event = parse_nostr_auth(auth_header)

            valid, pubkey, error = validate_blossom_auth(event, "delete", sha256)
            if not valid and MODE == "normal":
                log(f"Auth validation warning for delete: {error}")

            file_path = os.path.join(DATA_DIR, sha256)
            meta_path = os.path.join(METADATA_DIR, f"{sha256}.json")

            if not os.path.exists(file_path):
                self.send_error_json(404, "Blob not found")
                return

            try:
                os.remove(file_path)
                if os.path.exists(meta_path):
                    os.remove(meta_path)
                self.send_json_response(200, {"message": "Blob deleted"})
            except OSError as e:
                self.send_error_json(500, f"Failed to delete: {e}")
        else:
            self.send_error_json(400, "Invalid path")


class ThreadedHTTPServer(socketserver.ThreadingMixIn, socketserver.TCPServer):
    """Threaded HTTP server for handling concurrent requests."""
    allow_reuse_address = True
    daemon_threads = True


def main():
    """Run the mock Blossom server."""
    log(f"Starting Mock Blossom Server on port {PORT}")
    log(f"Mode: {MODE}")
    log(f"Data directory: {DATA_DIR}")

    with ThreadedHTTPServer(("127.0.0.1", PORT), MockBlossomHandler) as httpd:
        try:
            httpd.serve_forever()
        except KeyboardInterrupt:
            log("Shutting down...")
            httpd.shutdown()


if __name__ == '__main__':
    main()
