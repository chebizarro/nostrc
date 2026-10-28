#!/usr/bin/env python3
"""Local Linux CI-image integration: real sysfs, CLI dispatch and PAM/authd refusal.

Run as root in a disposable container with pamtester installed. No relay contact
is needed: strict policy must refuse before a fetch. The CLI helper below is an
explicit dispatch sentinel, not materialization proof.
"""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import time


def run(*args, env=None, input=None):
    return subprocess.run(list(map(str, args)), env=env, input=input, text=True,
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=40)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--build", type=Path, required=True)
    args = ap.parse_args()
    b = args.build.resolve() / "gnome/nostr-homed"
    assert os.geteuid() == 0 and os.environ.get("NH_DISPOSABLE_LOGIN_LAB") == "1"
    assert shutil.which("pamtester"), "pamtester required"
    with tempfile.TemporaryDirectory(prefix="nh-at-rest-") as tmp:
        root = Path(tmp)
        probe = run(b / "test_porthome_at_rest", "--probe", root)
        assert probe.returncode == 0
        state = probe.stdout.strip()
        assert state in ("unknown", "unencrypted"), "test requires non-crypt/unknown CI storage"
        account = root / "account.json"
        # Public fixture values, never operator credentials.
        account.write_text(json.dumps({"schema": 1, "account_pubkey_hex": "11" * 32,
            "wrap_seed_hex": "22" * 32, "root_id_hex": "33" * 32,
            "home_key_hex": "44" * 32, "d_tag": "nostr-homed.home.v1:personal",
            "home_relays": ["wss://relay.example"], "blossom_servers": ["https://blob.example"],
            "generation": 0}))
        marker = root / "helper-ran"
        helper = root / "helper"
        helper.write_text(f"#!/bin/sh\ncat >/dev/null\ntouch '{marker}'\nexit 0\n")
        helper.chmod(0o700)
        dest = root / "strict-home"
        cmd = [b / "nostr-homed-provision", "pull", "--account-file", account,
               "--dest", dest, "--helper", helper]
        strict = run(*cmd, "--require-encrypted")
        assert strict.returncode == 64, strict.stdout
        assert "WARNING:" in strict.stdout and "refusing provisioning" in strict.stdout
        assert not marker.exists() and not dest.exists(), "refusal must precede mkdir/helper"
        warning = run(*cmd)
        assert warning.returncode == 0 and marker.exists(), warning.stdout
        assert "WARNING:" in warning.stdout
        env = dict(os.environ, HOME=str(dest), XDG_STATE_HOME=str(dest / ".local/state"))
        status = run(b / "nostr-home-status", "--field", "at_rest.state", env=env)
        assert status.returncode == 0 and status.stdout.strip() == state, status.stdout
        print(f"PASS: live sysfs={state}; CLI warning dispatch/status and strict no-side-effect refusal")

        name = f"n_atrest_{os.getpid()}"
        home = Path("/home") / name
        assert not home.exists()
        db = root / "authority"
        db.mkdir(mode=0o700)
        seed = run(b / "nh-seed-authority", db, name, "fixture-pass", "22" * 32, "22" * 32,
                   "--no-fetch-profile", "--portable-home")
        assert seed.returncode == 0, seed.stdout
        assert not home.exists()
        conf = root / "auth.conf"
        conf.write_text("porthome_require_encrypted=on\n")
        sock = root / "auth.sock"
        service = f"nh-at-rest-{os.getpid()}"
        pam = Path("/etc/pam.d") / service
        assert not pam.exists()
        pam.write_text("\n".join(f"{kind} required {b}/pam_nostr.so socket={sock}"
                                  for kind in ("auth", "account", "session")) + "\n")
        log = root / "authd.log"
        try:
            with log.open("w+") as output:
                daemon = subprocess.Popen([str(b / "nostr-authd"), str(sock), str(db)],
                    env=dict(os.environ, NH_AUTH_CONF=str(conf), NOSTR_HOMED_SYNCD_NOTIFY="0"),
                    stdout=output, stderr=subprocess.STDOUT)
                try:
                    deadline = time.monotonic() + 10
                    while not sock.is_socket():
                        assert daemon.poll() is None, log.read_text()
                        assert time.monotonic() < deadline, "authd socket readiness timeout"
                        time.sleep(0.02)
                    login = run("pamtester", "-v", service, name,
                                "authenticate", "acct_mgmt", "open_session", input="fixture-pass\n")
                    assert "successfully authenticated" in login.stdout, login.stdout
                    assert login.returncode != 0 and "Session not opened" in login.stdout, login.stdout
                    assert "WARNING:" in login.stdout and "encryption" in login.stdout, login.stdout
                    assert not home.exists(), "strict first login installed a home"
                    import sqlite3
                    with sqlite3.connect(db / "authority.db") as con:
                        assert con.execute("SELECT portable_state FROM operations").fetchone()[0] == "enrolled-unprovisioned"
                    print("PASS: configured authd + authenticated PAM first login warns and refuses; no home installed")
                    print(login.stdout)
                finally:
                    daemon.terminate()
                    daemon.wait(timeout=10)
        finally:
            pam.unlink(missing_ok=True)
            if home.exists():
                shutil.rmtree(home)


if __name__ == "__main__":
    main()
