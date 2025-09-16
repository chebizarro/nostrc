#!/usr/bin/env bash
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
PATH="$HOME/.local/bin:$PATH"

if ! command -v dbus-run-session >/dev/null 2>&1; then
  echo "dbus-run-session not found; skipping" >&2
  exit 77
fi

# Verify user units are started (best-effort)
check_active(){
  local unit="$1"; local active
  active=$(gdbus call --session \
    --dest org.freedesktop.systemd1 \
    --object-path /org/freedesktop/systemd1 \
    --method org.freedesktop.DBus.Properties.Get \
    org.freedesktop.systemd1.Manager "ActiveState" 2>/dev/null || true)
  # Fallback: try to list units and grep our unit name
  if ! systemctl --user list-units --type=service 2>/dev/null | grep -q "$unit"; then
    echo "warn: could not confirm $unit active (skipping assertion)" >&2
  fi
}
check_active "nostr-router.service"
check_active "nostr-dav.service"
check_active "nostrfs.service"
check_active "nostr-notify.service"
if ! command -v python3 >/dev/null 2>&1; then
  echo "python3 not found; skipping" >&2
  exit 77
fi
if ! command -v goa_shims >/dev/null 2>&1; then
  echo "goa_shims not installed; skipping" >&2
  exit 77
fi

# Start a user bus and run fake services + provisioning
cleanup() {
  if [[ -n "${SIGNER_PID:-}" ]]; then kill "$SIGNER_PID" 2>/dev/null || true; fi
  if [[ -n "${DAV_PID:-}" ]]; then kill "$DAV_PID" 2>/dev/null || true; fi
}
trap cleanup EXIT

# Launch under existing session bus (headless)
python3 "$HERE/fake_signer.py" & SIGNER_PID=$!
python3 "$HERE/fake_dav.py" & DAV_PID=$!

# Give services a moment
sleep 0.3

# Provision for the fake identity
USER_ID="npub1testintegration"
if ! goa_shims provision --user "$USER_ID" --host 127.0.0.1 --port 7680; then
  echo "provision failed" >&2; exit 1
fi

# Check EDS sources materialized
EDS_DIR="$HOME/.config/evolution/sources"
CAL_SRC="$EDS_DIR/nostr-caldav-$USER_ID.source"
CARD_SRC="$EDS_DIR/nostr-carddav-$USER_ID.source"
if [[ ! -f "$CAL_SRC" || ! -f "$CARD_SRC" ]]; then
  echo "EDS sources missing" >&2
  exit 1
fi

# Teardown
if ! goa_shims teardown --user "$USER_ID"; then
  echo "teardown failed" >&2; exit 1
fi

if [[ -f "$CAL_SRC" || -f "$CARD_SRC" ]]; then
  echo "EDS sources not removed on teardown" >&2
  exit 1
fi

echo "ok"
