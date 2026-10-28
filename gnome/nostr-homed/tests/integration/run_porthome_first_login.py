#!/usr/bin/env python3
"""Real PAM first-login proof. Only run as root in a disposable systemd Linux lab.

Requires built authd/PAM/fetch/publisher/seeder, pamtester, python websockets,
shipped sysusers, and an already-running packaging/lab-blossom service.
No authentication or materialization mocks. See docs/INSTALLED_LOGIN.md.
"""
import argparse
import hashlib
import hmac
import os
from pathlib import Path
import selectors
import shutil
import sqlite3
import subprocess
import tempfile
import time


def run(*args, input=None, ok=True, timeout=90, env=None):
    p = subprocess.run([str(a) for a in args], input=input, text=True,
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                       timeout=timeout, env=env)
    if ok and p.returncode:
        raise AssertionError(f"{args[0]} exited {p.returncode}:\n{p.stdout}")
    return p


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--build", type=Path, required=True, help="root CMake build dir")
    ap.add_argument("--blossom", required=True, help="reachable isolated lab URL")
    ap.add_argument("--relay-port", type=int, default=17777)
    args = ap.parse_args()
    if os.geteuid() != 0 or os.environ.get("NH_DISPOSABLE_LOGIN_LAB") != "1":
        ap.error("requires root and NH_DISPOSABLE_LOGIN_LAB=1 in a disposable lab")
    assert Path("/run/systemd/system").is_dir(), "systemd must be running"
    root = Path(__file__).resolve().parents[2]
    build = args.build.resolve() / "gnome/nostr-homed"
    module = next(build.rglob("pam_nostr.so"))
    token = f"h10m-{os.getpid()}"
    unit = Path(f"/etc/systemd/system/{token}.service")
    pam = Path(f"/etc/pam.d/{token}")
    sock = f"/run/nostr-auth/{token}.sock"
    base = Path(tempfile.mkdtemp(prefix=token + "-", dir="/var/lib/nostr-auth"))
    fixture = base / "fixture"
    fixture.mkdir()
    contents = b"first login portable home\n"
    (fixture / "hello.txt").write_bytes(contents)
    relay_url = f"ws://127.0.0.1:{args.relay_port}"
    secret = "22" * 32  # Public test key, never an operator credential.
    seed = hmac.new(hmac.new(b"porthome/v1/wrap", bytes.fromhex(secret),
                            hashlib.sha256).digest(), b"\x01", hashlib.sha256).hexdigest()
    homes = []
    relay = None
    stage = Path("/home/.nostr-00000000-0000-4000-8000-000000000001")
    assert not stage.exists(), f"refuse to touch pre-existing stage {stage}"
    assert not unit.exists() and not pam.exists()

    def state(db):
        with sqlite3.connect(db / "authority.db") as con:
            return con.execute("SELECT portable_state,phase,outcome FROM operations").fetchone()

    def seed_account(label, portable=True, vault=secret):
        name = f"n_h10m_{os.getpid()}_{label}"
        home = Path("/home") / name
        assert not home.exists()
        homes.append(home)
        db = base / label
        db.mkdir(mode=0o700)
        cmd = [build / "nh-seed-authority", db, name, "fixture-pass", secret, vault,
               "--no-fetch-profile"]
        if portable:
            cmd.append("--portable-home")
        run(*cmd)
        if portable:
            assert not home.exists()
            assert state(db)[0] == "enrolled-unprovisioned"
        return name, home, db

    def start(db):
        run("systemctl", "stop", unit.name, ok=False)
        text = (build / "nostr-authd.service").read_text()
        text = "\n".join(f"ExecStart={build}/nostr-authd {sock} {db}" if line.startswith("ExecStart=")
                         else line for line in text.splitlines()) + "\n"
        text = text.replace("[Service]\n", "[Service]\n" +
            f"Environment=NH_PORTHOME_FETCH_HELPER={build}/nostr-home-fetch\n" +
            f"Environment=NH_PORTHOME_FETCH_RELAYS={relay_url}\n" +
            f"Environment=NH_PORTHOME_FETCH_BLOSSOM_SERVERS={args.blossom}\n" +
            "Environment=NH_PORTHOME_ALLOW_INSECURE=1\n")
        unit.write_text(text)
        run("systemctl", "daemon-reload")
        run("systemctl", "start", unit.name)
        # Wait for the actual socket, not a fixed startup delay. This is local
        # process readiness; relay catch-up remains EVENT/EOSE-driven.
        deadline = time.monotonic() + 10
        while not Path(sock).is_socket():
            assert time.monotonic() < deadline, "authd socket did not appear"
            time.sleep(0.02)
        props = run("systemctl", "show", unit.name, "-p", "ProtectHome",
                    "-p", "ProtectSystem", "-p", "BindPaths").stdout
        assert "ProtectHome=tmpfs" in props and "ProtectSystem=strict" in props
        assert "/home" in props

    def login(name, expected=True, actions=("authenticate", "acct_mgmt", "open_session"), password="fixture-pass"):
        p = run("pamtester", "-v", token, name, *actions,
                input=(password + "\n") * 3, ok=False)
        print(f"PAM {name} {' '.join(actions)} rc={p.returncode}\n{p.stdout}", flush=True)
        assert (p.returncode == 0) == expected
        return p.stdout

    try:
        # Exercise the shipped named service, replacing only lab-specific paths.
        text = (root / "packaging/pam/nostr-login.sample").read_text()
        pam.write_text(text.replace("pam_nostr.so", str(module)).replace(
            "/run/nostr-auth/auth.sock", sock))
        env = dict(os.environ, FAKE_RELAY_PORT=str(args.relay_port), FAKE_RELAY_VERIFY_SIGS="1")
        relay = subprocess.Popen(["python3", str(root / "tests/integ/fake_relay_fixture.py")],
                                 env=env, stdout=subprocess.PIPE, text=True)
        with selectors.DefaultSelector() as selector:
            selector.register(relay.stdout, selectors.EVENT_READ)
            assert selector.select(10), "fixture relay not ready"
            assert "READY" in relay.stdout.readline(), "fixture relay failed startup"
        published = run(build / "nostr-home-publisher", "publish", "--fixture-dir", fixture,
                        "--seed-hex", seed, "--nsec-hex", secret, "--schema", "2",
                        "--relay", relay_url, "--blossom", args.blossom,
                        env=dict(os.environ, NH_PORTHOME_ALLOW_INSECURE="1"))
        assert published.returncode == 0
        print("PASS: signed schema-v2 home published to fixture relay and real lab Blossom", flush=True)

        name, home, db = seed_account("ready")
        start(db)
        login("n_unknown_h10m", False, ("authenticate",))
        out = login("n_unknown_h10m", False, ("open_session",))
        assert "unknown_account" in out and "Session not opened" in out
        login(name, False, ("authenticate",), "wrong-pass")
        assert not home.exists() and state(db)[0] == "enrolled-unprovisioned"
        # Merely invoking session, without the verified auth connection, refuses.
        login(name, False, ("open_session",))
        assert not home.exists() and state(db)[0] == "enrolled-unprovisioned"
        out = login(name)
        assert "Loading your portable home" in out and "Your portable home is ready" in out
        assert "1 file" in out and "26 bytes" in out, "progress must describe plaintext copies"
        assert (home / "hello.txt").read_bytes() == contents
        with sqlite3.connect(db / "authority.db") as con:
            uid, gid = con.execute("SELECT uid,gid FROM accounts").fetchone()
        for path in [home, *home.rglob("*")]:
            st = path.lstat()
            assert (st.st_uid, st.st_gid) == (uid, gid), f"wrong owner: {path}"
        assert state(db)[0] == "installed"
        run("setpriv", f"--reuid={uid}", f"--regid={gid}", "--clear-groups", "sh", "-c",
            'cd "$1" && test -r hello.txt && test -w .local && printf local > retained.txt', "sh", home)
        inode = home.stat().st_ino
        drops = [Path(f"/run/nostr-auth/session/{uid}/{leaf}")
                 for leaf in ("home_seed", "home_seed.fuse")]
        for path in drops:
            assert path.read_text().strip() == seed
            assert path.stat().st_uid == uid and path.stat().st_mode & 0o777 == 0o600
            path.unlink()  # Simulate the read-once session consumers.
        run("systemctl", "stop", unit.name)
        login(name, False, ("open_session",))
        login("n_unknown_h10m", False, ("open_session",))
        login("root", True, ("open_session",))  # Positively local /etc/passwd account.
        relay.terminate()
        relay.wait(timeout=10)
        relay = None
        start(db)  # Discard in-memory jobs and cached seeds before second login.
        login(name)
        assert home.stat().st_ino == inode and (home / "retained.txt").read_text() == "local"
        assert state(db)[0] == "installed"
        for path in drops:
            assert path.read_text().strip() == seed, "second login must renew session seed handoff"
            path.unlink()
        print("PASS: second login reuses inode and renews session seeds with relay offline and broker restarted", flush=True)
        moved = home.with_name(home.name + "-moved")
        home.rename(moved)
        try:
            login(name, False)
            assert not home.exists(), "missing installed home must not silently fall back"
        finally:
            moved.rename(home)

        bad, badhome, baddb = seed_account("badkey", vault="33" * 32)
        start(baddb)
        login(bad, False, ("authenticate",))
        assert not badhome.exists() and state(baddb)[0] == "enrolled-unprovisioned"
        ordinary, ordinary_home, ordinary_db = seed_account("ordinary", portable=False)
        start(ordinary_db)
        login(ordinary)
        assert ordinary_home.is_dir() and state(ordinary_db)[0] == ""
        offline, offline_home, offline_db = seed_account("offline")
        start(offline_db)
        out = login(offline, False)
        assert "Loading your portable home" in out and "Session not opened" in out
        assert "Your portable home is ready" not in out and not offline_home.exists()
        assert state(offline_db)[0] == "failed"
        with sqlite3.connect(offline_db / "authority.db") as con:
            assert con.execute("SELECT status FROM accounts").fetchone()[0] == "repair_required"
        print("PASS: offline first login refuses, persists failed/repair_required, installs no home", flush=True)
        print("PASS: real PAM first-login suite (local Linux container; not graphical GDM proof)", flush=True)
    finally:
        journal = run("journalctl", "-u", unit.name, "--no-pager", "-n", "60", ok=False)
        print(journal.stdout, flush=True)
        run("systemctl", "stop", unit.name, ok=False)
        if relay:
            relay.terminate()
            relay.wait(timeout=10)
        unit.unlink(missing_ok=True)
        pam.unlink(missing_ok=True)
        run("systemctl", "daemon-reload")
        for home in homes:
            if home.exists():
                shutil.rmtree(home)
        if stage.exists():
            shutil.rmtree(stage)
        shutil.rmtree(base)


if __name__ == "__main__":
    main()
