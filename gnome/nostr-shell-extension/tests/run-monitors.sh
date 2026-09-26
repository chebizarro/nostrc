#!/bin/sh
# SPDX-License-Identifier: MIT
#
# Runs tests/test-monitors.js on a private session bus whose only service
# directory holds activatable fakes of org.nostr.SessionRelay1 and
# org.nostr.Wallet1 that leave a marker file if anything ever starts them.
# dbus-run-session tears the bus down when the test exits; no watcher
# processes are left behind (unlike Gio.TestDBus).
#
#   tests/run-monitors.sh [gjs]
set -eu
GJS=${1:-gjs}
here=$(cd "$(dirname "$0")" && pwd)
tmp=$(mktemp -d "${TMPDIR:-/tmp}/nostr-shellext-XXXXXX")
trap 'rm -rf "$tmp"' EXIT INT TERM
mkdir -p "$tmp/services"
for name in org.nostr.SessionRelay1 org.nostr.Wallet1; do
    printf '[D-BUS Service]\nName=%s\nExec=/usr/bin/touch %s/%s.activated\n' \
        "$name" "$tmp" "$name" > "$tmp/services/$name.service"
done
cat > "$tmp/bus.conf" <<CONF
<!DOCTYPE busconfig PUBLIC "-//freedesktop//DTD D-Bus Bus Configuration 1.0//EN"
 "http://www.freedesktop.org/standards/dbus/1.0/busconfig.dtd">
<busconfig>
  <type>session</type>
  <listen>unix:tmpdir=$tmp</listen>
  <auth>EXTERNAL</auth>
  <servicedir>$tmp/services</servicedir>
  <policy context="default">
    <allow send_destination="*" eavesdrop="true"/>
    <allow eavesdrop="true"/>
    <allow own="*"/>
  </policy>
</busconfig>
CONF
cd "$here/.."
NSE_TEST_TMP="$tmp" GIO_USE_VFS=local GSETTINGS_BACKEND=memory \
    dbus-run-session --config-file="$tmp/bus.conf" -- "$GJS" -m tests/test-monitors.js
