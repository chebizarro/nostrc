#!/usr/bin/env bash
set -euo pipefail

if [ $# -lt 1 ]; then
  echo "Usage: $0 <username>" >&2
  exit 2
fi

USER="$1"
: "${HOMED_NAMESPACE:=personal}"
: "${RELAYS_DEFAULT:=wss://relay.damus.io,wss://nostr.wine}"
: "${BLOSSOM_BASE_URL:=https://blossom.example.org}"

# Warm cache (provisions uid/gid, relays, manifest, secrets tmpfs)
./build/gnome/nostr-homectl warm-cache || true

# Open session: mounts /home/$USER via systemd
./build/gnome/nostr-homectl open-session "$USER"

echo "Mounted /home/$USER via nostrfs. To unmount: ./build/gnome/nostr-homectl close-session $USER"
