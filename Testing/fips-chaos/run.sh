#!/usr/bin/env bash
# Root wrapper for the netns/veth chaos harness (Testing/fips-chaos/README.md).
#
#   sudo Testing/fips-chaos/run.sh --fipsd build/fipsd/fipsd [--rust /path/to/fips] [chaos.py options]
#
# Checks the environment, runs chaos.py and, whatever happens, purges the
# namespaces the run created (chaos.py tears them down itself; the trap is the
# second line of defence for a killed driver).
set -euo pipefail
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
if [ "$(id -u)" -ne 0 ]; then
    echo "run.sh: must run as root (ip netns, nftables, tc)" >&2
    exit 2
fi
for tool in ip nft tc python3; do
    command -v "$tool" >/dev/null 2>&1 || { echo "run.sh: missing required tool: $tool" >&2; exit 2; }
done
command -v iperf3 >/dev/null 2>&1 || echo "run.sh: iperf3 not found; the netem scenario runs without background load" >&2
command -v conntrack >/dev/null 2>&1 || echo "run.sh: conntrack not found; NAT punch stage evidence will be unavailable" >&2
cleanup() { python3 "$here/chaos.py" --purge >/dev/null 2>&1 || true; }
trap cleanup EXIT
python3 "$here/chaos.py" "$@"
