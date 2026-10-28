#!/usr/bin/env python3
"""GRASP root relay + real Smart HTTP. Protocol observations, never sleeps."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import threading
from urllib.parse import urlsplit

from relay_fixture import Relay, Server
from grasp_nip11_fixture import FAILURES, serve_nip11

NPUB = "npub10xlxvlhemja6c4dqv22uapctqupfhlxm9h8z3k2e72k5cs2kd7pqf0h2zx"


class Grasp(Relay):
    def message(self, message, mode):
        server = self.server
        if message[0] == "AUTH":
            event = message[1]
            tags = dict(event["tags"])
            assert event["kind"] == 22242 and event["content"] == ""
            assert tags["relay"].rstrip("/") == f"ws://127.0.0.1:{server.server_address[1]}"
            assert tags["challenge"] == "grasp-challenge"
            self.authenticated = True
            self.frame(["OK", event["id"], True, ""])
            with server.lock:
                server.auth_count += 1
        elif message[0] == "REQ":
            sub, filt = message[1:3]
            assert filt["kinds"] == [30618] and filt["#d"] == ["repo"]
            assert len(filt["authors"]) == 1 and filt["limit"] == 1
            if server.case == "auth-success" and not getattr(self, "authenticated", False):
                self.frame(["AUTH", "grasp-challenge"])
                self.frame(["CLOSED", sub, "auth-required: fixture credentials"])
                return
            if server.case == "closed":
                self.frame(["CLOSED", sub, "blocked: fixture refusal"])
                return
            with server.lock:
                event = server.latest
                if event:
                    self.frame(["EVENT", sub, event])
                self.frame(["EOSE", sub])
        elif message[0] == "EVENT":
            event = message[1]
            assert event["kind"] == 30618
            with server.lock:
                if mode == "replica":
                    self.frame(["OK", event["id"], True, ""])
                    return
                server.announcements.append(event)
                # A seeded baseline is sent directly by the test before push.
                baseline = len(server.announcements) == 1 and server.case != "first-fail"
                accepted = baseline or (server.case != "reject" and not
                    (server.case == "correction-reject" and server.http_requests))
                if accepted:
                    server.latest = event
                    (server.root / "state.json").write_text(json.dumps(event))
                if server.case == "no-ok" and len(server.announcements) == 2:
                    # Store the intended state but withhold its OK entirely.
                    server.trace.append(("NO_OK", event["id"], True))
                    return
                self.frame(["OK", event["id"], accepted,
                            "purgatory: waiting for git data" if accepted else "blocked: fixture refusal"])
                server.trace.append(("OK", event["id"], accepted))

    def http(self, request, headers):
        server = self.server
        if serve_nip11(self, request, headers):
            return
        with server.lock:
            assert server.nip11_requests > 0
            assert server.latest, "Git HTTP started before state acceptance"
            assert server.trace[-1][0] == "OK" and server.trace[-1][2]
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
               "CONTENT_LENGTH": str(len(body)), "REMOTE_USER": "fixture"}
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
    with tempfile.TemporaryDirectory(prefix="hanami-grasp-") as tmp:
        root = Path(tmp)
        bare = root / NPUB / "repo.git"
        subprocess.run(["git", "init", "--bare", "-q", str(bare)], check=True)
        subprocess.run(["git", "--git-dir", str(bare), "config", "http.receivepack", "true"], check=True)
        hook = bare / "hooks/pre-receive"
        hook.write_text("#!/usr/bin/env python3\n" +
            "import json,sys\n" +
            f"state=json.load(open({str(root / 'state.json')!r}))\n" +
            "refs={t[0]:t[1] for t in state['tags'] if t[0].startswith('refs/')}\n" +
            "for old,new,ref in map(str.split,sys.stdin):\n" +
            " assert (new == '0'*40 and ref not in refs) or refs.get(ref) == new\n" +
            ("sys.exit(1)\n" if case in ("fail-push", "first-fail", "correction-reject") else ""))
        hook.chmod(0o755)
        with Server(("127.0.0.1", 0), Grasp) as server:
            server.root, server.case = root, case
            server.backend = str(Path(subprocess.check_output(["git", "--exec-path"], text=True).strip()) / "git-http-backend")
            assert Path(server.backend).is_file(), "git http-backend is required"
            server.latest = None
            server.announcements, server.trace = [], []
            server.http_requests = server.nip11_requests = 0
            server.auth_count = 0
            server.lock = threading.Lock()
            worker = threading.Thread(target=server.serve_forever, daemon=True)
            worker.start()
            try:
                subprocess.run([binary, f"127.0.0.1:{server.server_address[1]}", case, str(root / "local")],
                               check=True, timeout=90)
            finally:
                server.shutdown()
                worker.join()
            assert server.nip11_requests == (2 if case in ("success", "auth-success", "nip11-no-curation") else 1)
            if case in FAILURES:
                assert server.http_requests == 0 and not server.announcements
                assert server.latest is None
                continue_refs = subprocess.check_output(
                    ["git", "--git-dir", str(bare), "for-each-ref"], text=True)
                assert not continue_refs, "refusal changed Git refs"
            elif case in ("reject", "closed", "no-ok"):
                assert server.http_requests == 0
            else:
                assert server.http_requests > 0, "push entrypoint never reached Smart HTTP"
            if case == "auth-success":
                assert server.auth_count > 0
            if case == "success":
                refs = {t[0]: t[1] for t in server.latest["tags"] if t[0].startswith("refs/")}
                assert "refs/heads/main" not in refs, "deletion was not announced"
                assert "refs/heads/unrelated" in refs, "selected push erased unrelated state"
                assert len(server.announcements) == 3  # baseline, default push, delete
            if case in ("fail-push", "first-fail", "no-ok"):
                before = server.announcements[0] if case != "first-fail" else {"tags": [["d", "repo"]]}
                intended, correction = server.announcements[-2:]
                assert correction["created_at"] > intended["created_at"]
                assert correction["tags"] == before["tags"], "failed push left intended state advertised"


for scenario in ("success", "fail-push", "first-fail", "reject", "correction-reject", "closed", "auth-success", "no-ok", "nip11-no-curation", *FAILURES):
    run_case(sys.argv[1], scenario)
