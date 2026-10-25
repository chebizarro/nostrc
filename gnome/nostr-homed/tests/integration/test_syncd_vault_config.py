#!/usr/bin/env python3
"""Exercise vault config through the real daemon's offline --check entrypoint."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

binary = str(Path(sys.argv[1]).resolve())
entry = {"path": "Portable/Team", "vault_id": "01" * 32, "is_owner": True}
with tempfile.TemporaryDirectory(prefix="syncd-vault-config-") as tmp:
    base = Path(tmp)
    home = base / "home"
    home.mkdir()
    runtime = base / "runtime"
    runtime.mkdir(mode=0o700)
    config = base / "syncd.json"
    env = {k: v for k, v in os.environ.items()
           if not k.startswith("NOSTR_HOMED_") and k != "NOSTR_HOME_STATE"}
    env.update(HOME=str(home), XDG_STATE_HOME=str(base / "state"),
               XDG_CACHE_HOME=str(base / "cache"), XDG_RUNTIME_DIR=str(runtime),
               NOSTR_HOMED_SYNCD_HOME=str(home),
               NOSTR_HOMED_SYNCD_STATE_DIR=str(base / "syncd-state"),
               NOSTR_HOMED_SYNCD_SEED_FILE=str(base / "no-broker-seed"),
               NOSTR_HOMED_SYNCD_CONFIG=str(config))

    def check(value, valid=False, count=0, raw=False):
        config.write_text(value if raw else json.dumps(value))
        result = subprocess.run([binary, "--check"], env=env, capture_output=True,
                                text=True, timeout=10)
        assert result.returncode == (0 if valid else 2), (value, result.returncode,
                                                        result.stdout, result.stderr)
        if valid:
            assert f"vault config entries={count}" in result.stderr, result.stderr
        else:
            assert "invalid vault config" in result.stderr, result.stderr

    check({"vaults": []}, valid=True)
    check({"vaults": [entry]}, valid=True, count=1)
    check({"vaults": [entry, dict(entry, path="Portable/Teamwork", vault_id="02" * 32,
                                  is_owner=False)]}, valid=True, count=2)
    for path in ("", "/Portable/Team", "../Team", "Portable/../Team", "Portable//Team",
                 "Portable/Team/", "Portable/./Team", "Portable/Team\\x", "x\u0000y",
                 "Portable/.conflicts", "Portable/.conflicts/file", "a" * 256):
        check({"vaults": [dict(entry, path=path)]})
    for value in ({}, {"vaults": None}, {"vaults": [], "other": 1},
                  {"vaults": [dict(entry, other=True)]},
                  {"vaults": [dict(entry, is_owner=1)]},
                  {"vaults": [dict(entry, vault_id="00")]},
                  {"vaults": [dict(entry, vault_id="z" * 64)]},
                  {"vaults": [entry, entry]},
                  {"vaults": [entry, dict(entry, path="Elsewhere")]},
                  {"vaults": [entry, dict(entry, path="Portable/Team/nested", vault_id="02" * 32)]},
                  {"vaults": [entry, dict(entry, path="Portable", vault_id="02" * 32)]}):
        check(value)
    check('{"vaults":[],"vaults":[]}', raw=True)
    check('{"vaults":[{"path":"a","path":"b","vault_id":"' + "01" * 32 +
          '","is_owner":true}]}', raw=True)
    seed_drop = base / "no-broker-seed"
    seed_drop.write_text("03" * 32)
    check({"vaults": [dict(entry, is_owner="yes")]})
    assert seed_drop.read_text() == "03" * 32, "invalid config consumed broker credential"
    config.write_text(json.dumps({"vaults": []}))
    result = subprocess.run([binary, "--vault-sync"], env=env, capture_output=True, timeout=10)
    assert result.returncode == 2 and seed_drop.read_text() == "03" * 32, "empty vault one-shot consumed broker credential"
    config.write_text(json.dumps({"vaults": [entry]}))
    for budget in ("", "-1", "+1", " 1", "1x", str(2**63)):
        env["NOSTR_HOMED_SYNCD_RETENTION_BYTES"] = budget
        result = subprocess.run([binary, "--check"], env=env, capture_output=True, timeout=10)
        assert result.returncode == 2 and b"retention byte budget" in result.stderr, result.stderr
        assert seed_drop.read_text() == "03" * 32, "invalid budget consumed broker credential"
    env.pop("NOSTR_HOMED_SYNCD_RETENTION_BYTES")
    seed_drop.unlink()
    for budget in ("0", "134217728"):
        env["NOSTR_HOMED_SYNCD_RETENTION_BYTES"] = budget
        check({"vaults": [entry]}, valid=True, count=1)
    env.pop("NOSTR_HOMED_SYNCD_RETENTION_BYTES")
    config.unlink()
    result = subprocess.run([binary, "--check"], env=env, capture_output=True, timeout=10)
    assert result.returncode == 2, result.stderr
    env.pop("NOSTR_HOMED_SYNCD_CONFIG")
    result = subprocess.run([binary, "--check"], env=env, capture_output=True, timeout=10)
    assert result.returncode == 0, result.stderr
print("vault config: real --check accepts valid owner/member entries and refuses malformed sources")
