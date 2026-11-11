#!/bin/sh
# Run only inside dbus-run-session with disposable state (never the user's bus).
set -eu
: "${GH_CALL_TEST_PRIVATE_BUS:?Set only for a newly created private session bus}"
root=$(mktemp -d "${TMPDIR:-/tmp}/gh-voice-media-secrets.XXXXXX")
keyring=
cleanup() {
  if [ -n "$keyring" ]; then kill "$keyring" 2>/dev/null || true; wait "$keyring" 2>/dev/null || true; fi
  rm -rf "$root"
}
trap cleanup EXIT HUP INT TERM
export HOME="$root/home" XDG_RUNTIME_DIR="$root/runtime" XDG_DATA_HOME="$root/data" XDG_CONFIG_HOME="$root/config"
mkdir -m 700 "$HOME" "$XDG_RUNTIME_DIR" "$XDG_DATA_HOME" "$XDG_CONFIG_HOME"
printf '\n' | gnome-keyring-daemon --foreground --unlock --components=secrets --control-directory="$root/control" >"$root/keyring.log" 2>&1 &
keyring=$!
gdbus wait --session --timeout 10 org.freedesktop.secrets
export GH_CALL_TEST_TURN=1
python3 "$(dirname "$0")/turn_server.py" "$1"
