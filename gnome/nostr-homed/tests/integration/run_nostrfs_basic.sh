#!/usr/bin/env bash
set -euo pipefail

if ! command -v nostrfs >/dev/null 2>&1; then
  echo "nostrfs not found; skipping integration test" >&2
  exit 0
fi

if [ ! -e /dev/fuse ]; then
  echo "/dev/fuse not present; skipping integration test" >&2
  exit 0
fi

MNT="$(mktemp -d /tmp/nostrfs.XXXXXX)"
ETC_CONF="/etc/nss_nostr.conf"
DB_PATH="/tmp/nostr_cache.db"
CAS_DIR="/var/cache/nostrfs/$(id -u)"
mkdir -p "$(dirname "$ETC_CONF")" || true
mkdir -p "$CAS_DIR"
cat > "$ETC_CONF" <<CONF
db_path=$DB_PATH
uid_base=100000
uid_range=100000
CONF

if command -v sqlite3 >/dev/null 2>&1; then
  # Initialize DB and insert a manifest and warmcache flag
  sqlite3 "$DB_PATH" <<SQL
PRAGMA journal_mode=WAL;
CREATE TABLE IF NOT EXISTS users( uid INTEGER PRIMARY KEY, npub TEXT UNIQUE, username TEXT UNIQUE, gid INTEGER, home TEXT, updated_at INTEGER);
CREATE TABLE IF NOT EXISTS blobs( cid TEXT PRIMARY KEY, size INTEGER, mtime INTEGER, path TEXT, present INTEGER);
CREATE TABLE IF NOT EXISTS settings(key TEXT PRIMARY KEY, value TEXT);
INSERT OR REPLACE INTO settings(key,value) VALUES('warmcache','1');
INSERT OR REPLACE INTO settings(key,value) VALUES('manifest.personal','{"version":2,"entries":[{"path":"/hello.txt","cid":"testcid","size":12,"mode":420,"uid":0,"gid":0,"mtime":0}],"links":[]}');
SQL
else
  echo "sqlite3 CLI not found; skipping integration test" >&2
  exit 0
fi

# Seed CAS with blob for cid=testcid (12 bytes)
printf 'Hello world\n' >"$CAS_DIR/testcid"
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

# Start nostrfs in background
nostrfs "$MNT" &
PID=$!
# Give it some time to mount
sleep 1

# Verify mount and basic operations
if ! mountpoint -q "$MNT"; then
  echo "nostrfs did not mount at $MNT" >&2
  kill "$PID" 2>/dev/null || true
  exit 1
fi

# List directory and read README.txt and hello.txt
ls -la "$MNT" || true
if [ -f "$MNT/README.txt" ]; then
  head -n 1 "$MNT/README.txt" || true
fi
if [ -f "$MNT/hello.txt" ]; then
  CONTENT="$(cat "$MNT/hello.txt")"
  if [ "$CONTENT" != "Hello world" ]; then
    echo "Unexpected content for hello.txt: '$CONTENT'" >&2
    exit 1
  fi
fi

# Unmount via fusermount3 if available
if command -v fusermount3 >/dev/null 2>&1; then
  fusermount3 -u "$MNT"
else
  umount "$MNT"
fi

wait "$PID" 2>/dev/null || true
exit 0
