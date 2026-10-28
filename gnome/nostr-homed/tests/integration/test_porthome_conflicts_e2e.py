#!/usr/bin/env python3
"""nostrc-h10m.6 — conflict listing and resolution through the real binaries.

Runs the real nostr-home-syncd (managed via a seed file, no reachable
network) and the real nostr-home-status CLI against one sandbox:

  journal rows -> syncd projects them into porthome-status.json
  -> `nostr-home-status conflicts` lists them in plain language
  -> `conflicts resolve` drops a request in syncd's inbox
  -> syncd's main loop applies it and reports in syncd.resolutions[]
  -> the CLI reports the outcome with the documented exit status.

The resolutions used here touch only files the snapshot never published,
so syncd resolves them without a generation (no relay needed). Paths that
must publish are covered by test_syncd_conflicts / test_syncd_vault_runtime.

usage: test_porthome_conflicts_e2e.py NOSTR_HOME_SYNCD NOSTR_HOME_STATUS
"""
import json
import os
import shutil
import signal
import subprocess
import sys
import tempfile
import time

SYNCD, CLI = sys.argv[1], sys.argv[2]
EDIT_ID = "0123456789abcdef0123456789abcdef"
DEL_ID = "fedcba9876543210fedcba9876543210"


def wait_for(pred, secs, what):
    end = time.monotonic() + secs
    while time.monotonic() < end:
        v = pred()
        if v:
            return v
        time.sleep(0.05)
    raise AssertionError(f"timed out waiting for {what}")


def main():
    root = tempfile.mkdtemp(prefix="nh-conflicts-e2e-")
    home, state = f"{root}/home", f"{root}/state"
    xdg_state = f"{root}/xdg-state"
    status_path = f"{xdg_state}/nostr-homed/porthome-status.json"
    for d in (home, state, f"{root}/xdg-cache", f"{root}/xdg-config"):
        os.makedirs(d, mode=0o700)
    files = {
        "notes.txt": "current\n",
        "notes.txt.conflict-laptop-20261028T101500Z": "saved\n",
        "keeper.txt": "kept local edit\n",
        "keeper.txt.conflict-laptop-1.conflict-deleted": "",
    }
    for rel, body in files.items():
        with open(f"{home}/{rel}", "w") as f:
            f.write(body)
    journal = {"version": 1, "conflicts": [
        {"id": EDIT_ID, "path": "notes.txt", "kind": "edit",
         "saved": "notes.txt.conflict-laptop-20261028T101500Z", "saved_side": "remote",
         "generation": 5, "detected_at": 1793000000},
        {"id": DEL_ID, "path": "keeper.txt", "kind": "deleted-remotely",
         "saved": "keeper.txt.conflict-laptop-1.conflict-deleted",
         "generation": 5, "detected_at": 1793000000},
    ]}
    with open(f"{state}/home-conflicts.json", "w") as f:
        json.dump(journal, f)
    # A known (empty) snapshot base, as on a managed home: the conflict
    # copies were never published, so dropping them needs no generation.
    with open(f"{state}/snapshot.json", "w") as f:
        json.dump({"schema": 1, "generation": 5, "root": home, "d_tag": "home",
                   "account_pubkey_hex": "", "root_id_hex": "00" * 32, "files": {}}, f)
    with open(f"{root}/home_seed", "w") as f:
        f.write("ab" * 32 + "\n")

    env = dict(os.environ)
    for k in ("NOSTR_HOMED_SYNCD_SEED_HEX", "NOSTR_HOMED_SYNCD_TEST_MODE", "NOSTR_HOME_STATE"):
        env.pop(k, None)
    env.update({
        "HOME": home, "NOSTR_HOMED_SYNCD_HOME": home, "NOSTR_HOMED_SYNCD_STATE_DIR": state,
        "XDG_STATE_HOME": xdg_state, "XDG_CACHE_HOME": f"{root}/xdg-cache",
        "XDG_CONFIG_HOME": f"{root}/xdg-config",
        "NOSTR_HOMED_SYNCD_SEED_FILE": f"{root}/home_seed",
        "NOSTR_HOMED_SYNCD_NSEC_HEX": "11223344556677881122334455667788" * 2,
        "NOSTR_HOMED_SYNCD_BLOSSOM": "https://ignored.invalid",
        "NOSTR_HOMED_SYNCD_RELAYS": "wss://ignored.invalid",
        "NOSTR_HOMED_SYNCD_NOTIFY": "0",
    })
    log = open(f"{root}/daemon.log", "w")
    daemon = subprocess.Popen([SYNCD], env=env, stderr=log, stdout=log)

    def status():
        try:
            with open(status_path) as f:
                return json.load(f)
        except (OSError, ValueError):
            return None

    def cli(*args, timeout=60):
        return subprocess.run([CLI, *args], env=env, capture_output=True, text=True,
                              timeout=timeout)

    try:
        wait_for(lambda: len((status() or {}).get("syncd", {}).get("home_conflicts", [])) == 2,
                 20, "syncd to project the journal")
        s = status()["syncd"]
        assert s["conflicts_active_count"] == 2, s

        out = cli("conflicts")
        assert out.returncode == 0, out
        assert "2 conflicts need your attention." in out.stdout, out.stdout
        assert "changed in two places" in out.stdout and "The current version is yours." in out.stdout
        assert "This file was deleted on another device, but it was changed here." in out.stdout
        assert "--keep-both" in out.stdout and "Keep both" in out.stdout
        listed = json.loads(cli("conflicts", "--json").stdout)
        assert {c["id"] for c in listed} == {EDIT_ID, DEL_ID}
        print("PASS list: syncd projects the journal; CLI lists it in plain language", flush=True)

        # Refusals never reach syncd.
        r = cli("conflicts", "resolve", "ffffff", "--keep-current")
        assert r.returncode == 1 and "No unresolved conflict" in r.stderr, r
        r = cli("conflicts", "resolve", DEL_ID[:8], "--keep-both")
        assert r.returncode == 1 and "not available" in r.stderr, r
        r = cli("conflicts", "resolve", EDIT_ID[:8], "--merge")
        assert r.returncode == 2, r
        print("PASS refusals: unknown id, unsupported choice, unknown option", flush=True)

        # keep-mine on the edit row: the current version is this device's.
        r = cli("conflicts", "resolve", EDIT_ID[:8], "--keep-mine", "--timeout", "20")
        assert r.returncode == 0, (r, open(f"{root}/daemon.log").read())
        assert "--keep-mine means: Keep the current version." in r.stdout, r.stdout
        assert "Done." in r.stdout, r.stdout
        assert not os.path.exists(f"{home}/notes.txt.conflict-laptop-20261028T101500Z")
        assert open(f"{home}/notes.txt").read() == "current\n"

        # Deletion conflict: keep the file (marker removed, file kept).
        r = cli("conflicts", "resolve", DEL_ID, "--keep-current", "--timeout", "20")
        assert r.returncode == 0, r
        assert os.path.exists(f"{home}/keeper.txt")
        assert not os.path.exists(f"{home}/keeper.txt.conflict-laptop-1.conflict-deleted")
        s = wait_for(lambda: (status() or {}).get("syncd") if
                     not status()["syncd"].get("home_conflicts") else None, 10, "rows to clear")
        assert s["conflicts_active_count"] == 0
        assert [x["result"] for x in s["resolutions"][:2]] == ["ok", "ok"], s["resolutions"]
        persisted = json.load(open(f"{state}/home-conflicts.json"))
        assert all(c["resolution"]["state"] == "resolved" for c in persisted["conflicts"])
        print("PASS resolve: syncd applied both choices and reported ok; CLI exited 0", flush=True)

        # A malformed request is removed and reported, not executed.
        inbox = os.path.dirname(status_path) + "/requests"
        bad = "9" * 32
        with open(f"{inbox}/.{bad}.tmp", "w") as f:
            f.write('{"schema":1,"op":"rm","request":"%s"}' % bad)
        os.rename(f"{inbox}/.{bad}.tmp", f"{inbox}/{bad}.json")
        wait_for(lambda: any(x["request"] == bad and x["reason"] == "invalid-request"
                             for x in (status() or {}).get("syncd", {}).get("resolutions", [])),
                 10, "invalid request to be reported")
        assert not os.path.exists(f"{inbox}/{bad}.json")
        print("PASS inbox: malformed request removed and reported", flush=True)
    finally:
        daemon.send_signal(signal.SIGTERM)
        try:
            daemon.wait(20)
        except subprocess.TimeoutExpired:
            daemon.kill()
        log.close()
        if os.environ.get("NH_KEEP_SANDBOX"):
            print(f"sandbox kept at {root}")
        else:
            shutil.rmtree(root, ignore_errors=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
