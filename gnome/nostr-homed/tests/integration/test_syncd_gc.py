#!/usr/bin/env python3
"""Real syncd GC + local NIP-42/Blossom fixture. No hosted/release claim."""
import base64
import hashlib
import json
import os
from pathlib import Path
import shutil
import socket
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[4]
sys.path.insert(0, str(ROOT / "tools/vaultctl/tests"))
from fixtures import Handler, Lab, pub, verify

BINARY, HELPER, STATUS = map(lambda s: str(Path(s).resolve()), sys.argv[1:4])

class GCHandler(Handler):
    def authorization(self, action, digest=None):
        value = self.headers.get("Authorization", "")
        assert value.startswith("Nostr "), value
        event = json.loads(base64.b64decode(value[6:]))
        verify(event)
        assert event["kind"] == 24242 and event["pubkey"] == pub(1)
        assert ["t", action] in event["tags"]
        assert any(t[0] == "expiration" and int(t[1]) > time.time() for t in event["tags"])
        if digest:
            assert ["x", digest] in event["tags"]
        return event

    def do_GET(self):
        if self.path.startswith("/list/"):
            try:
                self.authorization("list")
                assert self.path == "/list/" + pub(1)
                if self.server.bad_list:
                    return self.response(200, b'[{"sha256":"bad","size":1}]')
                with self.server.lock:
                    rows = [{"sha256": h, "size": len(b), "uploaded": 1}
                            for h, b in self.server.blobs.items()]
                    if self.server.mutate:
                        self.server.events.pop(self.server.mutate)
                        self.server.mutate = None
                return self.response(200, json.dumps(rows).encode())
            except Exception as exc:
                self.server.failures.append(repr(exc))
                raise
        return super().do_GET()

    def do_DELETE(self):
        try:
            digest = self.path[1:]
            assert len(digest) == 64
            self.authorization("delete", digest)
            with self.server.lock:
                self.server.deletes.append(digest)
                if self.server.reject_delete:
                    return self.response(503, b'fixture refuses delete')
                assert digest in self.server.blobs
                del self.server.blobs[digest]
            self.response(200, {})
        except Exception as exc:
            self.server.failures.append(repr(exc))
            raise

    def do_PUT(self):
        self.server.failures.append("GC tried to upload")
        return self.response(500, b'no uploads permitted')

    def websocket(self):
        key = self.headers["Sec-WebSocket-Key"]
        accept = base64.b64encode(hashlib.sha1((key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode()).digest()).decode()
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
        challenge, authed = "gc-fixture", False
        self.frame(["AUTH", challenge])
        try:
            while True:
                message = self.recv()
                if message is None:
                    continue
                if message[0] == "AUTH":
                    event = message[1]
                    verify(event)
                    assert event["kind"] == 22242
                    assert ["challenge", challenge] in event["tags"]
                    assert ["relay", self.server.ws] in event["tags"]
                    authed = True
                    self.server.auths += 1
                    self.frame(["OK", event["id"], True, ""])
                elif message[0] == "REQ":
                    self.server.reqs += 1
                    sub = message[1]
                    if not authed:
                        self.frame(["AUTH", challenge])
                        self.frame(["CLOSED", sub, "auth-required: authenticate first"])
                        continue
                    if self.server.closed:
                        self.frame(["CLOSED", sub, "blocked: fixture refusal"])
                        continue
                    assert len(message) == 3
                    f = message[2]
                    assert f["kinds"] == [30078] and f["authors"] and f["limit"] > 0
                    assert "#d" not in f or len(f["#d"]) == 1
                    with self.server.lock:
                        events = list(self.server.events.values())
                    latest = {}
                    for event in events:
                        d = next(t[1] for t in event["tags"] if t[0] == "d")
                        address = (event["pubkey"], d)
                        prior = latest.get(address)
                        if prior is None or (event["created_at"], event["id"]) > (prior["created_at"], prior["id"]):
                            latest[address] = event
                    sent = 0
                    for (author, d), event in latest.items():
                        if author not in f["authors"] or ("#d" in f and d not in f["#d"]):
                            continue
                        if sent == f["limit"]:
                            break
                        self.frame(["EVENT", sub, event])
                        sent += 1
                    if not self.server.omit_eose:
                        self.frame(["EOSE", sub])
                elif message[0] != "CLOSE":
                    raise AssertionError("GC must not publish " + message[0])
        except (EOFError, ConnectionResetError, BrokenPipeError, socket.timeout):
            pass
        except Exception as exc:
            self.server.failures.append(repr(exc))
            raise
        finally:
            self.close_connection = True


def snapshot(base):
    return {str(p.relative_to(base)): (hashlib.sha256(p.read_bytes()).hexdigest(), p.stat().st_mode)
            for p in base.rglob("*") if p.is_file()}


def scenario(name, modify=None, dry=False, fail=False, copies=1, v1=False, expire_archive=False):
    labs = []
    with tempfile.TemporaryDirectory(prefix="nostrc-gc-") as tmp:
        base = Path(tmp)
        subprocess.run([HELPER, str(base)], check=True, capture_output=True, timeout=15)
        expected = json.loads((base / "expected.json").read_text())
        archive = expected["lazy_archive_retained"]
        prior = json.loads((base / "lazy-archive-prior.json").read_text())
        current = json.loads((base / "state/nostr-homed/snapshot.json").read_text())
        assert prior["files"]["Archive"]["lazy"] is True
        assert archive in prior["files"]["Archive/cold.txt"]["chunk_addrs_hex"]
        assert "Archive/cold.txt" not in current["files"]
        pins_path = base / "state/nostr-homed/pinned.json"
        pins = json.loads(pins_path.read_text())
        assert archive in next(g["hashes"] for g in pins["generations"] if g["gen"] == 2)
        assert all(archive not in g["hashes"] for g in pins["generations"] if g["gen"] != 2)
        if expire_archive:
            pins["generations"] = [g for g in pins["generations"] if g["gen"] != 2]
            pins_path.write_text(json.dumps(pins))
        events = json.loads((base / "events.json").read_text())
        if v1:
            events = [e for e in events if any(t[:2] == ["d", "nostr-homed.home.v1:personal"] for t in e["tags"])]
            shutil.rmtree(base / "state/nostr-homed/vaults")
            (base / "state/nostr-homed/vault-pinned.json").unlink()
            (base / "syncd.json").write_text('{"vaults":[]}')
        initial = {p.name: p.read_bytes() for p in (base / "blobs").iterdir()}
        env = {k: os.environ[k] for k in ("PATH", "LD_LIBRARY_PATH", "ASAN_OPTIONS", "UBSAN_OPTIONS") if k in os.environ}
        # No inherited provider/session secrets, SSH agent or developer services.
        env.update(HOME=str(base / "home"), XDG_STATE_HOME=str(base / "state"),
                   XDG_CACHE_HOME=str(base / "cache"), XDG_RUNTIME_DIR=str(base / "runtime"),
                   NOSTR_HOMED_SYNCD_HOME=str(base / "home"),
                   NOSTR_HOMED_SYNCD_STATE_DIR=str(base / "state/nostr-homed"),
                   NOSTR_HOMED_SYNCD_CONFIG=str(base / "syncd.json"),
                   NOSTR_HOMED_SYNCD_TEST_MODE="1", NOSTR_HOMED_SYNCD_NSEC_HEX="00" * 31 + "01",
                   NOSTR_HOMED_SYNCD_SEED_FILE=str(base / "seed"))
        (base / "seed").write_text("09" + "00" * 31)
        (base / "runtime").mkdir(mode=0o700)
        status_path = base / "state/nostr-homed/porthome-status.json"
        status_path.write_text(json.dumps({"schema": 1, "syncd": {"state": "idle", "sibling_marker": True}, "fuse": {"mounted": True}}))
        status_path.chmod(0o600)
        try:
            for _ in range(copies):
                lab = Lab()
                labs.append(lab)
                lab.RequestHandlerClass = GCHandler
                lab.blobs = dict(initial)
                lab.events = {e["id"]: e for e in events}
                lab.deletes, lab.bad_list, lab.reject_delete, lab.mutate = [], False, False, None
            env["NOSTR_HOMED_SYNCD_RELAYS"] = ",".join(l.ws for l in labs)
            env["NOSTR_HOMED_SYNCD_BLOSSOM"] = ",".join(l.url for l in labs)
            if modify:
                modify(base, expected, labs, env)
            before = snapshot(base)
            remote_before = [dict(l.blobs) for l in labs]
            result = subprocess.run([BINARY, "--gc"] + (["--dry-run"] if dry else []), env=env,
                                    capture_output=True, text=True, timeout=45)
            assert result.returncode == (8 if fail else 0), (name, result.returncode, result.stdout, result.stderr)
            report = json.loads(result.stdout.strip())
            assert report["dry_run"] == dry
            garbage = {expected["garbage"][0]} if v1 else set(expected["garbage"])
            if expire_archive:
                garbage.add(archive)
            for lab, baseline in zip(labs, remote_before):
                assert not lab.failures, (name, lab.failures)
                assert not lab.puts and not lab.attempts
                assert not (set(lab.deletes) - garbage), (name, "referenced/unknown blob deleted", lab.deletes)
                for digest, data in baseline.items():
                    if digest not in garbage:
                        assert lab.blobs.get(digest) == data, (name, digest)
            if dry:
                assert before == snapshot(base), (name, "dry run changed local files")
                assert all(not l.deletes and l.blobs == baseline for l, baseline in zip(labs, remote_before))
                if not fail:
                    assert {r["sha256"] for r in report["deletions"] if r["result"] == "would-delete"} == garbage
            elif fail == "partial":
                assert report["result"] == "partial", (name, report)
                journal = base / "state/nostr-homed/blob-gc-deletions.jsonl"
                receipts = [json.loads(line) for line in journal.read_text().splitlines()]
                assert any(r["server"] == labs[-1].url and r["result"] == "delete-failed" for r in receipts)
                state = base / "state/nostr-homed"
                for digest in garbage:
                    subprocess.run([HELPER, "--receipt-invalidated", str(state), digest], check=True)
                # Deterministic recovery in the next weekly class, after changing
                # the fixture's fault. This is not retry-to-green acceptance.
                for lab in labs:
                    lab.reject_delete = False
                subprocess.run([HELPER, "--expire-budget", str(state)], check=True)
                resumed = subprocess.run([BINARY, "--gc"], env=env, capture_output=True, text=True, timeout=45)
                assert resumed.returncode == 0, resumed.stdout + resumed.stderr
                assert json.loads(resumed.stdout)["result"] == "complete"
                assert all(not (set(l.blobs) & garbage) for l in labs)
                for lab, baseline in zip(labs, remote_before):
                    assert all(lab.blobs.get(h) == data for h, data in baseline.items() if h not in garbage)
            elif fail:
                assert report["result"] == "refused", (name, report)
                assert all(not l.deletes for l in labs), (name, "incomplete walk issued DELETE")
            else:
                assert report["result"] == "complete", (name, report)
                for lab in labs:
                    assert set(lab.deletes) == garbage, (name, lab.deletes, garbage)
                journal = base / "state/nostr-homed/blob-gc-deletions.jsonl"
                receipts = [json.loads(line) for line in journal.read_text().splitlines()]
                for lab in labs:
                    assert {r["sha256"] for r in receipts if r["server"] == lab.url and r["result"] == "deleted"} == garbage
                    assert {r["sha256"] for r in receipts if r["server"] == lab.url and r["result"] == "attempt"} == garbage
                status = subprocess.run([STATUS, "--field", "gc.result"], env=env, capture_output=True, text=True, check=True)
                assert status.stdout.strip() == "complete"
                status = subprocess.run([STATUS, "--json"], env=env, capture_output=True, text=True, check=True)
                combined = json.loads(status.stdout)
                assert combined["gc"] == report
                assert combined["syncd"]["sibling_marker"] and combined["fuse"]["mounted"]
                for delta, due in ((0, "0"), (604800, "1")):
                    state = base / "state/nostr-homed"
                    check = subprocess.run([HELPER, "--due", str(state), str(report["at"] + delta)],
                                           capture_output=True, text=True, check=True)
                    assert check.stdout.strip() == due
                assert all(l.auths and l.reqs for l in labs)
                if not v1:
                    for digest in garbage:
                        subprocess.run([HELPER, "--receipt-invalidated", str(state), digest], check=True)
            print("PASS", name)
        finally:
            for lab in labs:
                lab.finish()


def corrupt_ring(base, expected, labs, env):
    path = base / "state/nostr-homed/pinned.json"
    j = json.loads(path.read_text())
    j["generations"][0]["hashes"].append("invalid")
    path.write_text(json.dumps(j))


def incomplete_snapshot(base, expected, labs, env):
    path = base / "state/nostr-homed/snapshot.json"
    doc = json.loads(path.read_text())
    del doc["files"]["file"]["chunk_addrs_hex"]
    path.write_text(json.dumps(doc))


def unreachable(base, expected, labs, env):
    sock = socket.socket()
    sock.bind(("127.0.0.1", 0))
    port = sock.getsockname()[1]
    sock.close()
    env["NOSTR_HOMED_SYNCD_RELAYS"] += f",ws://127.0.0.1:{port}/relay"

scenario("v1 + every vault writer + older epoch + conflict + pending/reseal intent dry-run", dry=True)
scenario("retained lazy Archive is marked by real GC; expiry makes it a candidate", dry=True, expire_archive=True)
scenario("real authenticated BUD-02 DELETE on two servers, per-server receipts and status", copies=2)
scenario("v1-only daemon path, retained generations and conflict files", v1=True)
scenario("per-server failure receipts, PUT invalidation and next-window recovery", lambda b, e, labs, env: setattr(labs[-1], "reject_delete", True), copies=2, fail="partial")
scenario("missing manifest refuses all deletion", lambda b, e, labs, env: labs[0].blobs.pop(e["missing_manifest"]), fail=True)
scenario("CLOSED relay refuses all deletion", lambda b, e, labs, env: setattr(labs[0], "closed", True), fail=True)
scenario("unreachable additional relay is not replaced by an EOSE quorum", unreachable, fail=True)
scenario("changed heads after mark refuse all deletion", lambda b, e, labs, env: setattr(labs[0], "mutate", next(k for k, v in labs[0].events.items() if any(t[0] == "d" and t[1].startswith("porthome/v2/") for t in v["tags"]))), fail=True)
scenario("malformed inventory refuses all deletion", lambda b, e, labs, env: setattr(labs[0], "bad_list", True), fail=True)
scenario("malformed retained hash is not silently skipped", corrupt_ring, fail=True, dry=True)
scenario("missing file descriptors are not defaulted to an empty mark set", incomplete_snapshot, fail=True, dry=True)
scenario("missing retention evidence refuses without recreating it", lambda b, e, labs, env: (b / "state/nostr-homed/pinned.json").unlink(), fail=True, dry=True)
scenario("an unconfigured vault is not assumed unreferenced", lambda b, e, labs, env: (b / "state/nostr-homed/vaults" / ("ff" * 32)).mkdir(mode=0o700), fail=True, dry=True)
scenario("missing EOSE is failure, never a quiet-window completion", lambda b, e, labs, env: setattr(labs[0], "omit_eose", True), fail=True, dry=True)
scenario("retention budget refusal is visible and dry-run side-effect-free", lambda b, e, labs, env: env.update(NOSTR_HOMED_SYNCD_RETENTION_BYTES="0"), fail=True, dry=True)
print("PASS focused GC daemon/Blossom integration; all fixture servers and temporary files torn down")
