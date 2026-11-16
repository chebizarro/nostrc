#!/bin/bash
# Exercise an installed fipsd without systemd: run the daemon in the
# foreground from a throwaway config (no TUN, control socket in a temporary
# directory), query it, stop it.
#
#   fipsd/packaging/testing/smoke-direct.sh
#
# For the package jobs that run inside a container (the Fedora rpm build),
# where there is no PID 1 systemd and no /dev/net/tun. It proves the packaged
# binary starts from a config of the shipped shape and answers on its control
# socket; smoke-systemd.sh proves the unit on a real host.
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
work=$(mktemp -d)
pid=""
cleanup() {
    [ -n "$pid" ] && kill "$pid" 2>/dev/null && wait "$pid" 2>/dev/null || true
    rm -rf "$work"
}
trap cleanup EXIT

fipsd --version
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

fipsd --config "$work/fips.yaml" --log-file "$work/fipsd.log" &
pid=$!
if command -v fipsctl >/dev/null 2>&1; then
    for _ in $(seq 1 40); do
        [ -S "$work/control.sock" ] && break
        sleep 0.5
    done
    fipsctl -s "$work/control.sock" status || fipsctl -s "$work/control.sock" show_status
else
    python3 "$here/control-query.py" --socket "$work/control.sock" --retry 20 show_status
fi
kill "$pid"
wait "$pid" || true
pid=""
grep -q 'configuration loaded' "$work/fipsd.log"
echo "smoke-direct: ok"
