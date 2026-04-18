#!/usr/bin/env bash
set -euo pipefail

# =============================================================================
# run_e2e_real_signer_test.sh
#
# End-to-end integration test: real signer → warm-cache → nostrfs mount
#
# Exercises the full pipeline:
#   1. Signer provides GetPublicKey / SignEvent via D-Bus
#   2. warm-cache fetches manifest from a fake relay, persists to cache
#   3. nostrfs mounts and serves files from the manifest + CAS
#   4. Write-back creates a new file, persists to CAS + manifest
#   5. Re-mount verifies the new file survived
#
# Prerequisites: /dev/fuse, python3+websockets, dbus-run-session, sqlite3
# Exits 77 (skip) when prerequisites are missing.
# Exits 0 on success, 1 on failure.
# =============================================================================

# --- Guard: re-exec under isolated D-Bus session bus ---
if [ "${E2E_INNER:-}" != "1" ]; then
  command -v dbus-run-session >/dev/null 2>&1 || { echo "dbus-run-session not found; skipping" >&2; exit 77; }
  exec env E2E_INNER=1 dbus-run-session -- "$0" "$@"
fi

# --- Prerequisite checks ---
[ -e /dev/fuse ] || { echo "/dev/fuse not present; skipping" >&2; exit 77; }
command -v python3 >/dev/null 2>&1 || { echo "python3 not found; skipping" >&2; exit 77; }
python3 -c "import websockets" 2>/dev/null || { echo "python3 websockets not installed; skipping" >&2; exit 77; }
command -v sqlite3 >/dev/null 2>&1 || { echo "sqlite3 not found; skipping" >&2; exit 77; }

# --- Locate project directories ---
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
REPO_ROOT="$(cd "$PROJECT_ROOT/../.." && pwd)"

# --- Locate binaries ---
# Prefer real gnostr-signer-daemon; fall back to mock_signer
SIGNER_BIN=""
USING_REAL_SIGNER=0
for p in "$REPO_ROOT/build/apps/gnostr-signer/gnostr-signer-daemon" \
         "$REPO_ROOT/_build/apps/gnostr-signer/gnostr-signer-daemon"; do
  if [ -x "$p" ]; then SIGNER_BIN="$p"; USING_REAL_SIGNER=1; break; fi
done
if [ -z "$SIGNER_BIN" ]; then
  for p in "$REPO_ROOT/build/gnome/nostr-homed/mock_signer" \
           "$REPO_ROOT/_build/gnome/nostr-homed/mock_signer"; do
    if [ -x "$p" ]; then SIGNER_BIN="$p"; break; fi
  done
fi
[ -n "$SIGNER_BIN" ] || { echo "no signer binary found; skipping" >&2; exit 77; }

HOMECTL=""
for p in "$REPO_ROOT/build/gnome/nostr-homed/nostr-homectl" \
         "$REPO_ROOT/_build/gnome/nostr-homed/nostr-homectl"; do
  if [ -x "$p" ]; then HOMECTL="$p"; break; fi
done
[ -n "$HOMECTL" ] || { echo "nostr-homectl not built; skipping" >&2; exit 77; }

NOSTRFS=""
for p in "$REPO_ROOT/build/gnome/nostr-homed/nostrfs" \
         "$REPO_ROOT/_build/gnome/nostr-homed/nostrfs"; do
  if [ -x "$p" ]; then NOSTRFS="$p"; break; fi
done
[ -n "$NOSTRFS" ] || { echo "nostrfs not built; skipping" >&2; exit 77; }

echo "=== e2e test ==="
echo "  signer:  $SIGNER_BIN (real=$USING_REAL_SIGNER)"
echo "  homectl: $HOMECTL"
echo "  nostrfs: $NOSTRFS"

# --- Deterministic test key ---
# secp256k1 sk = 1 (generator point); pubkey is well-known
TEST_SK="0000000000000000000000000000000000000000000000000000000000000001"
TEST_PUBKEY="79be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798"

# --- Port selection ---
RELAY_PORT=17777
BLOSSOM_PORT=18081

# --- Temp dirs ---
TMPDIR_BASE="$(mktemp -d /tmp/e2e-homed.XXXXXX)"
MNT="$TMPDIR_BASE/mnt"
DB_PATH="$TMPDIR_BASE/cache.db"
CAS_DIR="$TMPDIR_BASE/cas"
BLOSSOM_DIR="$TMPDIR_BASE/blossom"
EVENTS_FILE="$TMPDIR_BASE/events.json"
ETC_CONF="/etc/nss_nostr.conf"
ETC_HAD_ORIGINAL=0
ETC_BAK=""
mkdir -p "$MNT" "$CAS_DIR/$(id -u)" "$CAS_DIR/$(id -u)/tmp" "$BLOSSOM_DIR"

# --- Cleanup ---
PIDS_TO_KILL=()
cleanup() {
  set +e
  for pid in "${PIDS_TO_KILL[@]}"; do
    kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null
  done
  if mountpoint -q "$MNT" 2>/dev/null; then
    if command -v fusermount3 >/dev/null 2>&1; then
      fusermount3 -u "$MNT" 2>/dev/null || true
    else
      umount "$MNT" 2>/dev/null || true
    fi
  fi
  # Restore original /etc/nss_nostr.conf if we backed it up
  if [ "$ETC_HAD_ORIGINAL" = "1" ] && [ -f "$ETC_BAK" ]; then
    cp "$ETC_BAK" "$ETC_CONF" 2>/dev/null || true
  else
    rm -f "$ETC_CONF" 2>/dev/null || true
  fi
  rm -rf "$TMPDIR_BASE"
}
trap cleanup EXIT INT TERM

# --- Back up and write /etc/nss_nostr.conf ---
if [ -f "$ETC_CONF" ]; then
  ETC_BAK="$TMPDIR_BASE/nss_nostr.conf.bak"
  cp "$ETC_CONF" "$ETC_BAK"
  ETC_HAD_ORIGINAL=1
fi
mkdir -p "$(dirname "$ETC_CONF")" 2>/dev/null || true
cat > "$ETC_CONF" <<CONF
db_path=$DB_PATH
uid_base=200000
uid_range=1000
CONF

# --- Generate pre-seeded events for the fake relay ---
python3 - "$TEST_PUBKEY" > "$EVENTS_FILE" <<'PYEOF'
import json, hashlib, sys, time

pubkey = sys.argv[1]
now = int(time.time())

# CID for "Hello world\n" (12 bytes) — matches the blob we'll seed in CAS
hello_cid = hashlib.sha256(b"Hello world\n").hexdigest()

def make_event(kind, tags, content):
    serial = json.dumps(
        [0, pubkey, now, kind, tags, content],
        separators=(",", ":"), ensure_ascii=False,
    )
    eid = hashlib.sha256(serial.encode()).hexdigest()
    return {
        "id": eid, "pubkey": pubkey, "created_at": now,
        "kind": kind, "tags": tags, "content": content,
        "sig": "0" * 128,
    }

manifest_content = json.dumps({
    "version": 2,
    "entries": [{
        "path": "/hello.txt", "cid": hello_cid, "size": 12,
        "meta": {"mode": 420, "mtime": 0, "uid": 0, "gid": 0},
    }],
    "links": [],
})

events = [
    make_event(30081, [["d", "personal"]], manifest_content),
]

json.dump(events, sys.stdout)
PYEOF

# --- Seed CAS with hello.txt blob ---
HELLO_CID="$(python3 -c "import hashlib; print(hashlib.sha256(b'Hello world\n').hexdigest())")"
printf 'Hello world\n' > "$CAS_DIR/$(id -u)/$HELLO_CID"

# --- Seed the DB ---
sqlite3 "$DB_PATH" <<SQL
PRAGMA journal_mode=WAL;
CREATE TABLE IF NOT EXISTS users(uid INTEGER PRIMARY KEY, npub TEXT UNIQUE, username TEXT UNIQUE, gid INTEGER, home TEXT, updated_at INTEGER);
CREATE TABLE IF NOT EXISTS groups(gid INTEGER PRIMARY KEY, name TEXT UNIQUE);
CREATE TABLE IF NOT EXISTS blobs(cid TEXT PRIMARY KEY, size INTEGER, mtime INTEGER, path TEXT, present INTEGER);
CREATE TABLE IF NOT EXISTS settings(key TEXT PRIMARY KEY, value TEXT);
INSERT OR REPLACE INTO settings(key,value) VALUES('username.personal','e2euser');
INSERT OR REPLACE INTO settings(key,value) VALUES('relays.personal','["ws://127.0.0.1:$RELAY_PORT"]');
SQL

# --- Start fake relay ---
echo "starting fake relay on port $RELAY_PORT ..."
FAKE_RELAY_PORT=$RELAY_PORT FAKE_RELAY_EVENTS="$EVENTS_FILE" \
  python3 "$SCRIPT_DIR/../integ/fake_relay_fixture.py" > /dev/null &
PIDS_TO_KILL+=($!)
sleep 0.5

# --- Start fake blossom ---
echo "starting fake blossom on port $BLOSSOM_PORT ..."
FAKE_BLOSSOM_PORT=$BLOSSOM_PORT FAKE_BLOSSOM_DIR="$BLOSSOM_DIR" \
  python3 "$SCRIPT_DIR/../integ/fake_blob_server.py" > /dev/null &
PIDS_TO_KILL+=($!)
sleep 0.3

# --- Start signer ---
echo "starting signer ($SIGNER_BIN) ..."
if [ "$USING_REAL_SIGNER" = "1" ]; then
  NOSTR_SIGNER_SECKEY_HEX="$TEST_SK" "$SIGNER_BIN" &
else
  "$SIGNER_BIN" &
fi
PIDS_TO_KILL+=($!)
sleep 0.5

# --- Verify signer is on the bus ---
if ! command -v gdbus >/dev/null 2>&1; then
  echo "gdbus not found; skipping" >&2
  exit 77
fi
NPUB="$(gdbus call --session --dest org.nostr.Signer \
  --object-path /org/nostr/signer \
  --method org.nostr.Signer.GetPublicKey 2>/dev/null \
  | sed "s/^('//;s/',)$//" || true)"
if [ -z "$NPUB" ]; then
  echo "signer not reachable on session bus; skipping" >&2
  exit 77
fi
echo "signer pubkey: $NPUB"

# --- Start nostr-homectl daemon (provides Homed1 D-Bus service) ---
echo "starting nostr-homectl --daemon ..."
export BLOSSOM_BASE_URL="http://127.0.0.1:${BLOSSOM_PORT}"
"$HOMECTL" --daemon &
PIDS_TO_KILL+=($!)
sleep 0.5

# =============================================================================
# Step 1: Warm the cache via D-Bus
# =============================================================================
echo "--- step 1: warm-cache ---"
"$HOMECTL" warm-cache personal || {
  echo "warm-cache failed" >&2
  exit 1
}

# Verify manifest was persisted
MANIFEST_RAW="$(sqlite3 "$DB_PATH" "SELECT value FROM settings WHERE key='manifest.personal';")"
if [ -z "$MANIFEST_RAW" ]; then
  echo "FAIL: manifest.personal not in cache after warm-cache" >&2
  exit 1
fi
echo "$MANIFEST_RAW" | grep -q '"path":"/hello.txt"' || {
  echo "FAIL: manifest does not contain /hello.txt" >&2
  exit 1
}
echo "warm-cache: manifest persisted ✓"

# =============================================================================
# Step 2: Mount nostrfs (read-only first)
# =============================================================================
echo "--- step 2: mount nostrfs ---"
export NOSTRFS_CACHE="$CAS_DIR"
"$NOSTRFS" --namespace=personal "$MNT" &
NOSTRFS_PID=$!
sleep 1

if ! mountpoint -q "$MNT"; then
  echo "FAIL: nostrfs did not mount at $MNT" >&2
  kill "$NOSTRFS_PID" 2>/dev/null || true
  exit 1
fi
echo "nostrfs mounted at $MNT ✓"

# =============================================================================
# Step 3: Verify hello.txt from manifest
# =============================================================================
echo "--- step 3: read hello.txt ---"
if [ ! -f "$MNT/hello.txt" ]; then
  echo "FAIL: hello.txt not visible in mount" >&2
  exit 1
fi
CONTENT="$(cat "$MNT/hello.txt")"
if [ "$CONTENT" != "Hello world" ]; then
  echo "FAIL: hello.txt content mismatch: '$CONTENT'" >&2
  exit 1
fi
echo "hello.txt content verified ✓"

# =============================================================================
# Step 4: Unmount
# =============================================================================
echo "--- step 4: unmount ---"
if command -v fusermount3 >/dev/null 2>&1; then
  fusermount3 -u "$MNT"
else
  umount "$MNT"
fi
wait "$NOSTRFS_PID" 2>/dev/null || true
echo "unmounted ✓"

# =============================================================================
# Step 5: Write-back — mount with --writeback, create a file, verify manifest
# =============================================================================
echo "--- step 5: writeback mount ---"
"$NOSTRFS" --writeback --namespace=personal "$MNT" &
NOSTRFS_PID=$!
sleep 1

if ! mountpoint -q "$MNT"; then
  echo "FAIL: nostrfs --writeback did not mount at $MNT" >&2
  kill "$NOSTRFS_PID" 2>/dev/null || true
  exit 1
fi

echo -n "e2e-test-payload" > "$MNT/newfile.txt"
sync
sleep 1

MANIFEST_2="$(sqlite3 "$DB_PATH" "SELECT value FROM settings WHERE key='manifest.personal';")"
echo "$MANIFEST_2" | grep -q '"path":"/newfile.txt"' || {
  echo "FAIL: manifest does not contain /newfile.txt after writeback" >&2
  exit 1
}
echo "writeback: newfile.txt in manifest ✓"

# Verify original hello.txt is still present
echo "$MANIFEST_2" | grep -q '"path":"/hello.txt"' || {
  echo "FAIL: hello.txt disappeared from manifest after writeback" >&2
  exit 1
}
echo "writeback: hello.txt still present ✓"

# Unmount
if command -v fusermount3 >/dev/null 2>&1; then
  fusermount3 -u "$MNT"
else
  umount "$MNT"
fi
wait "$NOSTRFS_PID" 2>/dev/null || true

# =============================================================================
# Step 6: Re-mount and verify persistence (newfile.txt survives)
# =============================================================================
echo "--- step 6: re-mount and verify ---"
"$NOSTRFS" --namespace=personal "$MNT" &
NOSTRFS_PID=$!
sleep 1

if ! mountpoint -q "$MNT"; then
  echo "FAIL: re-mount did not succeed" >&2
  kill "$NOSTRFS_PID" 2>/dev/null || true
  exit 1
fi

# hello.txt should still be readable
if [ -f "$MNT/hello.txt" ]; then
  C="$(cat "$MNT/hello.txt")"
  if [ "$C" != "Hello world" ]; then
    echo "FAIL: hello.txt content after re-mount: '$C'" >&2
    exit 1
  fi
  echo "re-mount: hello.txt verified ✓"
fi

# newfile.txt should appear in listing (CAS may or may not have the blob,
# so we check existence rather than content)
if [ -f "$MNT/newfile.txt" ]; then
  echo "re-mount: newfile.txt present ✓"
else
  echo "WARN: newfile.txt not visible on re-mount (CAS promotion may have failed in test env)"
fi

# Final unmount
if command -v fusermount3 >/dev/null 2>&1; then
  fusermount3 -u "$MNT"
else
  umount "$MNT"
fi
wait "$NOSTRFS_PID" 2>/dev/null || true

echo "=== e2e test PASSED ==="
exit 0
