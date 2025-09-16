#!/usr/bin/env bash
set -euo pipefail

# User-scoped install only; do NOT run with sudo/root
if [ "${EUID:-$(id -u)}" -eq 0 ] || [ -n "${SUDO_USER:-}" ]; then
  echo "Error: install-overlay.sh must be run as your normal user (no sudo)." >&2
  echo "This overlay installs to ~/.local and activates a user D-Bus service." >&2
  exit 1
fi

PREFIX="$HOME/.local"
SRCDIR="$(cd "$(dirname "$0")/.." && pwd)"
PATCH_DIR="$SRCDIR/vendor/patches"
MESON_ARGS=""
PKGCONF_SEED=""
LDLIB_SEED=""
SKIP_VENDOR="1" # deprecated: vendor build removed

# Load user config if present
CONF_FILE="${XDG_CONFIG_HOME:-$HOME/.config}/nostr-goa-overlay/build.conf"
if [ -f "$CONF_FILE" ]; then
  # shellcheck disable=SC1090
  . "$CONF_FILE"
  PREFIX="${PREFIX:-${NOSTR_OVERLAY_PREFIX:-$HOME/.local}}"
  MESON_ARGS="${MESON_ARGS:-${NOSTR_OVERLAY_MESON_ARGS:-}}"
  if [ -n "${NOSTR_OVERLAY_PKG_CONFIG_PATH:-}" ]; then PKGCONF_SEED="${NOSTR_OVERLAY_PKG_CONFIG_PATH}"; fi
  if [ -n "${NOSTR_OVERLAY_LD_LIBRARY_PATH:-}" ]; then LDLIB_SEED="${NOSTR_OVERLAY_LD_LIBRARY_PATH}"; fi
fi

# CLI overrides
while [ $# -gt 0 ]; do
  case "$1" in
    --prefix=*) PREFIX="${1#*=}" ;;
    --meson-args=*) MESON_ARGS="${1#*=}" ;;
    --pkg-config-path=*) PKGCONF_SEED="${1#*=}" ;;
    --ld-library-path=*) LDLIB_SEED="${1#*=}" ;;
    --) shift; break ;;
    *) echo "Unknown arg: $1" >&2; exit 2 ;;
  esac
  shift
done

# Export optional env seeds for pkg-config and runtime libs
if [ -n "$PKGCONF_SEED" ]; then
  export PKG_CONFIG_PATH="$PKGCONF_SEED${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
fi
if [ -n "$LDLIB_SEED" ]; then
  export LD_LIBRARY_PATH="$LDLIB_SEED${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
fi

echo "Using system GOA (no vendoring)." >&2
echo "Tip: If optional UI/docs tools cause build issues, pass extra meson switches via --meson-args, e.g.:" >&2
echo "  ./install-overlay.sh --meson-args='-Ddocumentation=false -Dintrospection=false'" >&2

# Build overlay (provider + overlay files)
pushd "$SRCDIR" >/dev/null
meson setup _build --prefix="$PREFIX" || true
meson compile -C _build
meson install -C _build
popd >/dev/null

# Restart user goa-daemon if running so DBus re-activates our overlay
if pgrep -u "$USER" -x goa-daemon >/dev/null 2>&1; then
  pkill -u "$USER" -x goa-daemon || true
fi

echo "Installed GOA overlay to $PREFIX"
