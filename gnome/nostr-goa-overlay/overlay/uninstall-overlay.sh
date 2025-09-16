#!/usr/bin/env bash
set -euo pipefail

PREFIX="$HOME/.local"

# Remove user service activation
rm -f "$PREFIX/share/dbus-1/services/org.gnome.OnlineAccounts.service" || true

# Optionally remove provider backend and icons (keep by default; uncomment to remove)
# rm -f "$PREFIX/lib/goa-1.0/backends/libgoa-nostr-provider.so" || true
# find "$PREFIX/share/icons/hicolor" -type f -name 'goa-nostr*' -delete || true

# Restart user goa-daemon to fall back to system GOA
if pgrep -u "$USER" -x goa-daemon >/dev/null 2>&1; then
  pkill -u "$USER" -x goa-daemon || true
fi

echo "Uninstalled user-scoped GOA activation; system GOA will be used on next activation."
