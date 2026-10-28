#!/usr/bin/env python3
"""Real syncd pointer publish against relayd and a scripted AUTH retry relay."""
import base64
import hashlib
import http.server
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import threading
import time

ROOT = Path(__file__).resolve().parents[4]
sys.path.insert(0, str(ROOT / "tools/vaultctl/tests"))
from fixtures import Handler, verify

PUSH, RELAYD = sys.argv[1:3]
MEMBER_SECRET = "00" * 31 + "01"
OUTSIDER_SECRET = "00" * 31 + "02"
MEMBER_PUBKEY = "79be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798"


def free_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def wait_listening(proc, port):
    deadline = time.monotonic() + 10
    while time.monotonic() < deadline:
        if proc.poll() is not None:
            raise AssertionError(f"relayd exited: {proc.returncode}")
        with socket.socket() as sock:
            sock.settimeout(0.2)
            if sock.connect_ex(("127.0.0.1", port)) == 0:
                return
    raise AssertionError("relayd did not listen")


def run_case(root, mode, secret, expected):
    port = free_port()
    url = f"ws://127.0.0.1:{port}"
    directory = root / mode
    directory.mkdir()
    config = f'listen = "127.0.0.1:{port}"\nstorage_driver = "nostrdb"\n'
    if mode.startswith("vsp"):
        allowlist = directory / "allowlist.txt"
        allowlist.write_text(MEMBER_PUBKEY + "\n")
        config += f'auth = "optional"\nrelay_url = "{url}"\nvault_allowlist = "{allowlist}"\n'
    else:
        config += 'auth = "off"\n'
    (directory / "relay.toml").write_text(config)
    with (directory / "relayd.log").open("w") as log:
        proc = subprocess.Popen([RELAYD], cwd=directory, stdout=log, stderr=log)
        try:
            wait_listening(proc, port)
            result = subprocess.run([PUSH, "--relay", url, secret, expected],
                                    capture_output=True, text=True, timeout=25)
            if result.returncode:
                raise AssertionError(f"{mode}/{expected} failed: {result.stdout}\n{result.stderr}")
        finally:
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait(timeout=5)
    print(f"{mode}/{expected}: OK")


class ScriptedHandler(Handler):
    def do_GET(self):
        if self.headers.get("Upgrade", "").lower() == "websocket":
            return self.websocket()
        self.response(404, b"")

    def websocket(self):
        key = self.headers["Sec-WebSocket-Key"]
        accept = base64.b64encode(hashlib.sha1(
            (key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode()
        ).digest()).decode()
        self.send_response(101)
        self.send_header("Upgrade", "websocket")
        self.send_header("Connection", "Upgrade")
        self.send_header("Sec-WebSocket-Accept", accept)
        protocol = self.headers.get("Sec-WebSocket-Protocol")
        if protocol:
            self.send_header("Sec-WebSocket-Protocol", protocol.split(",")[0])
        self.end_headers()
        self.wfile.flush()
        self.connection.settimeout(10)
        challenge = "first-pointer-challenge"
        try:
            while True:
                message = self.recv()
                if message is None:
                    continue
                verb, event = message[:2]
                assert verb in ("EVENT", "AUTH"), message
                verify(event)
                assert event["pubkey"] == MEMBER_PUBKEY
                self.server.frames.append((verb, event["id"]))
                if verb == "EVENT":
                    assert event["kind"] == 30078
                    event_count = sum(kind == "EVENT" for kind, _ in self.server.frames)
                    if event_count == 1:
                        self.frame(["OK", event["id"], False,
                                    "auth-required: authenticate first"])
                        self.frame(["AUTH", challenge])
                    elif event_count == 2:
                        assert sum(kind == "AUTH" for kind, _ in self.server.frames) == 1
                        if self.server.reject_retry:
                            self.frame(["OK", event["id"], False,
                                        "auth-required: still required"])
                            challenge = "second-pointer-challenge"
                            self.frame(["AUTH", challenge])
                        else:
                            self.frame(["OK", event["id"], True, ""])
                    else:
                        # Answer unexpected extra retries so the client finishes;
                        # the exact-frame assertion below will reject the run.
                        self.frame(["OK", event["id"], True, ""])
                else:
                    assert event["kind"] == 22242
                    assert ["challenge", challenge] in event["tags"]
                    assert ["relay", self.server.ws] in event["tags"]
                    self.frame(["OK", event["id"], True, ""])
        except (EOFError, ConnectionResetError, BrokenPipeError):
            pass
        except Exception as exc:
            self.server.failures.append(repr(exc))
        finally:
            self.server.done.set()


class ScriptedRelay(http.server.ThreadingHTTPServer):
    daemon_threads = True

    def __init__(self, reject_retry):
        super().__init__(("127.0.0.1", 0), ScriptedHandler)
        self.ws = f"ws://127.0.0.1:{self.server_port}/relay"
        self.reject_retry = reject_retry
        self.frames = []
        self.failures = []
        self.done = threading.Event()
        self.thread = threading.Thread(target=self.serve_forever, daemon=True)
        self.thread.start()

    def finish(self):
        self.shutdown()
        self.server_close()
        self.thread.join()


def run_scripted(reject_retry):
    relay = ScriptedRelay(reject_retry)
    expected = "auth-required" if reject_retry else "ok"
    try:
        result = subprocess.run([PUSH, "--relay", relay.ws, MEMBER_SECRET, expected],
                                capture_output=True, text=True, timeout=20)
        assert relay.done.wait(5), "scripted relay did not see client disconnect"
        assert result.returncode == 0, result.stdout + result.stderr
        assert not relay.failures, relay.failures
        assert [kind for kind, _ in relay.frames] == ["EVENT", "AUTH", "EVENT"], relay.frames
        assert relay.frames[0][1] == relay.frames[2][1], relay.frames
        assert relay.frames[1][1] != relay.frames[0][1], relay.frames
    finally:
        relay.finish()
    print(f"scripted/{expected}: one AUTH, one republish: OK")


with tempfile.TemporaryDirectory(prefix="syncd-publish-auth-") as tmp:
    root = Path(tmp)
    run_case(root, "vsp", MEMBER_SECRET, "ok")
    # Reuse one allowlist policy but a fresh relay process for the outsider.
    run_case(root, "vsp2", OUTSIDER_SECRET, "restricted")
    run_case(root, "open", MEMBER_SECRET, "ok")
    run_scripted(False)
    run_scripted(True)
