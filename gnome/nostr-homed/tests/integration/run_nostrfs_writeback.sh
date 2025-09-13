#!/usr/bin/env bash
set -euo pipefail

# This test exercises nostrfs write-back path: create/write/flush/commit via Blossom and CAS promotion,
# and manifest persistence to settings.manifest.personal.

if ! command -v nostrfs >/dev/null 2>&1; then
  echo "nostrfs not found; skipping write-back integration test" >&2
  exit 0
fi

if [ ! -e /dev/fuse ]; then
  echo "/dev/fuse not present; skipping write-back integration test" >&2
  exit 0
fi

MNT="$(mktemp -d /tmp/nostrfs-wb.XXXXXX)"
ETC_CONF="/etc/nss_nostr.conf"
DB_PATH="/tmp/nostr_cache.db"
CAS_DIR="/var/cache/nostrfs/$(id -u)"
TMP_DIR="$CAS_DIR/tmp"
mkdir -p "$(dirname "$ETC_CONF")" || true
mkdir -p "$CAS_DIR" "$TMP_DIR"
cat > "$ETC_CONF" <<CONF
db_path=$DB_PATH
uid_base=100000
uid_range=100000
CONF

if command -v sqlite3 >/dev/null 2>&1; then
  sqlite3 "$DB_PATH" <<SQL
PRAGMA journal_mode=WAL;
CREATE TABLE IF NOT EXISTS users( uid INTEGER PRIMARY KEY, npub TEXT UNIQUE, username TEXT UNIQUE, gid INTEGER, home TEXT, updated_at INTEGER);
CREATE TABLE IF NOT EXISTS blobs( cid TEXT PRIMARY KEY, size INTEGER, mtime INTEGER, path TEXT, present INTEGER);
CREATE TABLE IF NOT EXISTS settings(key TEXT PRIMARY KEY, value TEXT);
INSERT OR REPLACE INTO settings(key,value) VALUES('warmcache','1');
-- start with empty manifest
INSERT OR REPLACE INTO settings(key,value) VALUES('manifest.personal','{"version":2,"entries":[],"links":[]}');
SQL
else
  echo "sqlite3 CLI not found; skipping write-back integration test" >&2
  exit 0
fi

cleanup() {
  set +e
  if mountpoint -q "$MNT"; then
    if command -v fusermount3 >/dev/null 2>&1; then
      fusermount3 -u "$MNT" || true
    else
      umount "$MNT" || true
    fi
  fi
  rmdir "$MNT" 2>/dev/null || true
}
trap cleanup EXIT INT TERM

# Prefer not to actually publish to public relays during integration
export RELAYS_DEFAULT="wss://127.0.0.1:65535"
# Point blossom to a non-routable host so upload path is exercised but won't contact public infra in CI
export BLOSSOM_BASE_URL="http://127.0.0.1:65535"

# Start nostrfs in background with write-back enabled if supported
nostrfs --writeback "$MNT" &
PID=$!
sleep 1

if ! mountpoint -q "$MNT"; then
  echo "nostrfs did not mount at $MNT" >&2
  kill "$PID" 2>/dev/null || true
  exit 1
fi

# Create a new file and write content
TEST_FILE="$MNT/new.txt"
echo -n "hello writeback" > "$TEST_FILE"
# Ensure flush by closing descriptor (echo already does). Optionally sync
sync

# Give actor a moment to process commit
sleep 1

# Check manifest persisted and contains /new.txt
MANIFEST_JSON="$(sqlite3 "$DB_PATH" "select value from settings where key='manifest.personal';")"
echo "$MANIFEST_JSON" | grep -q '"path":"/new.txt"' || { echo "manifest did not include /new.txt" >&2; exit 1; }

# Extract cid for /new.txt (simple jq-less parse using sed/awk)
CID=$(echo "$MANIFEST_JSON" | sed -n 's/.*"path":"\/new.txt"[^}]*"cid":"\([^"]\+\)".*/\1/p' | head -n1)
if [ -z "$CID" ]; then
  echo "failed to extract CID for /new.txt from manifest" >&2
  exit 1
fi

# Verify CAS file exists or tmp file was promoted (best-effort; upload may fail in CI)
if [ -f "$CAS_DIR/$CID" ]; then
  echo "CAS object present: $CAS_DIR/$CID"
else
  echo "CAS object not present (this may be expected if BLOSSOM_BASE_URL is unreachable): $CAS_DIR/$CID" >&2
fi

# Unmount
if command -v fusermount3 >/dev/null 2>&1; then
  fusermount3 -u "$MNT"
else
  umount "$MNT"
fi

wait "$PID" 2>/dev/null || true
exit 0
