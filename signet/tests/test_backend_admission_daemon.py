#!/usr/bin/env python3
"""Real daemon + relay + authenticated crypto peer; no fake authority or hooks.

EOSE and a fresh request are sent in one socket write, behind a signed historical
request. Neither a sleep nor a client-side EOSE callback may gate publication.
The crypto peer implements no backend methods and never opens admission storage.
"""
import hashlib
import json
import os
from pathlib import Path
import queue
import re
import struct
import subprocess
import sys
import tempfile
import threading
import time
import uuid

from test_relay_pool_wire import Relay

AUTHORITY = "11111111-1111-1111-1111-111111111111"
HASH = "sha256:" + "a" * 64


def frame(value):
    payload = json.dumps(value, separators=(",", ":")).encode()
    assert len(payload) < 65536, "fixture_frame_limit"
    header = (bytes([0x81, len(payload)]) if len(payload) < 126 else
              b"\x81\x7e" + struct.pack("!H", len(payload)))
    return header + payload


class CryptoPeer:
    def __init__(self, binary, directory, env, stderr):
        self.process = subprocess.Popen([binary, str(directory)], stdin=subprocess.PIPE,
                                        stdout=subprocess.PIPE, stderr=stderr,
                                        text=True, bufsize=1, env=env)
        self.messages = queue.Queue()

        def read():
            for line in self.process.stdout:
                self.messages.put(json.loads(line))
            self.messages.put(None)

        self.reader = threading.Thread(target=read, daemon=True)
        self.reader.start()
        self.ready = self.receive()
        assert self.ready["type"] == "ready", "fixture_crypto_not_ready"

    def receive(self):
        value = self.messages.get(timeout=10)
        assert value is not None, "fixture_crypto_exited"
        return value

    def call(self, value):
        self.process.stdin.write(json.dumps(value) + "\n")
        self.process.stdin.flush()
        return self.receive()

    def close(self):
        self.process.stdin.close()
        try:
            self.process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait()
            raise AssertionError("fixture_crypto_did_not_stop")
        self.reader.join(timeout=2)
        self.process.stdout.close()
        assert self.process.returncode == 0, "fixture_crypto_failed"


def canonical_unsigned_rumor(rumor, sender, recipient):
    """Independent NIP-01 canonical bytes, not the C event hash/validator."""
    assert set(rumor) == {"id", "pubkey", "created_at", "kind", "tags", "content"}, "unsigned_rumor_fields"
    assert rumor["pubkey"] == sender and rumor["kind"] == 14, "rumor_sender_or_kind"
    assert type(rumor["created_at"]) is int and rumor["created_at"] > 0, "rumor_time"
    assert rumor["tags"] == [["p", recipient]], "rumor_recipient"
    assert isinstance(rumor["content"], str), "rumor_content_type"
    assert isinstance(rumor["id"], str) and re.fullmatch(r"[0-9a-f]{64}", rumor["id"]), "rumor_id_format"
    canonical = [0, rumor["pubkey"], rumor["created_at"], rumor["kind"], rumor["tags"], rumor["content"]]
    computed = hashlib.sha256(json.dumps(canonical, ensure_ascii=False, separators=(",", ":")).encode("utf-8")).hexdigest()
    assert rumor["id"] == computed, "reply_rumor_id_mismatch"


def canonical_vector_checks():
    sender = "79be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798"
    recipient = "c6047f9441ed7d6d3045406e95c07cd85c778e4b8cef3ca7abac09b95c709ee5"
    rumor = {"id": "d922a259cc1853e89f58da35e906d5759d20ac7141c90d40b304f0d358803692",
             "pubkey": sender, "created_at": 1700000000, "kind": 14,
             "tags": [["p", recipient]], "content": "quote:\" slash:\\ tab:\t line:\n snowman:☃"}
    canonical_unsigned_rumor(rumor, sender, recipient)
    for invalid in ({**rumor, "id": "0" * 64}, {k: v for k, v in rumor.items() if k != "id"},
                    {**rumor, "content": "changed"}, {**rumor, "sig": ""}):
        try:
            canonical_unsigned_rumor(invalid, sender, recipient)
        except AssertionError:
            pass
        else:
            raise AssertionError("independent_rumor_validator_accepted_invalid")


def run(daemon_binary, crypto_binary):
    canonical_vector_checks()
    with tempfile.TemporaryDirectory(prefix="signet-daemon-wire-") as temporary:
        directory = Path(temporary)
        (directory / "home").mkdir(mode=0o700)
        env = {"PATH": os.defpath, "HOME": str(directory / "home")}
        daemon = None
        relay = None
        peer = None
        with (directory / "crypto.stderr").open("wb") as crypto_stderr, \
                (directory / "daemon.stderr").open("wb") as daemon_stderr:
            try:
                peer = CryptoPeer(crypto_binary, directory, env, crypto_stderr)
                backend = str(uuid.uuid4())
                snapshot = {"profile_hash": HASH, "generation": "0", "namespace_digest": HASH}
                responses = queue.Queue()
                subscriptions = []
                initial_sent = False

                def request(method, **params):
                    nonce = str(uuid.uuid4())
                    return {"jsonrpc": "2.0", "id": nonce, "method": "backend/" + method,
                            "params": {"protocol_version": 1, "authority_id": AUTHORITY,
                                       "backend_uuid": backend, "correlation_nonce": nonce, **params}}

                historical = request("enroll", command_id=str(uuid.uuid4()),
                                     expected_revision="0", **snapshot)
                history_event = peer.call({"op": "wrap", "actor": "operator",
                                          "request": historical,
                                          "rumor_at": int(time.time()) + 60})["event"]
                initial = request("status", epoch="1", session_id=str(uuid.uuid4()), op_id=None)

                def handle(server, message):
                    nonlocal initial_sent
                    if message[0] == "REQ":
                        assert any(peer.ready["bunker_pubkey"] in f.get("#p", [])
                                   for f in message[2:]), "unexpected_daemon_subscription"
                        subscriptions.append(message[1])
                        if not initial_sent:
                            initial_sent = True
                            # Use the allowed skew window for BOTH rumors so
                            # startup/second rollover cannot make wire ordering
                            # accidentally pass or fail via the timestamp guard.
                            initial_event = peer.call({"op": "wrap", "actor": "operator",
                                                       "request": initial,
                                                       "rumor_at": int(time.time()) + 60})["event"]
                            # Preserve exact wire order in one write. The future-
                            # dated history must stay unadmitted despite its time.
                            batch = (frame(["EVENT", message[1], history_event]) +
                                     frame(["EOSE", message[1]]) +
                                     frame(["EVENT", message[1], initial_event]))
                            with server.lock:
                                server.sock.sendall(batch)
                        else:
                            server.send(["EOSE", message[1]])
                    elif message[0] == "EVENT":
                        event = message[1]
                        assert event["kind"] == 1059, "unencrypted_authority_reply"
                        server.send(["OK", event["id"], True, ""])
                        responses.put(event)

                relay = Relay(handle)
                config = directory / "authority.ini"
                config.write_text(
                    "[server]\nhealth_port=0\nlog_level=info\n[store]\ndb_path=" +
                    str(directory / "authority.db") + "\n[nostr]\nrelays=" + relay.url +
                    "\nidentity=admission-wire-fixture\n[backend_admission]\nauthority_id=" +
                    AUTHORITY + "\noperator_pubkey=" + peer.ready["operator_pubkey"] +
                    "\n[audit]\nstdout=false\npath=" + str(directory / "audit.log") +
                    "\n[policy_defaults]\ndefault_decision=deny\n[bootstrap]\nport=0\n"
                    "[dbus]\nunix_enabled=false\ntcp_enabled=false\n[nip5l]\nenabled=false\n"
                    "[ssh_agent]\nenabled=false\n[passkeys]\nenabled=false\nvirtual_ctap=false\n")
                daemon_env = {**env, "SIGNET_DB_KEY_FILE": str(directory / "master.hex"),
                              "SIGNET_BUNKER_NSEC_FILE": str(directory / "bunker.hex"),
                              "SIGNET_REQUIRE_ENCRYPTED_DB":
                                  "true" if peer.ready["sqlcipher_available"] else "false"}
                daemon = subprocess.Popen([daemon_binary, "-c", str(config)], env=daemon_env,
                                          stdout=subprocess.DEVNULL, stderr=daemon_stderr)

                def receive(actor, rpc):
                    try:
                        event = responses.get(timeout=10)
                    except queue.Empty:
                        raise AssertionError("actual_daemon_authenticated_reply_missing") from None
                    opened = peer.call({"op": "open", "actor": actor, "event": event})
                    assert opened["authority_authenticated"], opened.get("error", "authority_not_authenticated")
                    canonical_unsigned_rumor(opened["rumor"], peer.ready["bunker_pubkey"],
                                             peer.ready[actor + "_pubkey"])
                    response = opened["response"]
                    assert response["jsonrpc"] == "2.0", "invalid_response_version"
                    assert response["id"] != historical["id"], "pre_eose_history_was_processed"
                    assert response["id"] == rpc["id"], "wrong_reply_correlation"
                    if "result" in response:
                        result = response["result"]
                        assert result["correlation_nonce"] == rpc["id"], "wrong_nonce"
                        assert result["authority_id"] == AUTHORITY, "wrong_authority"
                        assert result["backend_uuid"] == backend, "wrong_backend"
                        assert result["method"] == rpc["method"], "wrong_method"
                    return response

                refused = receive("operator", initial)
                assert refused["error"]["data"] == "unauthorized", "wrong_authenticated_refusal"

                def exchange(actor, method, **params):
                    rpc = request(method, **params)
                    event = peer.call({"op": "wrap", "actor": actor, "request": rpc})["event"]
                    assert subscriptions, "daemon_has_no_subscription"
                    relay.send(["EVENT", subscriptions[-1], event])
                    return receive(actor, rpc)

                enrolled = exchange("operator", "enroll", command_id=str(uuid.uuid4()),
                                    expected_revision="0", **snapshot)["result"]
                assert enrolled["phase"] == "ENROLLED", "history_changed_enrollment"
                activated = exchange("operator", "activate", command_id=str(uuid.uuid4()),
                                     expected_revision=enrolled["revision"],
                                     boot_pubkey=peer.ready["boot_pubkey"], **snapshot)["result"]
                assert activated["phase"] == "STARTING", "activation_failed"
                opened = exchange("boot", "open", **snapshot)["result"]
                binding = {"epoch": opened["epoch"], "session_id": opened["session_id"]}
                operation = {**binding, "op_id": str(uuid.uuid4()), "class": "STARTUP",
                             "argument_digest": HASH, **snapshot}
                begun = exchange("boot", "begin", **operation)["result"]
                assert begun["created"] is True, "first_begin_not_admitted"
                repeated = exchange("boot", "begin", **operation)["result"]
                assert repeated["created"] is False, "duplicate_begin_armed"
                finished = exchange("boot", "finish", **binding, op_id=operation["op_id"],
                                    argument_digest=HASH, outcome="STARTUP_CONFIRMED",
                                    proof_digest=HASH, generation="0", namespace_digest=HASH)["result"]
                assert finished["phase"] == "ACTIVE", "startup_not_confirmed"
                terminal = exchange("boot", "status", **binding,
                                    op_id=operation["op_id"])["result"]
                assert terminal["op"]["state"] == "TERMINAL", "terminal_record_missing"
                sealed = exchange("operator", "seal", command_id=str(uuid.uuid4()),
                                  expected_revision=terminal["revision"],
                                  predecessor_proof_digest=HASH)["result"]
                assert sealed["phase"] == "SEALED", "seal_failed"
                print("real daemon: independently canonical unsigned replies, authenticated refusal, enroll/activate/open, first/duplicate Begin, terminal Finish/status and Seal PASS")
            finally:
                if daemon is not None:
                    daemon.terminate()
                    try:
                        daemon.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        daemon.kill()
                        daemon.wait()
                        raise AssertionError("daemon_did_not_join")
                if relay is not None:
                    relay.close()
                if peer is not None:
                    peer.close()
                daemon_stderr.flush()
                for line in (directory / "daemon.stderr").read_text(errors="replace").splitlines():
                    match = re.search(r"management-(?:wire|live): ([a-z_]+)$", line)
                    if match:
                        print("daemon-stage:", match.group(1))


if __name__ == "__main__":
    assert len(sys.argv) == 3, "usage: test_backend_admission_daemon.py SIGNETD CRYPTO_FIXTURE"
    run(sys.argv[1], sys.argv[2])
