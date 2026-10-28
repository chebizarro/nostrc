#!/usr/bin/env python3
"""EOSE-bounded signed state against a real git-http-backend fetch."""
import json
import os
from pathlib import Path
import subprocess
import struct
import sys
import tempfile
import threading
from urllib.parse import urlsplit

from relay_fixture import Relay, Server

NPUB = "npub10xlxvlhemja6c4dqv22uapctqupfhlxm9h8z3k2e72q4k9hcz7vqpkge6d"


def git(*args):
    return subprocess.check_output(["git", *map(str, args)], text=True).strip()


class FetchRelay(Relay):
    def frame(self, message):
        payload = json.dumps(message).encode()
        if len(payload) <= 65535:
            return super().frame(message)
        self.wfile.write(b"\x81\x7f" + struct.pack("!Q", len(payload)) + payload)
        self.wfile.flush()

    def message(self, message, mode):
        if message[0] == "AUTH":
            event = message[1]
            tags = dict(event["tags"])
            assert event["kind"] == 22242 and event["content"] == ""
            assert tags["challenge"] == "fixture-challenge"
            assert tags["relay"].rstrip("/") == f"ws://127.0.0.1:{self.server.server_address[1]}"
            with self.server.lock:
                self.server.auth_count += 1
                assert self.server.auth_count == 1, "unbounded AUTH retry"
            self.authenticated = True
            self.frame(["OK", event["id"], True, ""])
            return
        if message[0] != "REQ":
            return
        sub, filt = message[1:3]
        assert filt["#d"] == ["repo"] and len(filt["authors"]) == 1
        assert filt["limit"] == 1 and filt["kinds"] in ([30617], [30618])
        if filt["kinds"] == [30617]:
            with self.server.lock:
                self.server.announcement_queries += 1
            if self.server.announcement is not None:
                self.frame(["EVENT", sub, self.server.announcement])
            self.frame(["EOSE", sub])
            return
        with self.server.lock:
            self.server.state_queries += 1
            assert self.server.http_requests > 0, "state was queried before Smart HTTP"
        if self.server.case == "closed":
            self.frame(["CLOSED", sub, "blocked: fixture refusal"])
            return
        if self.server.case in ("auth", "auth-success") and not getattr(self, "authenticated", False):
            self.frame(["AUTH", "fixture-challenge"])
            self.frame(["CLOSED", sub, "auth-required: fixture credentials"])
            return
        event = self.server.state
        if self.server.owner_state is not None and self.server.owner_state["pubkey"] in filt["authors"]:
            event = self.server.owner_state
        if event is not None and event["pubkey"] in filt["authors"]:
            self.frame(["EVENT", sub, event])
            self.frame(["EVENT", sub, event])  # replayed ID
        self.frame(["EOSE", sub])

    def http(self, request, headers):
        server = self.server
        with server.lock:
            server.http_requests += 1
        path = urlsplit(request[1])
        if headers.get("expect", "").lower() == "100-continue":
            self.wfile.write(b"HTTP/1.1 100 Continue\r\n\r\n")
            self.wfile.flush()
        if headers.get("transfer-encoding", "").lower() == "chunked":
            chunks = []
            while size := int(self.rfile.readline().split(b";", 1)[0], 16):
                chunks.append(self.rfile.read(size))
                assert self.rfile.read(2) == b"\r\n"
            assert self.rfile.readline() == b"\r\n"
            body = b"".join(chunks)
        else:
            body = self.rfile.read(int(headers.get("content-length", 0)))
        env = {"PATH": os.environ["PATH"], "GIT_PROJECT_ROOT": str(server.root),
               "GIT_HTTP_EXPORT_ALL": "1", "REQUEST_METHOD": request[0],
               "PATH_INFO": path.path, "QUERY_STRING": path.query,
               "CONTENT_TYPE": headers.get("content-type", ""),
               "CONTENT_LENGTH": str(len(body))}
        backend = subprocess.run([server.backend], input=body, env=env,
                                 capture_output=True, check=True, timeout=15)
        response_headers, payload = backend.stdout.split(b"\r\n\r\n", 1)
        status = b"200 OK"
        output = []
        for line in response_headers.split(b"\r\n"):
            if line.lower().startswith(b"status:"):
                status = line.split(b":", 1)[1].strip()
            else:
                output.append(line)
        self.wfile.write(b"HTTP/1.1 " + status + b"\r\n" + b"\r\n".join(output) +
                         b"\r\nContent-Length: " + str(len(payload)).encode() +
                         b"\r\nConnection: close\r\n\r\n" + payload)
        self.wfile.flush()


def run_case(binary, case):
    with tempfile.TemporaryDirectory(prefix="hanami-grasp-fetch-") as tmp:
        root = Path(tmp)
        work = root / "work"
        bare = root / NPUB / "repo.git"
        git("init", "-q", "-b", "main", work)
        git("-C", work, "-c", "user.name=fixture", "-c", "user.email=fixture@example.com",
            "commit", "-q", "--allow-empty", "-m", "fixture")
        tip = git("-C", work, "rev-parse", "HEAD")
        git("init", "--bare", "-q", bare)
        git("-C", work, "push", "-q", bare, "main")
        if case == "extra":
            git("--git-dir", bare, "update-ref", "refs/heads/extra", tip)
        state = None if case in ("none", "closed", "auth") else json.loads(
            subprocess.check_output([binary, "sign", tip, case], text=True))
        announcement = json.loads(subprocess.check_output(
            [binary, "sign", tip, "forged-announcement" if case == "forged" else "announcement"],
            text=True)) if case in ("maintainer", "forged") else None
        owner_state = json.loads(subprocess.check_output(
            [binary, "sign", tip, "owner-old"], text=True)) if case == "maintainer" else None
        with Server(("127.0.0.1", 0), FetchRelay) as server:
            server.root, server.case, server.state = root, case, state
            server.announcement, server.owner_state = announcement, owner_state
            server.backend = str(Path(git("--exec-path")) / "git-http-backend")
            assert Path(server.backend).is_file()
            server.http_requests = server.state_queries = server.announcement_queries = 0
            server.auth_count = 0
            server.lock = threading.Lock()
            worker = threading.Thread(target=server.serve_forever, daemon=True)
            worker.start()
            try:
                subprocess.run([binary, "fetch", case, str(root / "local"),
                    f"127.0.0.1:{server.server_address[1]}", tip],
                    check=True, timeout=45)
            finally:
                server.shutdown()
                worker.join()
            assert server.http_requests > 0 and server.announcement_queries > 0
            if case != "forged":
                assert server.state_queries > 0
            if case == "auth-success":
                assert server.auth_count == 1


for scenario in ("match", "maintainer", "forged", "mismatch", "extra", "missing", "oversize", "closed", "auth", "auth-success", "none"):
    run_case(sys.argv[1], scenario)
