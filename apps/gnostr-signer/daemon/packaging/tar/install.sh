#!/usr/bin/env bash
set -euo pipefail

APP=gnostr-signer-daemon
PREFIX_DEFAULT="$HOME/.local"
PREFIX="${1:-$PREFIX_DEFAULT}"

BIN_DST="$PREFIX/bin"
UNIT_DST="$HOME/.config/systemd/user"

mkdir -p "$BIN_DST" "$UNIT_DST"

# Locate script dir
SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" &>/dev/null && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"

# Copy binary and user unit
install -m 0755 "$ROOT_DIR/bin/$APP" "$BIN_DST/$APP"
install -m 0644 "$ROOT_DIR/share/systemd/user/$APP.service" "$UNIT_DST/$APP.service"

# Ensure runtime dir socket subdir exists at start
mkdir -p "$XDG_RUNTIME_DIR/gnostr" 2>/dev/null || true

systemctl --user daemon-reload || true
systemctl --user enable --now "$APP.service" || true

echo "Installed $APP to $BIN_DST and enabled user service."
echo "Endpoint defaults to unix:%t/gnostr/signer.sock (see service Environment)."
