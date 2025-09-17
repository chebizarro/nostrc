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
VENDOR_DIR="$SRCDIR/vendor/gnome-online-accounts"
PATCH_DIR="$SRCDIR/vendor/patches"
MESON_ARGS=""
PKGCONF_SEED=""
LDLIB_SEED=""

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

echo "Building vendored GOA with in-tree Nostr provider." >&2
echo "Tip: pass extra meson switches via --meson-args, e.g.:" >&2
echo "  ./install-overlay.sh --meson-args='-Ddocumentation=false -Dintrospection=false'" >&2

# 1) Build vendored GOA into ~/.local
if [ ! -d "$VENDOR_DIR" ]; then
  echo "Error: vendor GOA not found at $VENDOR_DIR" >&2
  exit 2
fi
pushd "$VENDOR_DIR" >/dev/null
# Disable man pages (avoids xsltproc fetching remote docbook XSL). Use system deps only.
# Purge any previously fetched subprojects/wraps and wipe build dir to avoid
# cached wrap-mode=forcefallback.
unset MESON_FORCE_FALLBACK_FOR || true
rm -rf subprojects _build
MESON_WRAP_MODE=default meson setup _build --prefix="$PREFIX" $MESON_ARGS -Dman=false --wrap-mode=default
meson compile -C _build
meson install -C _build
popd >/dev/null

# 2) Install user-scoped DBus service for org.gnome.OnlineAccounts
DBUS_USER_SERVICES_DIR="$HOME/.local/share/dbus-1/services"
mkdir -p "$DBUS_USER_SERVICES_DIR"
SERVICE_FILE="$DBUS_USER_SERVICES_DIR/org.gnome.OnlineAccounts.service"
cat >"$SERVICE_FILE" <<EOF
[D-BUS Service]
Name=org.gnome.OnlineAccounts
Exec=$PREFIX/libexec/goa-daemon
SystemdService=
EOF
echo "Wrote $SERVICE_FILE"

# 3) Build overlay bits (icons, docs) if any
pushd "$SRCDIR" >/dev/null
meson setup _build --prefix="$PREFIX" || true
meson compile -C _build || true
meson install -C _build || true
popd >/dev/null

# Restart user goa-daemon if running so DBus re-activates our overlay
pkill -u "$USER" -x goa-daemon >/dev/null 2>&1 || true

echo "Installed vendored GOA + Nostr provider to $PREFIX"
