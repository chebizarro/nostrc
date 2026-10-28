#!/usr/bin/env python3
"""Explicit privileged disposable Linux lab: real PAM + authd + logind + fscrypt.
Mount an encrypt-enabled ext4/f2fs filesystem at /home before invoking. Uses an
isolated signed fixture relay and real lab Blossom, never an operator signer.
"""
import argparse
import errno
import fcntl
import hashlib
import hmac
import os
from pathlib import Path
import selectors
import shutil
import sqlite3
import struct
import subprocess
import tempfile
import time


def run(*args, ok=True, **kw):
    p = subprocess.run([str(a) for a in args], text=True, stdout=subprocess.PIPE,
                       stderr=subprocess.STDOUT, timeout=90, **kw)
    if ok and p.returncode:
        raise AssertionError(f"{args} rc={p.returncode}\n{p.stdout}")
    return p


def until(condition, description, timeout=45):
    deadline = time.monotonic() + timeout
    while True:
        value = condition()
        if value:
            return value
        if time.monotonic() >= deadline:
            raise AssertionError(f"timeout: {description}")
        time.sleep(0.05)  # local assertion wait, not relay completion/production polling


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--build", type=Path, required=True)
    ap.add_argument("--blossom", required=True)
    ap.add_argument("--relay-port", type=int, default=17778)
    ap.add_argument("--unsupported", action="store_true", help="assert refusal on the mounted unsupported filesystem")
    ap.add_argument("--migrate-plaintext", action="store_true", help="start with a plaintext home and exercise migration/refusal/resume")
    args = ap.parse_args()
    assert os.geteuid() == 0 and os.environ.get("NH_DISPOSABLE_LOGIN_LAB") == "1"
    root = Path(__file__).resolve().parents[2]
    build = args.build.resolve() / "gnome/nostr-homed"
    token = f"h12-{os.getpid()}"
    name = f"n_h12_{os.getpid()}"
    home = Path("/home") / name
    base = Path(tempfile.mkdtemp(prefix=token + "-", dir="/var/lib/nostr-auth"))
    db = base / "identity"; db.mkdir(mode=0o700)
    fixture = base / "fixture"; fixture.mkdir()
    (fixture / "hello.txt").write_text("portable fscrypt fixture\n")
    conf = base / "auth.conf"
    conf.write_text("porthome_fscrypt=on\nporthome_fscrypt_allow_without_luks=on\n")
    pam = Path("/etc/pam.d") / token
    unit = Path("/etc/systemd/system") / (token + ".service")
    sock = f"/run/nostr-auth/{token}.sock"
    secret = "22" * 32
    seed = hmac.new(hmac.new(b"porthome/v1/wrap", bytes.fromhex(secret), hashlib.sha256).digest(), b"\x01", hashlib.sha256).digest()
    relay_url = f"ws://127.0.0.1:{args.relay_port}"
    relay = None; login = None; uid = None; busy = None
    user_created = False; group_created = False

    def record():
        with sqlite3.connect(db / "fscrypt/state.db") as c:
            return c.execute("SELECT state,key_id,generation FROM homes").fetchone()

    def key_status(identifier=None):
        data = bytearray(128)
        struct.pack_into("I", data, 0, 2)
        data[8:24] = record()[1] if identifier is None else identifier
        fd = os.open("/home", os.O_RDONLY | os.O_DIRECTORY)
        try:
            fcntl.ioctl(fd, 0xc080661a, data, True)
        finally:
            os.close(fd)
        return struct.unpack_from("I", data, 64)[0]

    def start():
        run("systemctl", "start", unit.name)
        until(lambda: Path(sock).is_socket(), "authd socket")

    def open_session():
        p = subprocess.Popen([str(build / "test_fscrypt_pam_session"), token, name],
                             stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                             stderr=base.joinpath("pam.log").open("a"), text=True)
        p.stdin.write("fixture-pass\n"); p.stdin.flush()
        with selectors.DefaultSelector() as sel:
            sel.register(p.stdout, selectors.EVENT_READ)
            assert sel.select(45), "PAM did not respond"
            line = p.stdout.readline()
        assert line.strip() == "READY", f"PAM failed {line!r}: {(base / 'pam.log').read_text()}"
        return p

    def refused_session():
        p = run(build / "test_fscrypt_pam_session", token, name, input="fixture-pass\n", ok=False)
        assert p.returncode != 0 and "READY" not in p.stdout, p.stdout

    def shown_state():
        return run("setpriv", f"--reuid={uid}", f"--regid={gid}", "--clear-groups",
                   "env", f"HOME={home}", build / "nostr-home-status", "--field", "fscrypt.state").stdout.strip()

    def close_session(p):
        p.stdin.write("close\n"); p.stdin.flush()
        assert p.wait(timeout=30) == 0
        p.stdin.close(); p.stdout.close()

    try:
        assert not home.exists() and not pam.exists() and not unit.exists()
        run(build / "nh-seed-authority", db, name, "fixture-pass", secret, secret,
            "--no-fetch-profile", "--portable-home")
        with sqlite3.connect(db / "authority.db") as c:
            account, uid, gid = c.execute("SELECT account_id,uid,gid FROM accounts").fetchone()
        # The test uses a local NSS fixture so pam_systemd can resolve the UID;
        # authorization still comes from the real broker's identity authority.
        run("groupadd", "-g", gid, name)
        group_created = True
        run("useradd", "-M", "-u", uid, "-g", gid, "-d", home, "-s", "/bin/sh", name)
        user_created = True
        pam.write_text((root / "packaging/pam/nostr-login.sample").read_text().replace(
            "pam_nostr.so", str(build / "pam_nostr.so")).replace("/run/nostr-auth/auth.sock", sock)
            + "session required pam_systemd.so\n")
        text = (build / "nostr-authd.service").read_text()
        text = "\n".join(f"ExecStart={build}/nostr-authd {sock} {db}" if s.startswith("ExecStart=") else s for s in text.splitlines()) + "\n"
        text = text.replace("[Service]\n", "[Service]\n" +
            f"Environment=NH_AUTH_CONF={conf}\nEnvironment=NH_PORTHOME_FETCH_HELPER={build}/nostr-home-fetch\n" +
            f"Environment=NH_PORTHOME_FETCH_RELAYS={relay_url}\nEnvironment=NH_PORTHOME_FETCH_BLOSSOM_SERVERS={args.blossom}\n" +
            "Environment=NH_PORTHOME_ALLOW_INSECURE=1\n")
        unit.write_text(text)
        run("systemctl", "daemon-reload")
        if args.unsupported:
            start(); refused_session()
            assert not home.exists()
            with sqlite3.connect(db / "fscrypt/state.db") as c:
                assert c.execute("SELECT count(*) FROM homes").fetchone()[0] == 0
            print("PASS: real PAM refuses unsupported filesystem without a plaintext home", flush=True)
            return
        # Loop backing is deliberately unknown: without the explicit override,
        # even a capable filesystem must be refused before creating a home.
        conf.write_text("porthome_fscrypt=on\nporthome_fscrypt_allow_without_luks=off\n")
        start(); refused_session()
        assert not home.exists()
        run("systemctl", "stop", unit.name)
        conf.write_text(f"porthome_fscrypt={'off' if args.migrate_plaintext else 'on'}\nporthome_fscrypt_allow_without_luks=on\n")
        relay = subprocess.Popen(["python3", str(root / "tests/integ/fake_relay_fixture.py")],
            env=dict(os.environ, FAKE_RELAY_PORT=str(args.relay_port), FAKE_RELAY_VERIFY_SIGS="1"),
            stdout=subprocess.PIPE, text=True)
        with selectors.DefaultSelector() as sel:
            sel.register(relay.stdout, selectors.EVENT_READ)
            assert sel.select(10) and "READY" in relay.stdout.readline()
        run(build / "nostr-home-publisher", "publish", "--fixture-dir", fixture,
            "--seed-hex", seed.hex(), "--nsec-hex", secret, "--schema", "2", "--relay", relay_url,
            "--blossom", args.blossom, env=dict(os.environ, NH_PORTHOME_ALLOW_INSECURE="1"))
        start()
        login = open_session()
        if args.migrate_plaintext:
            original_inode = home.stat().st_ino
            (home / "before-migration").write_text("unpushed plaintext edit")
            os.link(home / "before-migration", home / "hardlink")
            close_session(login); login = None
            until(lambda: run("systemctl", "is-active", f"user@{uid}.service", ok=False).stdout.strip() == "inactive",
                  "plaintext user's manager exits")
            run("systemctl", "stop", unit.name)
            def migrate_plain(ok):
                readfd, writefd = os.pipe(); os.write(writefd, seed); os.close(writefd)
                try:
                    p = run(build / "nostr-authd", "--fscrypt-migrate", db, account, readfd,
                            "--allow-without-luks", pass_fds=(readfd,), ok=False)
                    assert (p.returncode == 0) == ok, p.stdout
                finally:
                    os.close(readfd)
            with (home / "before-migration").open("rb"):
                migrate_plain(False)  # Even a root FD outside the user manager blocks copying.
                with sqlite3.connect(db / "fscrypt/state.db") as c:
                    assert c.execute("SELECT count(*) FROM maintenance").fetchone()[0] == 0
            residual = subprocess.Popen(["python3", "-c",
                f"import os,sys; os.setgid({gid}); os.setuid({uid}); print('READY',flush=True); sys.stdin.read()"],
                stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
            try:
                assert residual.stdout.readline().strip() == "READY"
                migrate_plain(False)  # A process outside logind must also be gone.
            finally:
                residual.stdin.close(); residual.wait(timeout=10)
            migrate_plain(False)  # Never silently split hardlink semantics.
            assert home.stat().st_ino == original_inode
            assert (home / "before-migration").read_text() == "unpushed plaintext edit"
            (home / "hardlink").unlink()  # Explicit operator resolution, then resume.
            migrate_plain(True)
            assert record()[2] == 1 and key_status() == 1
            assert home.stat().st_ino != original_inode
            conf.write_text("porthome_fscrypt=on\nporthome_fscrypt_allow_without_luks=on\n")
            start(); login = open_session()
            assert (home / "before-migration").read_text() == "unpushed plaintext edit"
        assert record()[0] == "unlocked" and key_status() == 2
        assert shown_state() == "unlocked"
        assert (home / "hello.txt").read_text() == "portable fscrypt fixture\n"
        assert run("systemctl", "is-active", f"user@{uid}.service").stdout.strip() == "active"
        (home / "unpushed.txt").write_text("keep this local edit")
        busy = (home / "unpushed.txt").open("rb")
        close_session(login); login = None
        until(lambda: record()[0] == "locking", "FILES_BUSY status after user manager exits")
        assert key_status() == 3 and busy.read() == b"keep this local edit"
        assert shown_state() == "locking"
        assert run("systemctl", "is-active", f"user@{uid}.service", ok=False).stdout.strip() != "active"
        # A real authenticated re-login cancels the pending busy retry.
        login = open_session()
        assert key_status() == 2 and record()[0] == "unlocked"
        busy.close(); busy = None
        assert (home / "unpushed.txt").read_text() == "keep this local edit"
        close_session(login); login = None
        until(lambda: record()[0] == "locked", "complete logout lock")
        assert key_status() == 1 and shown_state() == "locked"
        assert "hello.txt" not in os.listdir(home), "locked view must expose ciphertext names only"
        try:
            (home / "forbidden").write_bytes(b"no")
            raise AssertionError("create succeeded without key")
        except OSError as error:
            assert error.errno == errno.ENOKEY
        # The same offline LOCAL vault unlocks without relay or broker cache.
        relay.terminate(); relay.wait(timeout=10); relay = None
        run("systemctl", "stop", unit.name); start()
        login = open_session()
        assert key_status() == 2 and (home / "unpushed.txt").read_text() == "keep this local edit"
        close_session(login); login = None
        until(lambda: record()[0] == "locked", "offline login logout lock")
        run("systemctl", "stop", unit.name)
        # Explicit offline generation re-key preserves unpushed data. The raw
        # seed enters a private pipe, not command arguments or persistent disk.
        readfd, writefd = os.pipe(); os.write(writefd, seed); os.close(writefd)
        try:
            run(build / "nostr-authd", "--fscrypt-migrate", db, account, readfd,
                "--allow-without-luks", pass_fds=(readfd,))
        finally:
            os.close(readfd)
        assert record()[2] == 2 and key_status() == 1
        start(); login = open_session()
        assert (home / "unpushed.txt").read_text() == "keep this local edit"
        close_session(login); login = None
        until(lambda: record()[0] == "locked", "re-keyed home locks")
        run("systemctl", "stop", unit.name)
        actual_id = record()[1]
        # Offline metadata/policy mismatch must fail closed before ADD_KEY.
        with sqlite3.connect(db / "fscrypt/state.db") as c:
            c.execute("UPDATE homes SET key_id=?", (bytes(16),))
        start(); refused_session()
        assert record()[0] == "repair-required" and key_status(actual_id) == 1
        print("PASS: real PAM provision/unlock, user-manager logout, busy lock, relogin, offline unlock, rekey, status and policy-downgrade refusal", flush=True)
    finally:
        if login:
            login.kill(); login.wait(timeout=10)
        if busy:
            busy.close()
        run("systemctl", "stop", unit.name, ok=False)
        if user_created:
            run("loginctl", "terminate-user", uid, ok=False)
            until(lambda: run("pgrep", "-u", uid, ok=False).returncode == 1,
                  "task account processes exit before cleanup")
        if relay:
            relay.terminate(); relay.wait(timeout=10)
        print(run("journalctl", "-u", unit.name, "--no-pager", ok=False).stdout, flush=True)
        if home.exists():
            shutil.rmtree(home)
        if user_created:
            run("userdel", name)
        if group_created and run("getent", "group", name, ok=False).returncode == 0:
            run("groupdel", name)
        unit.unlink(missing_ok=True); pam.unlink(missing_ok=True)
        run("systemctl", "daemon-reload", ok=False)
        shutil.rmtree(base)


if __name__ == "__main__":
    main()
