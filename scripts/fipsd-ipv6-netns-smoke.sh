#!/usr/bin/env bash
# Two fipsd daemons with REAL TUN devices in two Linux network namespaces,
# joined by a veth pair over UDP, pinging each other's fd00::/8 address
# through the IPv6 shim (nostrc-9nxwk.2) and, when iperf3 is installed,
# moving TCP across. Linux only, needs root (ip netns, TUN). Opt-in: not
# registered with ctest.
#
#   sudo scripts/fipsd-ipv6-netns-smoke.sh build/fipsd/fipsd
#
# Exit 0 when both pings succeed (and iperf3, when present, completes);
# the daemons' logs stay under the temporary directory on failure.
set -euo pipefail
FIPSD=${1:?usage: $0 <path to fipsd>}
[[ $(uname -s) == Linux ]] || { echo "SKIP: Linux network namespaces required"; exit 77; }
[[ $(id -u) == 0 ]] || { echo "SKIP: root required for namespaces and TUN devices"; exit 77; }
FIPSD=$(readlink -f "$FIPSD")
WORK=$(mktemp -d /tmp/fipsd-ipv6-netns.XXXXXX)
NS_A=fipsA-$$ NS_B=fipsB-$$
A_PID= B_PID=
cleanup() {
    set +e
    [[ -n $B_PID ]] && kill -TERM "$B_PID" 2>/dev/null && wait "$B_PID" 2>/dev/null
    [[ -n $A_PID ]] && kill -TERM "$A_PID" 2>/dev/null && wait "$A_PID" 2>/dev/null
    ip netns del "$NS_A" 2>/dev/null; ip netns del "$NS_B" 2>/dev/null
    if [[ ${KEEP:-0} == 1 ]]; then echo "logs kept in $WORK"; else rm -rf "$WORK"; fi
}
trap cleanup EXIT
ip netns add "$NS_A"; ip netns add "$NS_B"
ip link add veth-a type veth peer name veth-b
ip link set veth-a netns "$NS_A"; ip link set veth-b netns "$NS_B"
ip -n "$NS_A" addr add 10.77.0.1/24 dev veth-a; ip -n "$NS_B" addr add 10.77.0.2/24 dev veth-b
ip -n "$NS_A" link set lo up; ip -n "$NS_B" link set lo up
ip -n "$NS_A" link set veth-a up; ip -n "$NS_B" link set veth-b up
# Query a daemon's control socket (a filesystem socket: reachable from any namespace).
control() {
    python3 - "$1" "$2" <<'PY'
import json, socket, sys
path, command = sys.argv[1], sys.argv[2]
with socket.socket(socket.AF_UNIX) as c:
    c.settimeout(10); c.connect(path); c.sendall((json.dumps({'command': command}) + '\n').encode())
    line = c.makefile('rb').readline()
r = json.loads(line)
if r.get('status') != 'ok': sys.exit(f"{command}: {r.get('message')}")
print(json.dumps(r['data']))
PY
}
await_socket() { local path=$1 n=0; until [[ -S $path ]]; do (( n++ > 300 )) && { echo "no control socket at $path"; return 1; }; sleep 0.1; done; }
config() { # name nsec bind peer_npub peer_addr
    cat <<YAML
node:
  identity: {nsec: '$2', persistent: false}
  control: {enabled: true, socket_path: '$WORK/$1.sock'}
transports:
  udp: {bind_addr: '$3', mtu: 1472}
tun: {enabled: true, name: fips$1}
peers:$( [[ -n ${4:-} ]] && printf '\n  - npub: %s\n    addresses: [{transport: udp, addr: "%s"}]' "$4" "$5" || printf ' []' )
YAML
}
config A "$(printf '1%.0s' {1..64})" 10.77.0.1:2121 > "$WORK/A.yaml"
ip netns exec "$NS_A" "$FIPSD" --config "$WORK/A.yaml" > "$WORK/A.log" 2>&1 & A_PID=$!
await_socket "$WORK/A.sock"
A_STATUS=$(control "$WORK/A.sock" show_status)
A_NPUB=$(python3 -c 'import json,sys; print(json.loads(sys.argv[1])["npub"])' "$A_STATUS")
A_ADDR=$(python3 -c 'import json,sys; print(json.loads(sys.argv[1])["ipv6_addr"])' "$A_STATUS")
config B "$(printf '2%.0s' {1..64})" 10.77.0.2:2121 "$A_NPUB" 10.77.0.1:2121 > "$WORK/B.yaml"
ip netns exec "$NS_B" "$FIPSD" --config "$WORK/B.yaml" > "$WORK/B.log" 2>&1 & B_PID=$!
await_socket "$WORK/B.sock"
B_ADDR=$(python3 -c 'import json,sys; print(json.loads(sys.argv[1])["ipv6_addr"])' "$(control "$WORK/B.sock" show_status)")
echo "A $A_ADDR (fipsA in $NS_A)  B $B_ADDR (fipsB in $NS_B)"
# ping keeps sending once a second until the deadline: the first replies
# arrive once B's configured dial, the tree and the session are up.
ip netns exec "$NS_B" ping -6 -c 3 -w 30 "$A_ADDR"
ip netns exec "$NS_A" ping -6 -c 3 -w 30 "$B_ADDR"
ip netns exec "$NS_B" ping -6 -c 2 -s 1400 -w 10 "$A_ADDR" || echo "1400-byte ping: $? (PMTU 1280 answered by the shim; fragments or a resized probe follow)"
if command -v iperf3 > /dev/null; then
    ip netns exec "$NS_A" iperf3 -s -1 -B "$A_ADDR" -p 5201 > "$WORK/iperf-server.log" 2>&1 &
    IPERF=$!
    sleep 1
    ip netns exec "$NS_B" iperf3 -c "$A_ADDR" -6 -p 5201 -t 3
    wait "$IPERF"
fi
for n in A B; do
    status=$(control "$WORK/$n.sock" show_status)
    echo "$n: $(python3 -c 'import json,sys; d=json.loads(sys.argv[1]); print({k: d[k] for k in ("tun_state","tun_name","effective_ipv6_mtu","host_drops","peer_count","session_count")})' "$status")"
done
echo "fipsd IPv6 netns smoke: two real TUNs, ping both ways$(command -v iperf3 > /dev/null && echo ', iperf3'): ok"
