#!/bin/sh
# install-user-manifests.sh — per-user install of the org.nostr.signer_bridge
# native-messaging host manifest (nostrc-jjyp).
#
# The Debian/RPM package `nostr-signer-webext-host` already installs the
# system-wide manifests; use this for source builds, for browsers that do not
# read the system dirs, or for Flatpak Firefox.
#
# Usage: install-user-manifests.sh [--host PATH] [--firefox] [--flatpak-firefox]
#                                  [--chromium] [--chrome] [--all] [--uninstall]
#   --host PATH        host binary (default: /usr/libexec/nostr-signer-webext-host)
#   --firefox-id ID    Firefox extension id   (default: signer-bridge@gnostr.org)
#   --chromium-id ID   Chromium extension id  (default: ljigikpdhlameofnnalkmjnbeagdhbin)
# With no browser flag, --firefox --chromium are assumed.
set -eu

NAME=org.nostr.signer_bridge
HOST=/usr/libexec/nostr-signer-webext-host
FIREFOX_ID=signer-bridge@gnostr.org
CHROMIUM_ID=ljigikpdhlameofnnalkmjnbeagdhbin
targets=""
uninstall=0

while [ $# -gt 0 ]; do
  case "$1" in
    --host) HOST=$2; shift ;;
    --firefox-id) FIREFOX_ID=$2; shift ;;
    --chromium-id) CHROMIUM_ID=$2; shift ;;
    --firefox) targets="$targets firefox" ;;
    --flatpak-firefox) targets="$targets flatpak-firefox" ;;
    --chromium) targets="$targets chromium" ;;
    --chrome) targets="$targets chrome" ;;
    --all) targets="firefox flatpak-firefox chromium chrome" ;;
    --uninstall) uninstall=1 ;;
    -h|--help) sed -n '2,16p' "$0"; exit 0 ;;
    *) echo "unknown option: $1" >&2; exit 2 ;;
  esac
  shift
done
[ -n "$targets" ] || targets="firefox chromium"

case "$HOST" in /*) ;; *) echo "--host must be an absolute path" >&2; exit 2 ;; esac
if [ "$uninstall" -eq 0 ] && [ ! -x "$HOST" ]; then
  echo "warning: $HOST is not an executable (yet)" >&2
fi

firefox_manifest() {
  cat <<JSON
{
  "name": "$NAME",
  "description": "Nostr signer bridge: NIP-07 window.nostr -> org.nostr.Signer",
  "path": "$1",
  "type": "stdio",
  "allowed_extensions": ["$FIREFOX_ID"]
}
JSON
}

chromium_manifest() {
  cat <<JSON
{
  "name": "$NAME",
  "description": "Nostr signer bridge: NIP-07 window.nostr -> org.nostr.Signer",
  "path": "$HOST",
  "type": "stdio",
  "allowed_origins": ["chrome-extension://$CHROMIUM_ID/"]
}
JSON
}

place() { # dir generator [arg]
  dir=$1; shift
  if [ "$uninstall" -eq 1 ]; then
    rm -f "$dir/$NAME.json" && echo "removed $dir/$NAME.json"
    return
  fi
  mkdir -p "$dir"
  "$@" > "$dir/$NAME.json"
  echo "installed $dir/$NAME.json"
}

for t in $targets; do
  case "$t" in
    firefox)
      place "$HOME/.mozilla/native-messaging-hosts" firefox_manifest "$HOST" ;;
    flatpak-firefox)
      # Inside the Flatpak sandbox Firefox reads manifests from its own
      # ~/.mozilla, i.e. ~/.var/app/org.mozilla.firefox/.mozilla/ on the
      # host. The binary must be reachable *inside* the sandbox and the app
      # needs session-bus access to the signer:
      #   flatpak override --user --talk-name=org.nostr.Signer org.mozilla.firefox
      # See README.md "Flatpak Firefox" for the portal-based alternative.
      place "$HOME/.var/app/org.mozilla.firefox/.mozilla/native-messaging-hosts" firefox_manifest "$HOST" ;;
    chromium)
      place "$HOME/.config/chromium/NativeMessagingHosts" chromium_manifest ;;
    chrome)
      place "$HOME/.config/google-chrome/NativeMessagingHosts" chromium_manifest ;;
  esac
done
