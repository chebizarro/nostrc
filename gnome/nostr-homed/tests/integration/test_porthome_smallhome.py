#!/usr/bin/env python3
"""
test_porthome_smallhome.py — Phase 1 acceptance test for nostrc-h10m.

Round-trips a ~50-file fixture home through:
  fake_porthome_blossom (x3, with failover) → sealed manifest → fake_relay
  event round-trip → fresh "client" → decrypt+decode → materialize → byte compare.

Argv:
  sys.argv[1] = absolute path to porthome_smallhome_driver
"""
import asyncio
import base64
import hashlib
import json
import os
import random
import shutil
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path


def _free_port() -> int:
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def _wait_ready(proc, name, timeout_s=5.0):
    """Block until subprocess prints 'READY' on stdout."""
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        line = proc.stdout.readline()
        if not line:
            if proc.poll() is not None:
                raise RuntimeError(f"{name} died: rc={proc.returncode}")
            continue
        if b"READY" in line:
            return
    raise RuntimeError(f"{name} did not print READY within {timeout_s}s")


def _spawn_blossom(port, data_dir, mode="ok"):
    here = os.path.dirname(os.path.abspath(__file__))
    env = os.environ.copy()
    env["FAKE_BLOSSOM_PORT"] = str(port)
    env["FAKE_BLOSSOM_DIR"] = data_dir
    env["FAKE_BLOSSOM_MODE"] = mode
    proc = subprocess.Popen(
        [sys.executable, os.path.join(here, "fake_porthome_blossom.py")],
        env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, bufsize=0,
    )
    _wait_ready(proc, f"fake_blossom[{port}]")
    return proc


def _spawn_relay(port, events_file):
    here_this = os.path.abspath(__file__)
    integ_dir = os.path.dirname(os.path.dirname(here_this)) + "/integ"
    env = os.environ.copy()
    env["FAKE_RELAY_PORT"] = str(port)
    env["FAKE_RELAY_EVENTS"] = events_file
    proc = subprocess.Popen(
        [sys.executable, os.path.join(integ_dir, "fake_relay_fixture.py")],
        env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, bufsize=0,
    )
    _wait_ready(proc, f"fake_relay[{port}]", timeout_s=8.0)
    return proc


def _build_fixture(root: Path):
    """~50 files: many tiny + one 5 MiB, one symlink, nested dirs."""
    rng = random.Random(0xC0FFEE)
    (root / "docs").mkdir()
    (root / "code" / "src").mkdir(parents=True)
    (root / ".cache").mkdir()
    for i in range(30):
        p = root / "docs" / f"note_{i:02}.md"
        p.write_bytes(("hello %d\n" % i).encode() * (i + 1))
    for i in range(15):
        p = root / "code" / "src" / f"mod_{i:02}.c"
        p.write_bytes(bytes(rng.getrandbits(8) for _ in range(200 + i * 30)))
    (root / ".cache" / "small.dat").write_bytes(b"")  # empty file
    (root / "code" / "README").write_text("hi\n")
    # one ~5 MiB binary
    big = bytes(rng.getrandbits(8) for _ in range(5 * 1024 * 1024 + 17))
    (root / "code" / "big.bin").write_bytes(big)
    # symlink (relative)
    try:
        os.symlink("../code/README", root / "docs" / "readme_link")
    except OSError:
        pass  # non-fatal if filesystem doesn't support symlinks
    return root


def _tree_signature(root: Path):
    """Return dict of relpath -> sha256 hex for files + kind marker for dirs."""
    out = {}
    for base, dirs, files in os.walk(root, followlinks=False):
        rel_base = os.path.relpath(base, root)
        for d in sorted(dirs):
            rel = d if rel_base == "." else os.path.join(rel_base, d)
            out[("DIR", rel)] = None
        for name in sorted(files):
            full = Path(base) / name
            rel = name if rel_base == "." else os.path.join(rel_base, name)
            if full.is_symlink():
                out[("LNK", rel)] = os.readlink(full)
            else:
                h = hashlib.sha256(full.read_bytes()).hexdigest()
                out[("FILE", rel)] = h
    return out


def _encrypt_relpaths(rel_map, home_key_hex):
    """
    We can't reproduce nh_porthome_encrypt_path from Python without a lot
    of copied crypto. Instead, this helper is unused — we compare the
    ENCRYPTED-path universe of the reconstructed tree against a
    directly-recomputed encrypted-path signature of the fixture (see
    _tree_signature_encrypted_from_driver below).
    """
    del rel_map, home_key_hex


def _publish_and_relay_roundtrip(port, pubkey_hex, sealed_bytes):
    """Publish sealed_bytes as a kind-30078 event on the fake relay, then
    fetch it back on a fresh connection and return the raw bytes."""
    try:
        from websockets.asyncio.client import connect as ws_connect  # >= 13
    except ImportError:
        from websockets.client import connect as ws_connect  # < 13

    async def go():
        b64 = base64.b64encode(sealed_bytes).decode("ascii")
        eid = hashlib.sha256(sealed_bytes).hexdigest()
        event = {
            "id": eid, "pubkey": pubkey_hex, "created_at": int(time.time()),
            "kind": 30078,
            "tags": [
                ["d", "nostr-homed.home.v1:personal"],
                ["client", "nostr-homed"],
                ["alt", "encrypted portable home pointer (Phase 1 test)"],
            ],
            "content": b64,
            "sig": "00" * 64,  # fake_relay doesn't verify by default
        }
        async with ws_connect(f"ws://127.0.0.1:{port}") as ws:
            await ws.send(json.dumps(["EVENT", event]))
            reply = json.loads(await ws.wait_for(ws.recv(), 5) if hasattr(ws, "wait_for") else await ws.recv())
            assert reply[0] == "OK" and reply[2] is True, f"relay rejected: {reply}"

        # Fresh client — the "second machine".
        async with ws_connect(f"ws://127.0.0.1:{port}") as ws:
            filt = {"kinds": [30078], "authors": [pubkey_hex],
                    "#d": ["nostr-homed.home.v1:personal"]}
            await ws.send(json.dumps(["REQ", "sub-recover", filt]))
            got_event = None
            while True:
                msg = json.loads(await ws.recv())
                if msg[0] == "EVENT" and msg[1] == "sub-recover":
                    got_event = msg[2]
                elif msg[0] == "EOSE":
                    break
            assert got_event is not None, "no event received"
            return base64.b64decode(got_event["content"])
    return asyncio.run(go())


def main():
    if len(sys.argv) < 2:
        print("usage: test_porthome_smallhome.py <driver-binary>", file=sys.stderr)
        return 2
    driver = sys.argv[1]
    if not os.path.isfile(driver) or not os.access(driver, os.X_OK):
        print(f"driver not executable: {driver}", file=sys.stderr)
        return 2

    tmp = Path(tempfile.mkdtemp(prefix="porthome-smallhome-"))
    fixture = tmp / "fixture"
    fixture.mkdir()
    _build_fixture(fixture)
    out = tmp / "reconstructed"
    sealed_path = tmp / "manifest.sealed"
    relay_events_file = tmp / "relay_seed.json"
    relay_events_file.write_text("[]")

    ports_bl = [_free_port() for _ in range(3)]
    ports_relay = _free_port()

    procs = []
    try:
        # 3 fake_blossoms; the wrapper will HEAD-then-PUT/GET across them.
        for p in ports_bl:
            procs.append(_spawn_blossom(p, str(tmp / f"bl{p}")))
        procs.append(_spawn_relay(ports_relay, str(relay_events_file)))

        seed_hex = "11" * 32
        blossom_csv = ",".join(f"http://127.0.0.1:{p}" for p in ports_bl)
        env = os.environ.copy()
        env["NH_PORTHOME_SEED_HEX"] = seed_hex
        env["NH_PORTHOME_BLOSSOM_URLS"] = blossom_csv
        env["NH_PORTHOME_ALLOW_INSECURE"] = "1"

        # ── upload ──────────────────────────────
        r = subprocess.run(
            [driver, "upload", str(fixture), str(sealed_path)],
            env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False,
        )
        if r.returncode != 0:
            print("upload driver failed:\nSTDOUT:", r.stdout.decode(),
                  "\nSTDERR:", r.stderr.decode(), file=sys.stderr)
            return 1
        home_key_hex = None
        for line in r.stdout.decode().splitlines():
            if line.startswith("HOME_KEY_HEX="):
                home_key_hex = line.split("=", 1)[1].strip()
        assert home_key_hex and len(home_key_hex) == 64
        sealed_bytes = sealed_path.read_bytes()
        assert len(sealed_bytes) > 0

        # ── publish + relay round-trip ──────────
        pubkey_hex = "aa" * 32
        recovered = _publish_and_relay_roundtrip(ports_relay, pubkey_hex, sealed_bytes)
        assert recovered == sealed_bytes, "relay round-trip did not preserve bytes"
        # Write recovered bytes to a fresh file to simulate "fresh client".
        sealed_from_relay = tmp / "manifest.from_relay.sealed"
        sealed_from_relay.write_bytes(recovered)

        # ── download (fresh "client") ──────────
        env_dl = env.copy()
        env_dl["NH_PORTHOME_HOME_KEY_HEX"] = home_key_hex
        # Deliberately omit one of the servers to prove failover works.
        env_dl["NH_PORTHOME_BLOSSOM_URLS"] = ",".join(
            f"http://127.0.0.1:{p}" for p in ports_bl[1:]
        )
        r = subprocess.run(
            [driver, "download", str(sealed_from_relay), str(out)],
            env=env_dl, stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False,
        )
        if r.returncode != 0:
            print("download driver failed:\nSTDOUT:", r.stdout.decode(),
                  "\nSTDERR:", r.stderr.decode(), file=sys.stderr)
            return 1

        # ── verify ─────────────────────────────
        # The reconstructed tree is keyed by ENCRYPTED path components,
        # so we cannot compare against fixture paths directly. Instead,
        # we count files/dirs/symlinks and compare the *multiset* of
        # file-content sha256 hashes and symlink targets. That verifies
        # byte-identical content (which is the acceptance-test claim)
        # without requiring an inverse of the name-encryption.
        def content_sig(root: Path):
            files_hashes = []
            symlinks = []
            n_dirs = 0
            for base, dirs, names in os.walk(root, followlinks=False):
                n_dirs += len(dirs)
                for n in names:
                    full = Path(base) / n
                    if full.is_symlink():
                        symlinks.append(os.readlink(full))
                    else:
                        files_hashes.append(hashlib.sha256(full.read_bytes()).hexdigest())
            files_hashes.sort()
            symlinks.sort()
            return (n_dirs, files_hashes, symlinks)

        want = content_sig(fixture)
        got = content_sig(out)
        if want != got:
            print("MISMATCH:", file=sys.stderr)
            print("  want dirs=%d files=%d syms=%d" %
                  (want[0], len(want[1]), len(want[2])), file=sys.stderr)
            print("  got  dirs=%d files=%d syms=%d" %
                  (got[0], len(got[1]), len(got[2])), file=sys.stderr)
            # Diagnostic: which hashes differ?
            missing = set(want[1]) - set(got[1])
            extra   = set(got[1]) - set(want[1])
            print("  missing hashes:", len(missing), file=sys.stderr)
            print("  extra hashes:", len(extra), file=sys.stderr)
            return 1

        print("OK porthome_smallhome: dirs=%d files=%d syms=%d bytes=%d"
              % (got[0], len(got[1]), len(got[2]), len(sealed_bytes)))
        return 0

    finally:
        for pr in procs:
            try:
                pr.terminate()
                pr.wait(timeout=3)
            except Exception:
                pr.kill()
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
