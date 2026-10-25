#!/usr/bin/env python3
"""SV-10 extension of jmx0: real syncd/FUSE callbacks, two Linux UIDs, VSP lab.
Services are caller-owned; this runner owns and removes only its account homes.
The SDK fixture helper performs owner administration and actual NIP-59 delivery;
no owner-private state or plaintext metadata is copied to the member.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import selectors
import shutil
import subprocess
import tempfile
import time

P1 = "79be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798"
P2 = "c6047f9441ed7d6d3045406e95c07cd85c778e4b8cef3ca7abac09b95c709ee5"
p = argparse.ArgumentParser()
p.add_argument("--build", required=True, type=Path)
p.add_argument("--allowlist", required=True, type=Path)
p.add_argument("--http-receipts", required=True, type=Path)
p.add_argument("--blossom", default="http://127.0.0.1:18080")
p.add_argument("--relay", default="ws://127.0.0.1:17777")
p.add_argument("--disable-leak-check", action="store_true", help="Diagnostic only: pre-existing libblossom signed-GET leak nostrc-k1zq6; not leak-check acceptance")
a = p.parse_args()
assert os.geteuid() == 0, "Run in a disposable Linux test container: two actual UIDs are required"
bin_dir = a.build / "gnome/nostr-homed"
root = Path(tempfile.mkdtemp(prefix="nostrc-jmx0-vault-"))
root.chmod(0o755)
processes = []

def account(name, uid, secret, seed):
    base = root / name
    base.mkdir(mode=0o700)
    os.chown(base, uid, uid)
    for child in ("home", "state", "cache", "status"):
        (base / child).mkdir(mode=0o700)
        os.chown(base / child, uid, uid)
    env = os.environ.copy()
    env.update(HOME=str(base / "home"), XDG_CACHE_HOME=str(base / "cache"),
        XDG_STATE_HOME=str(base / "status"), NOSTR_HOMED_SYNCD_HOME=str(base / "home"),
        NOSTR_HOMED_SYNCD_STATE_DIR=str(base / "state"), NOSTR_HOMED_SYNCD_NSEC_HEX=secret,
        NOSTR_HOMED_SYNCD_TEST_MODE="1", NOSTR_HOMED_SYNCD_SEED_HEX=seed,
        NOSTR_HOMED_SYNCD_BLOSSOM=a.blossom, NOSTR_HOMED_SYNCD_RELAYS=a.relay,
        NOSTR_HOMED_SYNCD_MIN_REPLICATION="1", NOSTR_HOMED_SYNCD_NOTIFY="0",
        ASAN_OPTIONS=f"detect_leaks={0 if a.disable_leak_check else 1}:abort_on_error=1", UBSAN_OPTIONS="halt_on_error=1")
    return dict(base=base, uid=uid, env=env, secret=secret, seed=seed)

def as_user(ac):
    def change():
        os.setgroups([])
        os.setgid(ac["uid"])
        os.setuid(ac["uid"])
    return change

def run(ac, binary, *args, expected=0, **extra):
    r = subprocess.run([str(bin_dir / binary), *map(str, args)], env=ac["env"],
        preexec_fn=as_user(ac), text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        timeout=70, **extra)
    if r.returncode != expected:
        raise AssertionError(f"{binary} {args}: exit={r.returncode}\n{r.stdout}\n{r.stderr}")
    print(r.stdout.strip(), flush=True)
    return r.stdout

def config(ac, vid, owner):
    f = ac["base"] / "vaults.json"
    f.write_text(json.dumps({"vaults": [dict(path="Portable/Team", vault_id=vid, is_owner=owner)]}))
    f.chmod(0o600)
    os.chown(f, ac["uid"], ac["uid"])
    ac["env"]["NOSTR_HOMED_SYNCD_CONFIG"] = str(f)

def fuse(ac, path, expected_hash=None, expected=0):
    env = ac["env"].copy()
    ac["env"].update(NH_FUSE_TEST_HARNESS="1", NH_FUSE_TEST_READ=path,
        NH_FUSE_STATE_DIR=str(ac["base"] / "state"), NH_FUSE_MOUNTPOINT=str(ac["base"] / "home/Portable"),
        NH_FUSE_SEED_HEX=ac["seed"])
    try:
        result = run(ac, "nostr-home-fuse", expected=expected)
        if expected_hash:
            assert f"sha256={expected_hash}" in result
    finally:
        ac["env"] = env

def admin(ac, action, vid):
    return run(ac, "test_syncd_vault_runtime", "admin", action, ac["base"] / "state",
        vid, ac["seed"], ac["secret"], P2, a.blossom, a.relay)

def launch(ac):
    proc = subprocess.Popen([str(bin_dir / "nostr-home-syncd")], env=ac["env"],
        preexec_fn=as_user(ac), stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=False)
    processes.append(proc)
    return proc

def until(proc, condition, timeout=40):
    deadline = time.monotonic() + timeout
    select = selectors.DefaultSelector()
    for child in processes:
        if child.poll() is None:
            select.register(child.stderr, selectors.EVENT_READ)
    pending = b""
    try:
        while time.monotonic() < deadline:
            if condition():
                return
            assert proc.poll() is None, f"daemon exited {proc.returncode}: {pending.decode(errors='replace')}"
            ready = select.select(max(0, deadline - time.monotonic()))
            assert ready, "daemon condition deadline expired (not an EOSE substitute)"
            for key, _ in ready:
                block = os.read(key.fd, 65536)
                assert block, "daemon stderr closed"
                pending += block
                for line in block.decode(errors="replace").splitlines():
                    print(line, flush=True)
    finally:
        select.close()

def ready(proc):
    deadline = time.monotonic() + 40
    sel = selectors.DefaultSelector()
    sel.register(proc.stderr, selectors.EVENT_READ)
    data = b""
    try:
        while b"syncd: vault catch-up complete" not in data:
            assert sel.select(max(0, deadline-time.monotonic())), "EOSE-bounded bootstrap did not finish"
            chunk = os.read(proc.stderr.fileno(), 65536)
            assert chunk, data.decode(errors="replace")
            data += chunk
        print(data.decode(errors="replace"), flush=True)
    finally:
        sel.close()

def stop(proc):
    if proc.poll() is None:
        proc.terminate()
    out = proc.communicate(timeout=20)[1]
    assert proc.returncode == 0, out.decode(errors="replace")
    assert b"Sanitizer" not in out and b"runtime error:" not in out, out.decode(errors="replace")

try:
    if a.disable_leak_check:
        print("DIAGNOSTIC: LeakSanitizer disabled for known pre-existing nostrc-k1zq6; not leak-check acceptance", flush=True)
    owner = account("owner", 1001, f"{1:064x}", "11"*32)
    member = account("member", 1002, f"{2:064x}", "22"*32)
    migrate = account("migration", 1001, f"{1:064x}", "33"*32)
    a.allowlist.write_text(P1 + "\n" + P2 + "\n")
    vid = os.urandom(32).hex()
    config(owner, vid, True)
    config(member, vid, False)
    home = owner["base"] / "home"
    team = home / "Portable/Team"
    team.mkdir(parents=True, mode=0o700)
    (team / "nested").mkdir(mode=0o700)
    payload = bytes((i*31+i//127)%256 for i in range(1800000))
    (team / "file").write_bytes(payload)
    (team / "nested/child").write_text("directory conflict survives\n")
    for f in (home / "Portable", team, team / "nested", team / "file", team / "nested/child"):
        os.chown(f, owner["uid"], owner["uid"])
    run(owner, "nostr-home-syncd", "--vault-sync")
    admin(owner, "grant", vid)
    run(owner, "nostr-home-syncd", "--vault-sync")
    run(member, "nostr-home-syncd", "--vault-sync")
    member_file = member["base"] / "home/Portable/Team/file"
    assert member_file.read_bytes() == payload
    # Actual FUSE open/read/release callbacks, not a warm-HOME shortcut.
    member_file.unlink()
    fuse(member, "Portable/Team/file", hashlib.sha256(payload).hexdigest())
    member_file.write_bytes(payload)
    os.chown(member_file, member["uid"], member["uid"])
    # Real live subscription + watcher edit. Completion is an observable commit,
    # never a timed subscription close or a sleep standing in for EOSE.
    print("LIMIT: nostrc-vqu69 relayd ends subscriptions at EOSE; owner fold uses explicit current-head catch-up, not client polling", flush=True)
    mp = launch(member)
    ready(mp)
    edited = b"member writer edit\n"
    previous_generation = json.loads((member["base"] / "state/snapshot.json").read_text())["generation"]
    owner_edit = b"owner concurrent authoritative edit\n"
    (team / "file").write_bytes(owner_edit)
    member_file.write_bytes(edited)
    conflict = owner["base"] / f"home/Portable/.conflicts/Portable/Team/file@{P2}"
    until(mp, lambda: member_file.read_bytes() == edited and
        json.loads((member["base"] / "state/snapshot.json").read_text())["generation"] > previous_generation)
    stop(mp)
    run(owner, "nostr-home-syncd", "--vault-sync")
    assert conflict.is_file() and conflict.read_bytes() == edited
    assert (team / "file").read_bytes() == owner_edit
    fuse(owner, f"Portable/.conflicts/Portable/Team/file@{P2}", hashlib.sha256(edited).hexdigest())
    print("PASS two UIDs/npubs: grant -> cold bootstrap -> live edit/head -> owner checkpoint fold -> conflict copy", flush=True)
    # A deletion is preserved in the signed source and existing status metadata.
    member_file.unlink()
    member_team = member_file.parent
    (member_team / "new-dir").mkdir(mode=0o700)
    (member_team / "new-dir/child").write_bytes(b"whole directory conflict\n")
    long_name = "x" * 200
    (member_team / long_name).write_bytes(b"overflow conflict\n")
    for f in (member_team / "new-dir", member_team / "new-dir/child", member_team / long_name):
        os.chown(f, member["uid"], member["uid"])
    run(member, "nostr-home-syncd", "--vault-sync")
    run(owner, "nostr-home-syncd", "--vault-sync")
    directory_copy = owner["base"] / f"home/Portable/.conflicts/Portable/Team/new-dir@{P2}/child"
    assert directory_copy.read_bytes() == b"whole directory conflict\n"
    fuse(owner, str(directory_copy.relative_to(owner["base"] / "home")), hashlib.sha256(b"whole directory conflict\n").hexdigest())
    status = json.loads((owner["base"] / "status/nostr-homed/porthome-status.json").read_text())
    assert any(not row["present"] for row in status["syncd"]["vault_conflicts"])
    overflow = next(row for row in status["syncd"]["vault_conflicts"] if row["path"] == f"Portable/Team/{long_name}")
    assert overflow["destination"].startswith("Portable/.conflicts/.overflow/")
    assert (owner["base"] / "home" / overflow["destination"]).read_bytes() == b"overflow conflict\n"
    print("PASS deletion status metadata, full directory conflict copy and deterministic overflow mapping", flush=True)
    # Owner admin uses public SDK revoke and sends an actual NIP-59 self-grant.
    admin(owner, "revoke", vid)
    run(owner, "nostr-home-syncd", "--vault-sync")
    owner["env"]["NOSTR_HOMED_SYNCD_RETENTION_BYTES"] = "0"
    run(owner, "nostr-home-syncd", "--vault-retention")
    waiting = json.loads(run(owner, "test_syncd_vault_runtime", "inspect", owner["base"] / "state", vid, owner["seed"]))
    assert waiting["epoch"] == 2 and waiting["remaining"] > 0
    owner["env"].pop("NOSTR_HOMED_SYNCD_RETENTION_BYTES")
    run(owner, "nostr-home-syncd", "--vault-retention")
    current = json.loads(run(owner, "test_syncd_vault_runtime", "inspect", owner["base"] / "state", vid, owner["seed"]))
    assert current["epoch"] == 2 and current["remaining"] == 0
    snapshot = json.loads((owner["base"] / "state/snapshot.json").read_text())
    # The ring refusal uses the revoked member's unmodified persisted grant.
    run(member, "test_syncd_vault_runtime", "ring-refusal", member["base"] / "state", vid,
        member["seed"], member["secret"], a.blossom, a.relay, "2", current["head"], current["chunk_hash"], current["size_plain"])
    a.allowlist.write_text(P1 + "\n")
    # Both reference services load operator allowlists at startup. Service
    # administration has no invented protocol event or implicit client authority.
    input("Allowlist now excludes member. Restart the disposable VSP auth service and relay, then press Enter: ")
    # Service refusal on a new authenticated connection, independent of cache.
    wire = snapshot["files"]["Portable/Team/file"]
    run(member, "test_syncd_vault_runtime", "get", a.blossom, member["secret"], wire["chunk_addrs_hex"][0])
    run(member, "nostr-home-syncd", "--vault-sync", expected=8)
    print("PASS revoke: owner retention job, revoked ring EPOCH/no output, VSP GET AUTH and relay refusal", flush=True)
    # Migration uses a distinct owner state and existing v1 blobs, not CLI import.
    mid = os.urandom(32).hex()
    config(migrate, mid, True)
    before_seed = a.http_receipts.read_text().splitlines()
    old_hash = run(migrate, "test_syncd_vault_runtime", "seed-v1", migrate["base"] / "home",
        migrate["base"] / "state", a.blossom, migrate["secret"], migrate["seed"]).strip().splitlines()[-1]
    def uploads():
        return sum(json.loads(line).get("method") == "PUT" and json.loads(line).get("sha") == old_hash
            for line in a.http_receipts.read_text().splitlines() if line.startswith("{"))
    prior_uploads = sum(json.loads(line).get("method") == "PUT" and json.loads(line).get("sha") == old_hash
        for line in before_seed if line.startswith("{"))
    assert uploads() == prior_uploads + 1
    run(migrate, "nostr-home-syncd", "--vault-sync")
    assert uploads() == prior_uploads + 1, "migration re-uploaded an existing v1 sealed chunk"
    old = migrate["base"] / "home/Portable/Team/nested/old"
    digest = hashlib.sha256(old.read_bytes()).hexdigest()
    old.unlink()
    fuse(migrate, "Portable/Team/nested/old", digest)
    print("PASS actual daemon v1->v2 migration: old upload count remains one, cache-only FUSE reads old blob", flush=True)
    # Private cache eviction is a visible I/O error, not authenticated lazy GET.
    before = len(a.http_receipts.read_text().splitlines())
    shutil.rmtree(migrate["base"] / "cache/nostr-homed/blobs")
    fuse(migrate, "Portable/Team/nested/old", expected=6)
    assert len(a.http_receipts.read_text().splitlines()) == before
    print("PASS private cache miss: FUSE I/O error/status, zero HTTP/key fallback", flush=True)
    print("PASS SV-10 local Linux two-account scenario; kernel FUSE mount and post-EOSE relay delivery not exercised", flush=True)
finally:
    for proc in processes:
        if proc.poll() is None:
            proc.terminate()
            try:
                remaining = proc.communicate(timeout=15)[1]
                if remaining:
                    print(remaining.decode(errors="replace"), flush=True)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.communicate()
    a.allowlist.write_text(P1 + "\n" + P2 + "\n")
    shutil.rmtree(root)
