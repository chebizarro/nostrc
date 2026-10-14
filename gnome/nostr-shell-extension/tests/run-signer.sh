#!/bin/sh
# SPDX-License-Identifier: MIT
set -eu
GJS=${1:-gjs}
here=$(cd "$(dirname "$0")" && pwd)
tmp=$(mktemp -d "${TMPDIR:-/tmp}/nostr-signer-test-XXXXXX")
trap 'rm -rf "$tmp"' EXIT INT TERM
mkdir -p "$tmp/services"
printf '[D-BUS Service]\nName=org.nostr.Signer\nExec=/usr/bin/touch %s/activated\n' \
    "$tmp" > "$tmp/services/org.nostr.Signer.service"
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
    dbus-run-session --config-file="$tmp/bus.conf" -- "$GJS" -m tests/test-signer.js
