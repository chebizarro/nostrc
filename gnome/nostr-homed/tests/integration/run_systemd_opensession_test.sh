#!/usr/bin/env bash
set -euo pipefail

# Preconditions: running inside a systemd-enabled container with --privileged and cgroups mounted.
# This script installs built binaries to /usr/bin, installs unit files,
# seeds minimal warmcache, then runs OpenSession/CloseSession via homectl.

if ! command -v systemctl >/dev/null 2>&1; then
  echo "systemctl not available; skipping" >&2
  exit 0
fi

# Install built binaries
if [ ! -x build/gnome/nostr-homed/nostr-homectl ] || [ ! -x build/gnome/nostr-homed/nostrfs ]; then
  echo "built binaries not found; run build first" >&2
  exit 1
fi
install -D -m 0755 build/gnome/nostr-homed/nostr-homectl /usr/bin/nostr-homectl
install -D -m 0755 build/gnome/nostr-homed/nostrfs /usr/bin/nostrfs

# Install unit files
install -D -m 0644 gnome/nostr-homed/systemd/nostr-homectl.service /etc/systemd/system/nostr-homectl.service
install -D -m 0644 gnome/nostr-homed/systemd/nostrfs@.service /etc/systemd/system/nostrfs@.service
systemctl daemon-reload

# Seed warmcache DB and manifest
ETC_CONF="/etc/nss_nostr.conf"
DB_PATH="/var/lib/nostr-homed/cache.db"
mkdir -p /var/lib/nostr-homed
cat > "$ETC_CONF" <<CONF
db_path=$DB_PATH
uid_base=100000
uid_range=100000
CONF

apt_update_if_needed() {
  if command -v apt-get >/dev/null 2>&1; then apt-get update || true; fi
}

if ! command -v sqlite3 >/dev/null 2>&1; then
  apt_update_if_needed
  if command -v apt-get >/dev/null 2>&1; then apt-get install -y sqlite3 >/dev/null 2>&1 || true; fi
fi

if command -v sqlite3 >/dev/null 2>&1; then
  sqlite3 "$DB_PATH" <<SQL
PRAGMA journal_mode=WAL;
CREATE TABLE IF NOT EXISTS users( uid INTEGER PRIMARY KEY, npub TEXT UNIQUE, username TEXT UNIQUE, gid INTEGER, home TEXT, updated_at INTEGER);
CREATE TABLE IF NOT EXISTS blobs( cid TEXT PRIMARY KEY, size INTEGER, mtime INTEGER, path TEXT, present INTEGER);
CREATE TABLE IF NOT EXISTS settings(key TEXT PRIMARY KEY, value TEXT);
INSERT OR REPLACE INTO settings(key,value) VALUES('warmcache','1');
INSERT OR REPLACE INTO settings(key,value) VALUES('manifest.personal','{"version":2,"entries":[{"path":"/hello.txt","cid":"testcid","size":12,"mode":420,"uid":0,"gid":0,"mtime":0}],"links":[]}');
SQL
else
  echo "sqlite3 CLI not found; skipping DB seed and test" >&2
  exit 0
fi

# Ensure FUSE is present
if [ ! -e /dev/fuse ]; then
  echo "/dev/fuse not present; skipping" >&2
  exit 0
fi

# Seed CAS blob so file is readable without Blossom
CAS_DIR="/var/cache/nostrfs/$(id -u)"
mkdir -p "$CAS_DIR"
printf 'Hello world\n' >"$CAS_DIR/testcid"

# Run OpenSession and check mount
user="runner"
mkdir -p "/home/$user"
nostr-homectl open-session "$user"
sleep 1
if ! mountpoint -q "/home/$user"; then
  echo "mount did not appear at /home/$user" >&2
  exit 1
fi

# Verify hello.txt content
if [ -f "/home/$user/hello.txt" ]; then
  C=$(cat "/home/$user/hello.txt")
  if [ "$C" != "Hello world" ]; then
    echo "unexpected hello.txt content: '$C'" >&2
    exit 1
  fi
fi

# Close session and ensure unmounted
nostr-homectl close-session "$user"
sleep 1
if mountpoint -q "/home/$user"; then
  echo "mount still present after close-session" >&2
  exit 1
fi

exit 0
