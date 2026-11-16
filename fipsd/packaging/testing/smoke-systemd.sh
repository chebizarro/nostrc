#!/bin/bash
# Exercise an installed fipsd package under systemd: start the unit, query the
# control socket, check the socket's group and mode, stop it.
#
#   fipsd/packaging/testing/smoke-systemd.sh [--keep-running]
#
# Run as root on a host whose systemd is PID 1 (a VM or the CI runner itself;
# not a plain container, see smoke-direct.sh for that). The packages run this
# between install and uninstall in .github/workflows/fips-packaging.yml.
#
# Uses fipsctl when the package installed one, else the control-query.py next
# to this script, which speaks the same one-line JSON protocol.
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
keep=0
[ "${1:-}" = "--keep-running" ] && keep=1

[ "$(id -u)" -eq 0 ] || { echo "smoke-systemd: run as root" >&2; exit 2; }
[ -d /run/systemd/system ] || { echo "smoke-systemd: systemd is not running here" >&2; exit 2; }

fipsd --version
getent group fips >/dev/null || { echo "smoke-systemd: the fips group is missing" >&2; exit 1; }
[ -f /etc/fips/fips.yaml ] || { echo "smoke-systemd: /etc/fips/fips.yaml was not seeded" >&2; exit 1; }
[ "$(stat -c %a /etc/fips/fips.yaml)" = 600 ] || { echo "smoke-systemd: fips.yaml is not mode 0600" >&2; exit 1; }
systemctl is-enabled fipsd.service

systemctl start fipsd.service
# Type=simple reads active the instant the fork happens; give a failing exec
# a moment to show, then query.
sleep 2
systemctl is-active fipsd.service || { journalctl -u fipsd.service --no-pager -n 50; exit 1; }

if command -v fipsctl >/dev/null 2>&1; then
    fipsctl status || fipsctl show_status
else
    python3 "$here/control-query.py" --retry 20 show_status
fi

sock=/run/fips/control.sock
[ -S "$sock" ] || { echo "smoke-systemd: $sock is not a socket" >&2; exit 1; }
echo "socket: $(stat -c '%U:%G %a' "$sock"), dir: $(stat -c '%U:%G %a' /run/fips)"
[ "$(stat -c %G "$sock")" = fips ] || { echo "smoke-systemd: socket group is not fips" >&2; exit 1; }
[ "$(stat -c %a "$sock")" = 770 ] || { echo "smoke-systemd: socket mode is not 0770" >&2; exit 1; }
[ "$(stat -c '%G %a' /run/fips)" = "fips 750" ] || { echo "smoke-systemd: /run/fips is not fips 0750" >&2; exit 1; }

# The firewall unit is opt-in and must at least parse; it is not started here
# because nft rules change the host.
systemd-analyze verify /usr/lib/systemd/system/fips-firewall.service 2>&1 | grep -v 'Unit is bound' || true

if [ "$keep" -eq 0 ]; then
    systemctl stop fipsd.service
    systemctl is-active fipsd.service && exit 1
fi
echo "smoke-systemd: ok"
