#!/bin/sh
# Remove fipsd installed by the macOS .pkg: the launchd job, the BLE agent's
# LaunchAgent, the resolver entry, the binaries and the client library. Configuration, identity keys
# and logs are preserved.
#
#   sudo fipsd/packaging/macos/uninstall.sh
set -e

[ "$(id -u)" -eq 0 ] || { echo "uninstall: must run as root (sudo $0)" >&2; exit 1; }

PLIST=/Library/LaunchDaemons/com.fips.fipsd.plist
echo "Uninstalling fipsd..."
launchctl bootout system "$PLIST" 2>/dev/null || true
rm -f "$PLIST"
echo "  Removed $PLIST"

# The BLE agent, in every GUI session.
AGENT=/Library/LaunchAgents/com.fips.ble-agent.plist
for AGENT_USER in $(who | awk '$2 == "console" { print $1 }' | sort -u); do
    [ "$AGENT_USER" = root ] && continue
    AGENT_UID="$(id -u "$AGENT_USER" 2>/dev/null)" || continue
    launchctl bootout "gui/$AGENT_UID/com.fips.ble-agent" 2>/dev/null || true
done
rm -f "$AGENT"
echo "  Removed $AGENT"

rm -f /etc/resolver/fips
dscacheutil -flushcache
killall -HUP mDNSResponder 2>/dev/null || true
echo "  Removed /etc/resolver/fips"

rm -f /usr/local/bin/fipsd /usr/local/bin/fipsctl /usr/local/bin/fips-ble-agent
rm -f /usr/local/lib/libfipsnative.dylib /usr/local/lib/libfipsnative.*.dylib /usr/local/lib/libfipsnative.a
rm -f /usr/local/lib/pkgconfig/libfipsnative.pc /usr/local/include/fips-native-client.h /usr/local/include/fipsnative-version.h
rm -rf /usr/local/lib/fipsd
rm -f /usr/local/share/fips/fips.yaml.example
rmdir /usr/local/share/fips 2>/dev/null || true
rm -f /usr/local/etc/fips/fips.yaml.default /usr/local/etc/fips/hosts.default
rm -rf /var/run/fips
pkgutil --forget com.fips.fipsd.pkg >/dev/null 2>&1 || true
echo "  Removed binaries, library and headers from /usr/local/"
echo ""
echo "fipsd uninstalled."
echo "Config preserved at /usr/local/etc/fips/ (remove it, and the fips group with"
echo "'dscl . -delete /Groups/fips', if you mean it)."
echo "Logs preserved at /usr/local/var/log/fips/."
