#!/usr/bin/env bash
set -euo pipefail

PREFIX="$HOME/.local"
SRCDIR="$(cd "$(dirname "$0")/.." && pwd)"
PATCH_DIR="$SRCDIR/vendor/patches"

# Build vendor GOA and provider
if [ ! -d "$SRCDIR/vendor/gnome-online-accounts" ]; then
  echo "Vendor GOA dir missing; cloning GOA 46..." >&2
  mkdir -p "$SRCDIR/vendor"
  git clone --depth 1 --branch 46 https://gitlab.gnome.org/GNOME/gnome-online-accounts.git "$SRCDIR/vendor/gnome-online-accounts"
fi

# Apply provider patch if not already applied
if [ -d "$SRCDIR/vendor/gnome-online-accounts/.git" ]; then
  pushd "$SRCDIR/vendor/gnome-online-accounts" >/dev/null
  if ! git log --oneline | grep -q "add Nostr provider (user overlay)"; then
    echo "Applying Nostr provider patch to vendor GOA..." >&2
    git am "$PATCH_DIR/0001-add-nostr-provider.patch" || {
      echo "Patch did not apply; trying 'git apply' as a fallback" >&2
      git apply "$PATCH_DIR/0001-add-nostr-provider.patch" || true
    }
  fi
  popd >/dev/null
fi

# Build vendor GOA (minimal)
pushd "$SRCDIR/vendor/gnome-online-accounts" >/dev/null
meson setup _build --prefix="$PREFIX" || true
meson compile -C _build
meson install -C _build
popd >/dev/null

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
