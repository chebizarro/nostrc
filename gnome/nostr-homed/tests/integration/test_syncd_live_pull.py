#!/usr/bin/env python3
"""Running-daemon personal-home pull with a fixture relay and Blossom."""
import ctypes
import hashlib
import json
import os
from pathlib import Path
import select
import signal
import socket
import subprocess
import sys
import tempfile
import time

SYNCD, STATUS, SEED, RELAYD, BLOSSOM = sys.argv[1:6]
FUSE = sys.argv[6] if len(sys.argv) > 6 else None
SOAK = sys.argv[7] if len(sys.argv) > 7 else None
SECRET = "11" * 32
HOME_SEED = "22" * 32
IN_ALL = 0x00000FFF
libc = ctypes.CDLL(None, use_errno=True)
libc.inotify_init1.argtypes = [ctypes.c_int]
libc.inotify_add_watch.argtypes = [ctypes.c_int, ctypes.c_char_p, ctypes.c_uint32]


def port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def ready(proc, number, label):
    deadline = time.monotonic() + 8
    while time.monotonic() < deadline:
        if proc.poll() is not None:
            raise AssertionError(f"{label} exited {proc.returncode}")
        with socket.socket() as sock:
            sock.settimeout(0.1)
            if sock.connect_ex(("127.0.0.1", number)) == 0:
                return
    raise AssertionError(f"{label} did not listen")


def stop(proc):
    if proc is None or proc.poll() is not None:
        return
    proc.send_signal(signal.SIGCONT)
    proc.terminate()
    try:
        proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait(timeout=5)


def wait_for(predicate, dirs, description, processes=(), timeout=35):
    fd = libc.inotify_init1(os.O_NONBLOCK | os.O_CLOEXEC)
    if fd < 0:
        raise OSError(ctypes.get_errno(), "inotify_init1")
    try:
        for directory in dirs:
            if not Path(directory).exists():
                Path(directory).mkdir(parents=True, exist_ok=True)
            if libc.inotify_add_watch(fd, os.fsencode(directory), IN_ALL) < 0:
                raise OSError(ctypes.get_errno(), f"inotify_add_watch {directory}")
        result = predicate()
        deadline = time.monotonic() + timeout
        while not result:
            for proc in processes:
                if proc.poll() is not None:
                    raise AssertionError(f"{description}: process exited {proc.returncode}")
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise AssertionError(f"timed out waiting for {description}")
            readable, _, _ = select.select([fd], [], [], remaining)
            if not readable:
                raise AssertionError(f"timed out waiting for {description}")
            try:
                os.read(fd, 65536)
            except BlockingIOError:
                pass
            result = predicate()
        return result
    finally:
        os.close(fd)


def generation(state):
    try:
        return int((state / "generation").read_text().strip())
    except (OSError, ValueError):
        return 0


def status(machine):
    try:
        return json.loads((machine / "xdg" / "nostr-homed" / "porthome-status.json").read_text())
    except (OSError, ValueError):
        return {}


def machine(root, name, relay_port, blossom_port, initialise=True, lazy=False):
    base = root / name
    home, state = base / "home", base / "state"
    for directory in (home, state, base / "xdg", base / "cache", base / "config"):
        directory.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ)
    env.update(HOME=str(home), XDG_STATE_HOME=str(base / "xdg"),
               XDG_CACHE_HOME=str(base / "cache"), XDG_CONFIG_HOME=str(base / "config"),
               NOSTR_HOMED_SYNCD_HOME=str(home), NOSTR_HOMED_SYNCD_STATE_DIR=str(state),
               NOSTR_HOMED_SYNCD_TEST_MODE="1", NOSTR_HOMED_SYNCD_SEED_HEX=HOME_SEED,
               NOSTR_HOMED_SYNCD_NSEC_HEX=SECRET,
               NOSTR_HOMED_SYNCD_RELAYS=f"ws://127.0.0.1:{relay_port}",
               NOSTR_HOMED_SYNCD_BLOSSOM=f"http://127.0.0.1:{blossom_port}",
               NOSTR_HOMED_SYNCD_MIN_REPLICATION="1", NH_PORTHOME_ALLOW_INSECURE="1",
               NOSTR_HOMED_SYNCD_NOTIFY="0")
    if lazy:
        env["NOSTR_HOMED_SYNCD_LAZY_SUBTREES"] = "Archive"
    if initialise:
        subprocess.run([SEED, str(home), str(state), SECRET, HOME_SEED], check=True, env=env)
    return base, env


def launch_syncd(base, env):
    log = (base / "syncd.log").open("a")
    return subprocess.Popen([SYNCD], env=env, stdout=log, stderr=log)


def file_is(path, expected):
    try:
        return path.read_text() == expected
    except OSError:
        return False


def main():
    with tempfile.TemporaryDirectory(prefix="syncd-live-") as tmp:
        root = Path(tmp)
        rp, bp = port(), port()
        relay_dir = root / "relay"
        relay_dir.mkdir()
        (relay_dir / "relay.toml").write_text(
            f'listen = "127.0.0.1:{rp}"\nstorage_driver = "nostrdb"\nauth = "off"\n')
        blossom_env = dict(os.environ, FAKE_BLOSSOM_PORT=str(bp),
                           FAKE_BLOSSOM_DIR=str(root / "blobs"))
        procs = []
        def relay_start():
            log = (root / "relayd.log").open("a")
            proc = subprocess.Popen([RELAYD], cwd=relay_dir, stdout=log, stderr=log)
            procs.append(proc)
            ready(proc, rp, "fixture relay")
            return proc
        try:
            relay = relay_start()
            blog = (root / "blossom.log").open("w")
            blossom = subprocess.Popen([sys.executable, BLOSSOM], env=blossom_env,
                                       stdout=blog, stderr=blog)
            procs.append(blossom)
            ready(blossom, bp, "fixture Blossom")
            a, ae = machine(root, "a", rp, bp)
            b, be = machine(root, "b", rp, bp)
            ad = launch_syncd(a, ae); procs.append(ad)
            bd = launch_syncd(b, be); procs.append(bd)
            wait_for(lambda: status(a).get("syncd") and status(b).get("syncd"),
                     [a / "xdg" / "nostr-homed", b / "xdg" / "nostr-homed"],
                     "daemon startup", (ad, bd))
            (b / "home" / "note.txt").write_text("v1 from B\n")
            wait_for(lambda: file_is(a / "home" / "note.txt", "v1 from B\n"),
                     [a / "home"], "B live edit at A", (ad, bd))
            first = wait_for(lambda: generation(b / "state") or None,
                             [b / "state"], "B first generation", (bd,))
            wait_for(lambda: generation(a / "state") >= first,
                     [a / "state"], "A persisted live generation", (ad,))
            print("PASS live two-writer pull", flush=True)

            # A failed Blossom fetch must retain the pointer and recover
            # without waiting for a new relay EVENT.
            ad.send_signal(signal.SIGSTOP)
            os.waitpid(ad.pid, os.WUNTRACED)
            before = generation(b / "state")
            (b / "home" / "retry.txt").write_text("recover the blob\n")
            def retry_chunk():
                try:
                    snap = json.loads((b / "state" / "snapshot.json").read_text())
                    if snap["generation"] <= before:
                        return None
                    return snap["files"]["retry.txt"]["chunk_addrs_hex"][0]
                except (OSError, ValueError, KeyError, IndexError):
                    return None
            chunk = wait_for(retry_chunk, [b / "state"], "B retry pointer", (bd,))
            blob = root / "blobs" / chunk
            withheld = root / "blobs" / (chunk + ".withheld")
            blob.rename(withheld)
            ad.send_signal(signal.SIGCONT)
            wait_for(lambda: "pointer apply failed" in (a / "syncd.log").read_text(),
                     [a / "syncd.log"], "A observes failed fetch", (ad,))
            withheld.rename(blob)
            wait_for(lambda: file_is(a / "home" / "retry.txt", "recover the blob\n"),
                     [a / "home"], "A retries retained pointer", (ad,))
            print("PASS failed Blossom fetch retries with backoff", flush=True)
            wait_for(lambda: generation(a / "state") >= generation(b / "state"),
                     [a / "state"], "A settled after retry", (ad, bd))

            ad.send_signal(signal.SIGSTOP)
            os.waitpid(ad.pid, os.WUNTRACED)
            (a / "home" / "note.txt").write_text("mine at A\n")
            before = generation(b / "state")
            (b / "home" / "note.txt").write_text("theirs at B\n")
            def b_published_theirs():
                try:
                    snap = json.loads((b / "state" / "snapshot.json").read_text())
                    return snap["generation"] > before and snap["files"]["note.txt"]["content_hash_hex"] == \
                        hashlib.sha256(b"theirs at B\n").hexdigest()
                except (OSError, ValueError, KeyError):
                    return False
            wait_for(b_published_theirs, [b / "state"], "B conflicting publish", (bd,))
            ad.send_signal(signal.SIGCONT)
            rows = wait_for(lambda: status(a).get("syncd", {}).get("home_conflicts"),
                            [a / "xdg" / "nostr-homed"], "A conflict projection", (ad,))
            assert rows[0]["path"] == "note.txt", rows
            result = subprocess.run([STATUS, "conflicts", "resolve", rows[0]["id"],
                                     "--keep-mine", "--timeout", "20"],
                                    env=ae, capture_output=True, text=True, timeout=25)
            assert result.returncode == 0, (result.stdout, result.stderr)
            wait_for(lambda: not status(a).get("syncd", {}).get("home_conflicts"),
                     [a / "xdg" / "nostr-homed"], "resolved conflict", (ad,))
            assert file_is(a / "home" / "note.txt", "mine at A\n")
            print("PASS conflict journal and h10m.6 inbox resolution", flush=True)

            stop(relay)
            relay = relay_start()
            before = generation(b / "state")
            (b / "home" / "after-restart.txt").write_text("reconnected\n")
            wait_for(lambda: generation(b / "state") > before, [b / "state"],
                     "B publish after relay restart", (bd,))
            wait_for(lambda: file_is(a / "home" / "after-restart.txt", "reconnected\n"),
                     [a / "home"], "A reconnect delivery", (ad,))
            print("PASS relay restart reconnect", flush=True)

            stop(ad)
            before = generation(b / "state")
            (b / "home" / "while-away.txt").write_text("backfill\n")
            wait_for(lambda: generation(b / "state") > before, [b / "state"],
                     "B publish while A offline", (bd,))
            ad = launch_syncd(a, ae); procs.append(ad)
            wait_for(lambda: file_is(a / "home" / "while-away.txt", "backfill\n"),
                     [a / "home"], "A EOSE backfill", (ad,))
            print("PASS catch-up backfill", flush=True)
            stop(ad)  # isolate the fresh-login publication from A's catch-up echoes
            a_head = generation(a / "state")
            wait_for(lambda: generation(b / "state") >= a_head,
                     [b / "state"], "B caught up to A before Archive", (bd,))

            before = generation(b / "state")
            (b / "home" / "Archive").mkdir()
            (b / "home" / "Archive" / "report.txt").write_text("cold archive\n")
            wait_for(lambda: generation(b / "state") > before, [b / "state"],
                     "Archive publish", (bd,))
            c, ce = machine(root, "c", rp, bp, initialise=False, lazy=True)
            (c / "home" / "local-first.txt").write_text("kept through catch-up\n")
            cd = launch_syncd(c, ce); procs.append(cd)
            def lazy_snapshot():
                try:
                    snap = json.loads((c / "state" / "snapshot.json").read_text())
                    return snap if snap.get("files", {}).get("Archive", {}).get("lazy") else None
                except (OSError, ValueError):
                    return None
            snap = wait_for(lazy_snapshot, [c / "state"], "fresh lazy Archive snapshot", (cd,))
            assert snap["generation"] > 0, snap
            assert (c / "home" / "Archive").is_dir()
            assert not (c / "home" / "Archive" / "report.txt").exists()
            assert file_is(c / "home" / "local-first.txt", "kept through catch-up\n")
            wait_for(lambda: file_is(b / "home" / "local-first.txt", "kept through catch-up\n"),
                     [b / "home"], "C's pre-login edit published after catch-up", (bd, cd))
            print("PASS fresh-login lazy snapshot publication", flush=True)
            if FUSE and SOAK:
                mount = c / "home" / "Portable"
                mount.mkdir()
                fuse_env = dict(ce, NH_FUSE_STATE_DIR=str(c / "state"),
                                NH_FUSE_MOUNTPOINT=str(mount),
                                NH_FUSE_SEED_HEX=HOME_SEED,
                                NH_FUSE_BLOSSOM=f"http://127.0.0.1:{bp}")
                fuse_log = (c / "fuse.log").open("w")
                fuse = subprocess.Popen([FUSE], env=fuse_env, stdout=fuse_log, stderr=fuse_log)
                procs.append(fuse)
                def mounted():
                    try:
                        return any("fuse.porthome" in line and str(mount) in line
                                   for line in Path("/proc/self/mountinfo").read_text().splitlines())
                    except OSError:
                        return False
                deadline = time.monotonic() + 8
                while not mounted() and time.monotonic() < deadline:
                    if fuse.poll() is not None:
                        raise AssertionError(f"FUSE exited: {(c / 'fuse.log').read_text()}")
                    time.sleep(0.05)  # mount readiness only; no relay-event polling
                assert mounted(), (c / "fuse.log").read_text()
                report = root / "soak.json"
                outcome = subprocess.run([sys.executable, SOAK,
                    "--mountpoint", str(mount), "--sample", "Archive/report.txt",
                    "--sha256", hashlib.sha256(b"cold archive\n").hexdigest(),
                    "--duration-seconds", "5", "--interval-seconds", "1",
                    "--cache-dir", str(c / "cache" / "nostr-homed" / "blobs"),
                    "--status", str(c / "xdg" / "nostr-homed" / "porthome-status.json"),
                    "--pid", str(fuse.pid), "--report", str(report),
                    "--label", "headless-container"], capture_output=True, text=True,
                    timeout=20)
                assert outcome.returncode == 0, (outcome.stdout, outcome.stderr,
                                                 (c / "fuse.log").read_text())
                print("PASS short lazy Archive FUSE soak", flush=True)
        except Exception:
            for name in ("a", "b", "c"):
                log = root / name / "syncd.log"
                if log.exists():
                    print(f"--- {name} syncd ---\n{log.read_text()[-10000:]}", file=sys.stderr)
                home = root / name / "home"
                if home.exists():
                    print(f"--- {name} home --- {[(str(p.relative_to(home)), p.read_bytes()[:100] if p.is_file() else b'') for p in home.rglob('*')]}", file=sys.stderr)
                snap = root / name / "state" / "snapshot.json"
                if snap.exists():
                    print(f"--- {name} snapshot ---\n{snap.read_text()[:3000]}", file=sys.stderr)
            for name in ("relayd.log", "blossom.log"):
                log = root / name
                if log.exists():
                    print(f"--- {name} ---\n{log.read_text()[-5000:]}", file=sys.stderr)
            raise
        finally:
            for proc in reversed(procs):
                stop(proc)


if __name__ == "__main__":
    main()
