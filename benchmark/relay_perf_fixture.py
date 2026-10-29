#!/usr/bin/env python3
"""Exercise the benchmark's real wire boundaries using only a loopback relay."""
import base64
import hashlib
import json
from pathlib import Path
import socketserver
import struct
import subprocess
import sys
import tempfile
import threading

# Signed kind-1 seed from tests/fuzz_seeds/envelopes/valid_signed_event.json.
EVENT = {
    "id": "ef00e16f6b8bc091ff2ebf87cb1d4a8d3d882983150a74806998b103cc2c20b8",
    "pubkey": "1b84c5567b126440995d3ed5aaba0565d71e1834604819ff9c17f5e9d5dd078f",
    "created_at": 1700000000, "kind": 1, "tags": [], "content": "hello world",
    "sig": "c65d23aab2e355682bd3621292d7f3608d7d094b83fadf4547b8c782653ff2957d064dd6acdad49bf759644e43fd14244d4556dc578f6a85bf717100b21de66d",
}


class Relay(socketserver.StreamRequestHandler):
    def frame(self, message):
        payload = json.dumps(message).encode()
        header = (bytes([0x81, len(payload)]) if len(payload) < 126 else
                  b"\x81\x7e" + struct.pack("!H", len(payload)))
        self.wfile.write(header + payload)
        self.wfile.flush()

    def handle(self):
        try:
            self.exchange()
        except (BrokenPipeError, ConnectionResetError):
            pass  # The benchmark intentionally closes on terminal outcomes.

    def exchange(self):
        request = self.rfile.readline().split()
        assert len(request) >= 2
        mode = request[1].decode().strip("/")
        headers = {}
        while line := self.rfile.readline().strip():
            key, value = line.decode().split(":", 1)
            headers[key.lower()] = value.strip()
        if mode == "unavailable":
            self.wfile.write(b"HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\n"
                             b"Connection: close\r\n\r\n")
            self.wfile.flush()
            return
        key = headers["sec-websocket-key"] + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
        accept = base64.b64encode(hashlib.sha1(key.encode()).digest()).decode()
        self.wfile.write(("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
                          "Connection: Upgrade\r\nSec-WebSocket-Accept: " + accept + "\r\n\r\n").encode())
        self.wfile.flush()
        while header := self.rfile.read(2):
            if len(header) != 2 or header[0] & 15 == 8:
                return
            length = header[1] & 127
            if length == 126:
                length = struct.unpack("!H", self.rfile.read(2))[0]
            elif length == 127:
                length = struct.unpack("!Q", self.rfile.read(8))[0]
            assert header[1] & 128, "client frame must be masked"
            mask = self.rfile.read(4)
            payload = self.rfile.read(length)
            message = json.loads(bytes(c ^ mask[i % 4] for i, c in enumerate(payload)))
            if message[0] == "CLOSE":
                return
            assert message[0] == "REQ", message
            assert message[2] == {"kinds": [1], "limit": 200}, message
            subid = message[1]
            if mode in ("events", "event-drop", "event-silent", "events-drop"):
                self.frame(["EVENT", subid, EVENT])
                self.frame(["EVENT", subid, EVENT])  # replay must not add a sample
            if mode in ("events", "empty", "events-drop"):
                self.frame(["EOSE", subid])
            elif mode == "closed":
                self.frame(["CLOSED", subid, "auth-required: fixture read policy"])
            if mode in ("drop", "event-drop", "events-drop"):
                return
            # Silent modes block on client input until its deadline causes CLOSE.


class Server(socketserver.ThreadingTCPServer):
    daemon_threads = True
    allow_reuse_address = True

    def handle_error(self, request, client_address):
        self.errors.append(sys.exc_info()[1])


def main():
    binary = str(Path(sys.argv[1]).resolve())
    with Server(("127.0.0.1", 0), Relay) as server, tempfile.TemporaryDirectory() as work:
        server.errors = []
        worker = threading.Thread(target=server.serve_forever, daemon=True)
        worker.start()
        base = f"ws://127.0.0.1:{server.server_address[1]}"

        def run(modes, outcomes, events, samples):
            result = subprocess.run([binary, "--timeout-ms", "400"] +
                                    [base + "/" + mode for mode in modes],
                                    cwd=work, capture_output=True, text=True, timeout=20)
            print(result.stdout, end="")
            assert result.returncode == (0 if all(samples) else 1), result.stderr
            lines = [line for line in result.stdout.splitlines() if line.startswith("relay=")]
            assert len(lines) == len(modes), result.stdout
            for line, outcome, count, sample in zip(lines, outcomes, events, samples):
                assert f"outcome={outcome} " in line, line
                assert f"events_received={count} " in line, line
                assert f"eose_samples={sample}" in line, line
                assert (" eose_ms=" in line) == bool(sample), line
                assert (" first_event_ms=" in line) == bool(count), line
                if outcome == "closed":
                    assert "reason=auth-required: fixture read policy" in line, line
            metrics = dict(line.split("=", 1) for line in
                           Path(work, "baseline_metrics.txt").read_text().splitlines())
            assert int(metrics["eose_samples"]) == sum(samples), metrics
            assert int(metrics["relays_failed"]) == len(modes) - sum(samples), metrics
            assert ("avg_eose_ms" in metrics) == bool(sum(samples)), metrics
            assert ("p99_eose_ms" in metrics) == bool(sum(samples)), metrics

        try:
            run(["events"], ["completed"], [1], [1])
            run(["empty"], ["completed"], [0], [1])
            run(["closed"], ["closed"], [0], [0])
            run(["drop"], ["transport_loss"], [0], [0])
            run(["event-drop"], ["transport_loss"], [1], [0])
            run(["silent"], ["timeout"], [0], [0])
            run(["event-silent"], ["timeout"], [1], [0])
            run(["unavailable"], ["connect_failure"], [0], [0])
            run(["events-drop"], ["completed"], [1], [1])
            run(["events", "closed"], ["completed", "closed"], [1, 0], [1, 0])
            for args in ([], ["--timeout-ms"], ["--timeout-ms", "0"],
                         ["--timeout-ms", "-1"], ["--timeout-ms", "garbage"],
                         [base] * 9):
                result = subprocess.run([binary] + args, cwd=work, capture_output=True, timeout=5)
                assert result.returncode == 2, (args, result)
        finally:
            server.shutdown()
            worker.join()
        assert not server.errors, server.errors


if __name__ == "__main__":
    main()
