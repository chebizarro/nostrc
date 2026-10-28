#!/usr/bin/env python3
"""Real syncd pointer publish against relayd VSP-01 and unauthenticated modes."""
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import time

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


with tempfile.TemporaryDirectory(prefix="syncd-publish-auth-") as tmp:
    root = Path(tmp)
    run_case(root, "vsp", MEMBER_SECRET, "ok")
    # Reuse one allowlist policy but a fresh relay process for the outsider.
    run_case(root, "vsp2", OUTSIDER_SECRET, "restricted")
    run_case(root, "open", MEMBER_SECRET, "ok")
