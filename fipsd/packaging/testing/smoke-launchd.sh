#!/bin/bash
# Exercise fipsd under launchd on macOS.
#
#   fipsd/packaging/testing/smoke-launchd.sh            # as root: the installed .pkg
#   fipsd/packaging/testing/smoke-launchd.sh BINARY     # as a user: a built fipsd
#
# As root, the installed job (system/com.fips.fipsd, loaded by the package's
# postinstall) is checked: running, control socket in /var/run/fips with the
# fips group and 0770, and a show_status query answered.
#
# As a user, a throwaway plist in the user's launchd domain (gui/<uid>) runs
# BINARY from a temporary config with the TUN off and the control socket in
# a temporary directory -- the shape of the shipped plist, without root or
# the installer. The job is booted out at the end.
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
[ "$(uname -s)" = Darwin ] || { echo "smoke-launchd: macOS only" >&2; exit 2; }

diagnose() {
    # A failed smoke must say why: launchd's view and the daemon's own logs.
    echo "smoke-launchd: diagnostics" >&2
    launchctl print system/com.fips.fipsd 2>&1 | grep -E 'state|pid|last exit|runs|reason' >&2 || true
    for log in /usr/local/var/log/fips/fipsd.log /usr/local/var/log/fips/fipsd.stderr.log; do
        echo "--- $log" >&2
        tail -n 60 "$log" >&2 2>/dev/null || echo "(missing)" >&2
    done
    ls -la /var/run/fips /usr/local/var/log/fips >&2 2>/dev/null || true
}

if [ "$(id -u)" -eq 0 ] && [ $# -eq 0 ]; then
    /usr/local/bin/fipsd --version
    launchctl print system/com.fips.fipsd | grep -E 'state|pid' | head -3
    if command -v fipsctl >/dev/null 2>&1; then
        # Same 20-try bound as control-query.py --retry 20; the socket may not exist yet.
        for try in $(seq 1 20); do
            fipsctl show status && break
            [ "$try" -lt 20 ] || { diagnose; exit 1; }
            sleep 0.5
        done
    else
        python3 "$here/control-query.py" --socket /var/run/fips/control.sock --retry 20 show_status ||
            { diagnose; exit 1; }
    fi
    sock=/var/run/fips/control.sock
    echo "socket: $(stat -f '%Su:%Sg %Lp' "$sock"), dir: $(stat -f '%Su:%Sg %Lp' /var/run/fips)"
    [ "$(stat -f %Sg "$sock")" = fips ] || { echo "smoke-launchd: socket group is not fips" >&2; exit 1; }
    [ "$(stat -f %Lp "$sock")" = 770 ] || { echo "smoke-launchd: socket mode is not 0770" >&2; exit 1; }
    echo "smoke-launchd: ok (system domain)"
    exit 0
fi

binary=${1:?usage: smoke-launchd.sh BINARY (as a user) or no arguments (as root)}
binary=$(cd "$(dirname "$binary")" && pwd)/$(basename "$binary")
work=$(mktemp -d)
label="com.fips.fipsd.smoke.$$"
target="gui/$(id -u)/$label"
cleanup() {
    launchctl bootout "$target" 2>/dev/null || true
    rm -rf "$work"
}
trap cleanup EXIT

"$binary" --version
cat > "$work/fips.yaml" <<YAML
node:
  control:
    socket_path: "$work/control.sock"
tun:
  enabled: false
dns:
  enabled: false
transports:
  udp:
    bind_addr: "127.0.0.1:0"
peers: []
YAML
cat > "$work/$label.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>Label</key><string>$label</string>
    <key>ProgramArguments</key>
    <array>
        <string>$binary</string>
        <string>--config</string><string>$work/fips.yaml</string>
        <string>--log-file</string><string>$work/fipsd.log</string>
    </array>
    <key>RunAtLoad</key><true/>
    <key>KeepAlive</key><dict><key>SuccessfulExit</key><false/></dict>
    <key>StandardOutPath</key><string>/dev/null</string>
    <key>StandardErrorPath</key><string>$work/fipsd.stderr.log</string>
    <key>WorkingDirectory</key><string>$work</string>
</dict>
</plist>
PLIST
plutil -lint "$work/$label.plist"
launchctl bootstrap "gui/$(id -u)" "$work/$label.plist"
launchctl print "$target" | grep -E 'state|pid' | head -3
python3 "$here/control-query.py" --socket "$work/control.sock" --retry 20 show_status
echo "socket: $(stat -f '%Su:%Sg %Lp' "$work/control.sock")"
launchctl bootout "$target"
grep -q 'configuration loaded' "$work/fipsd.log"
echo "smoke-launchd: ok (user domain)"
