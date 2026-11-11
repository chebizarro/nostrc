#!/usr/bin/env python3
"""Disposable local coturn fixture; wait for listener readiness, not a sleep."""
import os
from pathlib import Path
import selectors
import subprocess
import sys
import tempfile
import time

# For the no-local policy test, assign TEST-NET-1 to loopback in a disposable,
# network-isolated container. No production-policy bypass or public server.
address = os.environ.get("GH_CALL_TEST_TURN_ADDRESS", "127.0.0.1")
os.environ["GH_CALL_TEST_TURN_URL"] = "turn://" + address + ":3478"
with tempfile.TemporaryDirectory(prefix="gh-voice-media-turn-") as directory:
    command = ["turnserver", "--no-cli", "--no-tls", "--no-dtls", "--fingerprint",
               "--lt-cred-mech", "--realm=gh-voice-media",
               "--user=gh-voice-media:disposable-test-only", "--listening-ip=" + address,
               "--relay-ip=" + address, "--allow-loopback-peers", "--no-multicast-peers",
               "--listening-port=3478", "--min-port=49160", "--max-port=49200",
               "--relay-threads=1", "--log-file=stdout", "--simple-log", "-v",
               "--pidfile=" + directory + "/turn.pid", "--userdb=" + directory + "/turn.db"]
    server = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, bufsize=0)
    selector = selectors.DefaultSelector()
    selector.register(server.stdout, selectors.EVENT_READ)
    try:
        output = b""
        deadline = time.monotonic() + 10
        while b"UDP listener opened on" not in output:
            if not selector.select(max(0, deadline - time.monotonic())):
                raise RuntimeError("coturn did not report UDP listener readiness")
            chunk = os.read(server.stdout.fileno(), 65536)
            if not chunk:
                raise RuntimeError("coturn exited before readiness: " + output.decode(errors="replace"))
            output += chunk
        # Keep log output draining while both children perform the media test.
        # Only a disposable server credential occurs in this test fixture.
        with tempfile.TemporaryFile() as log:
            drain = subprocess.Popen(["cat"], stdin=server.stdout, stdout=log)
            try:
                subprocess.run([sys.executable, str(Path(__file__).with_name("two_process.py")), sys.argv[1]], check=True)
            finally:
                server.terminate()
                server.wait(timeout=10)
                drain.wait(timeout=10)
    finally:
        selector.close()
        if server.poll() is None:
            server.kill()
        server.wait()
        server.stdout.close()
