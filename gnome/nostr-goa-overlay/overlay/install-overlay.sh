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
GOA_DIR="$SRCDIR/vendor/gnome-online-accounts"
GOA_TAG_PREF=""
MESON_ARGS=""
PKGCONF_SEED=""
LDLIB_SEED=""
GOA_REF_OVERRIDE=""
GOA_REF_PATTERN=""
SKIP_PATCH=""
SKIP_VENDOR=""

# Load user config if present
CONF_FILE="${XDG_CONFIG_HOME:-$HOME/.config}/nostr-goa-overlay/build.conf"
if [ -f "$CONF_FILE" ]; then
  # shellcheck disable=SC1090
  . "$CONF_FILE"
  PREFIX="${PREFIX:-${NOSTR_OVERLAY_PREFIX:-$HOME/.local}}"
  MESON_ARGS="${MESON_ARGS:-${NOSTR_OVERLAY_MESON_ARGS:-}}"
  if [ -n "${NOSTR_OVERLAY_GOA_SRC:-}" ]; then GOA_DIR="${NOSTR_OVERLAY_GOA_SRC}"; fi
  if [ -n "${NOSTR_OVERLAY_GOA_TAG:-}" ]; then GOA_TAG_PREF="${NOSTR_OVERLAY_GOA_TAG}"; fi
  if [ -n "${NOSTR_OVERLAY_PKG_CONFIG_PATH:-}" ]; then PKGCONF_SEED="${NOSTR_OVERLAY_PKG_CONFIG_PATH}"; fi
  if [ -n "${NOSTR_OVERLAY_LD_LIBRARY_PATH:-}" ]; then LDLIB_SEED="${NOSTR_OVERLAY_LD_LIBRARY_PATH}"; fi
  if [ -n "${NOSTR_OVERLAY_GOA_REF:-}" ]; then GOA_REF_OVERRIDE="${NOSTR_OVERLAY_GOA_REF}"; fi
  if [ -n "${NOSTR_OVERLAY_GOA_REF_PATTERN:-}" ]; then GOA_REF_PATTERN="${NOSTR_OVERLAY_GOA_REF_PATTERN}"; fi
  if [ -n "${NOSTR_OVERLAY_SKIP_PATCH:-}" ]; then SKIP_PATCH="${NOSTR_OVERLAY_SKIP_PATCH}"; fi
  if [ -n "${NOSTR_OVERLAY_SKIP_VENDOR:-}" ]; then SKIP_VENDOR="${NOSTR_OVERLAY_SKIP_VENDOR}"; fi
fi

# CLI overrides
while [ $# -gt 0 ]; do
  case "$1" in
    --prefix=*) PREFIX="${1#*=}" ;;
    --goa-src=*) GOA_DIR="${1#*=}" ;;
    --goa-tag=*) GOA_TAG_PREF="${1#*=}" ;;
    --meson-args=*) MESON_ARGS="${1#*=}" ;;
    --pkg-config-path=*) PKGCONF_SEED="${1#*=}" ;;
    --ld-library-path=*) LDLIB_SEED="${1#*=}" ;;
    --goa-ref=*) GOA_REF_OVERRIDE="${1#*=}" ;;
    --goa-ref-pattern=*) GOA_REF_PATTERN="${1#*=}" ;;
    --skip-patch) SKIP_PATCH=1 ;;
    --skip-vendor) SKIP_VENDOR=1 ;;
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

if [ -z "$SKIP_VENDOR" ]; then
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

  # Select a GOA ref based on overrides, host, or best-available tags
  if [ -d "$SRCDIR/vendor/gnome-online-accounts/.git" ]; then
    pushd "$SRCDIR/vendor/gnome-online-accounts" >/dev/null
    git fetch --tags --quiet || true
    # Determine default pattern from host if not provided
    if [ -z "$GOA_REF_OVERRIDE" ] && [ -z "$GOA_TAG_PREF" ] && [ -z "$GOA_REF_PATTERN" ] && [ -f /etc/os-release ]; then
      . /etc/os-release || true
      case "${ID:-}-${VERSION_ID:-}" in
        ubuntu-24.*) GOA_REF_PATTERN="GNOME_46*" ;;
        fedora-40*) GOA_REF_PATTERN="GNOME_46*" ;;
        arch-*)     GOA_REF_PATTERN="" ;; # rolling, leave empty to choose latest
        *)          GOA_REF_PATTERN="GNOME_46*" ;;
      esac
      arch-*)     GOA_REF_PATTERN="" ;; # rolling, leave empty to choose latest
      *)          GOA_REF_PATTERN="GNOME_46*" ;;
    esac
  fi

  # Priority: explicit ref > explicit tag > pattern > best-known 46 patterns > latest tag
  if [ -n "$GOA_REF_OVERRIDE" ]; then
    echo "Checking out GOA ref $GOA_REF_OVERRIDE" >&2
    git checkout -f "$GOA_REF_OVERRIDE" --quiet || true
  else
    TAG="$GOA_TAG_PREF"
    if [ -z "$TAG" ] && [ -n "$GOA_REF_PATTERN" ]; then
      TAG="$(git tag -l "$GOA_REF_PATTERN" | sort -V | tail -n1)"
    fi
    if [ -z "$TAG" ]; then
      TAG="$(git tag -l 'GNOME_46*' | sort -V | tail -n1)"
      [ -z "$TAG" ] && TAG="$(git tag -l 'v46*' | sort -V | tail -n1)"
      [ -z "$TAG" ] && TAG="$(git tag -l '46*' | sort -V | tail -n1)"
    fi
    if [ -z "$TAG" ]; then
      # Fallback to latest tag overall
      TAG="$(git tag -l | sort -V | tail -n1)"
    fi
    if [ -n "$TAG" ]; then
      echo "Checking out GOA tag $TAG" >&2
      git checkout -f "tags/$TAG" --quiet || true
    else
      echo "Warning: Could not find any GOA tag; staying on default branch." >&2
    fi
  fi
    popd >/dev/null
  fi

########################################
# Apply provider patch if not already applied
  if [ -d "$SRCDIR/vendor/gnome-online-accounts/.git" ]; then
    pushd "$SRCDIR/vendor/gnome-online-accounts" >/dev/null
    if [ -n "$SKIP_PATCH" ]; then
      echo "Skipping vendor patch per --skip-patch/NOSTR_OVERLAY_SKIP_PATCH. Relying on runtime backend discovery." >&2
    elif ! git log --oneline | grep -q "add Nostr provider (user overlay)"; then
      echo "Applying Nostr provider patch to vendor GOA..." >&2
    # Clean up any previous failed 'git am'
    if [ -d .git/rebase-apply ] || [ -d .git/rebase-merge ]; then
      git am --abort >/dev/null 2>&1 || true
      rm -rf .git/rebase-apply .git/rebase-merge || true
    fi
    # Try mbox import with 3-way merge
    if ! git am --3way "$PATCH_DIR/0001-add-nostr-provider.patch"; then
      echo "git am failed; attempting 'git apply -p1' fallback" >&2
      git am --abort >/dev/null 2>&1 || true
      rm -rf .git/rebase-apply .git/rebase-merge || true
      if ! git apply -p1 "$PATCH_DIR/0001-add-nostr-provider.patch"; then
        echo "ERROR: Could not apply provider patch to GOA.\n" \
             "Try pinning a specific ref that matches the patch via:\n" \
             "  ./install-overlay.sh --goa-ref=GNOME_46_2\n" \
             "or provide your own GOA source with --goa-src. Exiting." >&2
        exit 2
      fi
    fi
  fi
  popd >/dev/null
fi

echo "Note: If Meson later fails pulling GTK subprojects or due to version constraints,\n" \
     "you can pass extra meson switches via --meson-args to disable UI bits, e.g.:\n" \
     "  ./install-overlay.sh --meson-args='-Dgtk=false -Dgtk_doc=false'\n" >&2

  # Build vendor GOA (minimal)
  pushd "$SRCDIR/vendor/gnome-online-accounts" >/dev/null
  # Clean previous failed or incompatible build dir
  if [ -d _build ]; then rm -rf _build; fi
  meson setup _build --prefix="$PREFIX" --wrap-mode=nodownload $MESON_ARGS
  meson compile -C _build
  meson install -C _build
  popd >/dev/null
else
  echo "Skipping vendor GOA build per --skip-vendor/NOSTR_OVERLAY_SKIP_VENDOR. Using system GOA." >&2
fi

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
