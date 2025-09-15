#!/usr/bin/env bash
set -euo pipefail

# Minimal warm-cache smoke test that avoids external network and signer deps.
# Seeds DB with username and manifest, runs warm-cache, and asserts warmcache=1.

if [ ! -x build/gnome/nostr-homed/nostr-homectl ]; then
  echo "nostr-homectl not built; skipping" >&2
  exit 0
fi

ETC_CONF="/etc/nss_nostr.conf"
DB_DIR="/tmp/nostr-homed-smoke"
DB_PATH="$DB_DIR/cache.db"
mkdir -p "$DB_DIR"
mkdir -p "$(dirname "$ETC_CONF")" || true
cat > "$ETC_CONF" <<CONF
db_path=$DB_PATH
uid_base=200000
uid_range=1000
CONF

if ! command -v sqlite3 >/dev/null 2>&1; then
  echo "sqlite3 not available; skipping warm-cache smoke" >&2
  exit 0
fi

sqlite3 "$DB_PATH" <<SQL
PRAGMA journal_mode=WAL;
CREATE TABLE IF NOT EXISTS users( uid INTEGER PRIMARY KEY, npub TEXT UNIQUE, username TEXT UNIQUE, gid INTEGER, home TEXT, updated_at INTEGER);
CREATE TABLE IF NOT EXISTS groups( gid INTEGER PRIMARY KEY, name TEXT UNIQUE);
CREATE TABLE IF NOT EXISTS blobs( cid TEXT PRIMARY KEY, size INTEGER, mtime INTEGER, path TEXT, present INTEGER);
CREATE TABLE IF NOT EXISTS settings(key TEXT PRIMARY KEY, value TEXT);
-- seed username and a trivial manifest
INSERT OR REPLACE INTO settings(key,value) VALUES('username.personal','ciuser');
INSERT OR REPLACE INTO settings(key,value) VALUES('manifest.personal','{"version":2,"entries":[],"links":[]}');
SQL

# Run warm-cache with env overrides that avoid network/publish
export RELAYS_DEFAULT="wss://127.0.0.1:65535"
export BLOSSOM_BASE_URL="http://127.0.0.1:65535"
export HOMED_NAMESPACE="personal"
export HOMED_USERNAME="ciuser"

./build/gnome/nostr-homectl warm-cache || true

# Assert warmcache setting is present
WARM=$(sqlite3 "$DB_PATH" "select value from settings where key='warmcache';")
if [ "$WARM" != "1" ]; then
  echo "warmcache flag not set" >&2
  exit 1
fi

echo "run_warmcache_smoke: ok"
exit 0
