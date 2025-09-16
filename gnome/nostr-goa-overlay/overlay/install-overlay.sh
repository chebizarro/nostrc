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

# Build vendor GOA and provider
if [ -d "$SRCDIR/.git" ] && git -C "$SRCDIR" config --file .gitmodules --name-only --get-regexp \
    '^submodule\.gnome/nostr-goa-overlay/vendor/gnome-online-accounts\.' >/dev/null 2>&1; then
  echo "Initializing vendor GOA submodule..." >&2
  git -C "$SRCDIR" submodule update --init --recursive gnome/nostr-goa-overlay/vendor/gnome-online-accounts || true
fi

if [ ! -d "$SRCDIR/vendor/gnome-online-accounts" ] || [ -z "$(ls -A "$SRCDIR/vendor/gnome-online-accounts" 2>/dev/null)" ]; then
  echo "Vendor GOA dir missing or empty; cloning upstream GOA..." >&2
  mkdir -p "$SRCDIR/vendor"
  git clone https://gitlab.gnome.org/GNOME/gnome-online-accounts.git "$SRCDIR/vendor/gnome-online-accounts"
fi

# Checkout a GOA 46.x series tag if possible
if [ -d "$SRCDIR/vendor/gnome-online-accounts/.git" ]; then
  pushd "$SRCDIR/vendor/gnome-online-accounts" >/dev/null
  git fetch --tags --quiet || true
  # Prefer tags like GNOME_46*, otherwise try v46.* or 46.* patterns
  TAG="$(git tag -l 'GNOME_46*' | sort -V | tail -n1)"
  if [ -z "$TAG" ]; then TAG="$(git tag -l 'v46*' | sort -V | tail -n1)"; fi
  if [ -z "$TAG" ]; then TAG="$(git tag -l '46*' | sort -V | tail -n1)"; fi
  if [ -n "$TAG" ]; then
    echo "Checking out GOA tag $TAG" >&2
    git checkout -f "tags/$TAG" --quiet || true
  else
    echo "Warning: Could not find a GOA 46.x tag; staying on default branch." >&2
  fi
  popd >/dev/null
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
