#!/usr/bin/env python3
"""Loopback-only Nostr wire fixture; no external services or timing sleeps."""
import base64
import hashlib
import json
import socketserver
import struct
import subprocess
import sys
import threading
import socket


class Relay(socketserver.StreamRequestHandler):
    def frame(self, message):
        payload = json.dumps(message).encode()
        header = bytes([0x81, len(payload)]) if len(payload) < 126 else b"\x81\x7e" + struct.pack("!H", len(payload))
        self.wfile.write(header + payload)
        self.wfile.flush()

    def handle(self):
        request = self.rfile.readline().decode().split()
        if len(request) < 2:
            return
        mode = request[1].strip("/")
        headers = {}
        while line := self.rfile.readline().strip():
            key, value = line.decode().split(":", 1)
            headers[key.lower()] = value.strip()
        if mode == "unavailable":
            # A relay that cannot be connected: the websocket upgrade fails.
            self.wfile.write(b"HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\n"
                             b"Connection: close\r\n\r\n")
            self.wfile.flush()
            return
        if "sec-websocket-key" not in headers:
            return self.http(request, headers)
        accept = base64.b64encode(hashlib.sha1((headers["sec-websocket-key"] + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode()).digest()).decode()
        self.wfile.write(("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: " + accept + "\r\n\r\n").encode())
        self.wfile.flush()
        if mode == "auth-early":
            self.frame(["AUTH", "fixture-challenge"])
        try:
            while header := self.rfile.read(2):
                if len(header) != 2 or header[0] & 15 == 8:
                    return
                length = header[1] & 127
                if length == 126:
                    length = struct.unpack("!H", self.rfile.read(2))[0]
                elif length == 127:
                    length = struct.unpack("!Q", self.rfile.read(8))[0]
                mask = self.rfile.read(4) if header[1] & 128 else None
                payload = self.rfile.read(length)
                if mask:
                    payload = bytes(c ^ mask[i % 4] for i, c in enumerate(payload))
                if header[0] & 15 == 1:
                    self.message(json.loads(payload), mode)
        except (BrokenPipeError, ConnectionResetError):
            return

    def http(self, request, headers):
        raise AssertionError(f"unexpected HTTP request: {request}")

    def message(self, message, mode):
        boundary = None
        if "-boundary-" in mode:
            mode, key = mode.split("-boundary-", 1)
            with self.server.lock:
                boundary = self.server.boundaries.setdefault(key, threading.Event())
        if message[0] == "CLOSE" and boundary and mode != "drop":
            # CLOSE proves the consumer handled this peer's EOSE/CLOSED.
            # Only then drop the other, necessarily awaited, subscription.
            boundary.set()
        authenticated = getattr(self, "authenticated", False)
        auth_modes = ("publish-auth", "auth-denied", "auth-repeat", "auth-no-ok", "auth-nosigner", "auth-early", "auth-late", "auth-next")
        if message[0] == "AUTH":
            event = message[1]
            assert event["kind"] == 22242 and event["content"] == ""
            tags = dict(event["tags"])
            assert tags["challenge"] == getattr(self, "challenge", "fixture-challenge")
            assert tags["relay"] == f"ws://127.0.0.1:{self.server.server_address[1]}/{mode}"
            assert len(event["sig"]) == 128 and len(event["id"]) == 64
            self.auth_attempts = getattr(self, "auth_attempts", 0) + 1
            assert self.auth_attempts <= (2 if mode == "auth-next" else 1), "unbounded AUTH retry"
            if mode == "auth-no-ok":
                return
            self.authenticated = mode != "auth-denied"
            self.frame(["OK", event["id"], self.authenticated,
                        "" if self.authenticated else "restricted: fixture denied"])
        elif message[0] == "EVENT":
            event = message[1]
            assert event["kind"] != 22242, "NIP-42 must use AUTH, not EVENT"
            if mode == "auth-next" and getattr(self, "next_challenge_needed", False):
                self.challenge = "fixture-challenge-2"
                self.authenticated = authenticated = False
                self.next_challenge_needed = False
            if mode in auth_modes and (not authenticated or mode == "auth-repeat"):
                previous_id = getattr(self, "application_id", event["id"])
                assert event["id"] == previous_id, "retry changed the signed application event"
                self.application_id = event["id"]
                # Deliberately reject before sending the challenge: caller must
                # wait on AUTH delivery, not sleep or assume it arrived first.
                if mode != "auth-late":
                    self.frame(["OK", event["id"], False, "auth-required: fixture credentials"])
                self.frame(["AUTH", getattr(self, "challenge", "fixture-challenge")])
                return
            if mode in auth_modes and hasattr(self, "application_id"):
                assert event["id"] == self.application_id
                del self.application_id
            if mode == "auth-next":
                self.next_challenge_needed = True
            accepted = mode != "reject"
            if mode == "refdb-acceptance":
                with self.server.lock:
                    self.server.refdb_writes += 1
                    accepted = self.server.refdb_writes not in (3, 4, 5)
            if accepted:
                with self.server.lock:
                    previous = self.server.events.get(mode)
                    if mode == "refdb-acceptance" and previous:
                        assert event["created_at"] > previous["created_at"], "state did not supersede last ACK"
                    self.server.events[mode] = event
            reason = "blocked: fixture refusal"
            self.frame(["OK", event["id"], accepted, "" if accepted else reason])
        elif message[0] == "REQ":
            subid, filters = message[1], message[2]
            if mode == "publish-auth" and authenticated:
                self.frame(["AUTH", "fixture-challenge"])  # already completed, not a new boundary
            if mode.startswith("refdb"):
                assert filters["kinds"] == [30618]
                assert filters["authors"]
                assert filters["#d"] == ["test-repo"]
                with self.server.lock:
                    self.server.refdb_requests += 1
            else:
                assert filters["kinds"][0] in (30617, 30618)
                expected_id = "integration-test" if mode == "integration" else "repo"
                assert filters["authors"] and filters["#d"] == [expected_id]
            assert filters["limit"] == 1
            if mode == "drop":
                assert boundary and boundary.wait(30), "peer boundary was not consumed"
                self.request.shutdown(socket.SHUT_RDWR)
                return
            if mode == "closed":
                self.frame(["CLOSED", subid, "blocked: fixture refusal"])
            elif mode == "restricted":
                self.frame(["CLOSED", subid, "restricted: fixture read policy"])
            elif mode in ("auth", "query-auth") and not authenticated:
                self.frame(["AUTH", "fixture-challenge"])
                self.frame(["CLOSED", subid, "auth-required: fixture requires credentials"])
            else:
                with self.server.lock:
                    event = self.server.events.get(mode)
                if event and mode == "invalid":
                    event = dict(event, sig="0" * 128)
                if event and event["kind"] in filters["kinds"] and event["pubkey"] in filters["authors"]:
                    self.frame(["EVENT", subid, event])
                    self.frame(["EVENT", subid, event])
                if mode == "event-drop":
                    self.request.shutdown(socket.SHUT_RDWR)
                    return
                if mode != "no-eose":
                    self.frame(["EOSE", subid])


class Server(socketserver.ThreadingTCPServer):
    daemon_threads = True
    allow_reuse_address = True


def main():
    with Server(("127.0.0.1", 0), Relay) as server:
        server.events = {}
        server.boundaries = {}
        server.refdb_requests = 0
        server.refdb_writes = 0
        server.lock = threading.Lock()
        worker = threading.Thread(target=server.serve_forever, daemon=True)
        worker.start()
        try:
            mode = sys.argv[2] if len(sys.argv) > 2 else ""
            url = f"ws://127.0.0.1:{server.server_address[1]}"
            if mode:
                url += f"/{mode}"
            result = subprocess.run([sys.argv[1], url], timeout=25 if mode.startswith("refdb") else 75)
        finally:
            server.shutdown()
            worker.join()
        if mode == "refdb-acceptance":
            assert server.refdb_writes == 6, "ref operations did not reach the relay"
        if mode.startswith("refdb") and server.refdb_requests == 0:
            sys.exit("refdb test did not query the loopback relay")
        sys.exit(result.returncode)


if __name__ == "__main__":
    main()
