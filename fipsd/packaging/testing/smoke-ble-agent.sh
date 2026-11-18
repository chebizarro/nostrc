#!/bin/bash
# Real-radio smoke of the macOS BLE backend (nostrc-gfi8h.6.3), as a user:
#
#   fipsd/packaging/testing/smoke-ble-agent.sh FIPSD FIPS_BLE_AGENT [PROBE_SECONDS]
#
# 1. The agent alone (--probe PROBE_SECONDS, default 15) under a throwaway
#    LaunchAgent in gui/<uid>, so that it is its own responsible process for
#    the Bluetooth permission (TCC). Its first run of a given build shows
#    "fips-ble-agent would like to use Bluetooth", which only the user can
#    answer; started from a terminal or IDE instead, macOS would hold that app
#    responsible and abort the agent if it carries no Bluetooth usage string.
#    The radio must come up, publish an L2CAP PSM and advertise; the probe
#    also reports what it heard.
# 2. FIPSD as this user with transports.ble and the agent socket in a
#    temporary directory (FIPSD_BLE_AGENT_SOCKET), and the agent, again under
#    launchd, connecting to it (--socket, --daemon-uid). fipsd must take the
#    agent's hello and bring the transport up on the agent's PSM (listen), the
#    agent must start advertising and scanning. (show_transports is printed;
#    its "up" covers an absent transport too, as upstream, so the daemon's
#    "BLE transport up ... psm" line is the proof.)
# Both jobs are booted out and the directory removed at the end. A real L2CAP
# link needs a second BLE device running FIPS; this checks one Mac's half.
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
[ "$(uname -s)" = Darwin ] || { echo "smoke-ble-agent: macOS only" >&2; exit 2; }
abs() { echo "$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"; }
fipsd=$(abs "${1:?usage: smoke-ble-agent.sh FIPSD FIPS_BLE_AGENT [PROBE_SECONDS]}")
agent=$(abs "${2:?usage: smoke-ble-agent.sh FIPSD FIPS_BLE_AGENT [PROBE_SECONDS]}")
probe_seconds=${3:-15}
uid=$(id -u)
work=$(mktemp -d /tmp/fips-ble-smoke.XXXXXX) # short: the sockets live here
probe_label=com.fips.ble-agent.smoke-probe.$$
agent_label=com.fips.ble-agent.smoke.$$
fipsd_pid=""
cleanup() {
    launchctl bootout "gui/$uid/$probe_label" 2>/dev/null || true
    launchctl bootout "gui/$uid/$agent_label" 2>/dev/null || true
    if [ -n "$fipsd_pid" ]; then kill -TERM "$fipsd_pid" 2>/dev/null || true; wait "$fipsd_pid" 2>/dev/null || true; fi
    rm -rf "$work"
}
trap cleanup EXIT
fail() { echo "smoke-ble-agent: $*" >&2; for f in "$work"/*.log "$work"/*.out; do [ -f "$f" ] && { echo "--- $f" >&2; tail -40 "$f" >&2; }; done; exit 1; }
# The first line of FILE, as it grows, matching REGEX within SECONDS: a
# bounded wait on a daemon's log (reread every 200 ms), not a timing guess.
wait_line() {
    perl -e 'my ($file, $re, $secs) = @ARGV; my $end = time + $secs; my $pos = 0;
        while (time < $end) {
            if (open my $fh, "<", $file) { seek $fh, $pos, 0; while (my $l = <$fh>) { if ($l =~ /$re/) { print $l; exit 0 } } $pos = tell $fh; close $fh }
            select undef, undef, undef, 0.2 }
        exit 1' "$1" "$2" "$3"
}
launch() { # LABEL OUT ARGS...: a throwaway LaunchAgent in gui/<uid>
    local label=$1 out=$2; shift 2
    { printf '<?xml version="1.0" encoding="UTF-8"?>\n<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">\n'
      printf '<plist version="1.0"><dict><key>Label</key><string>%s</string><key>ProgramArguments</key><array>' "$label"
      for a in "$@"; do printf '<string>%s</string>' "$a"; done
      printf '</array><key>StandardOutPath</key><string>%s</string><key>StandardErrorPath</key><string>%s</string>' "$out" "$out"
      printf '<key>RunAtLoad</key><true/><key>KeepAlive</key><false/></dict></plist>\n'; } > "$work/$label.plist"
    plutil -lint "$work/$label.plist" >/dev/null
    launchctl bootstrap "gui/$uid" "$work/$label.plist"
}

"$fipsd" -V; "$agent" --version
launchctl plist __TEXT,__info_plist "$agent" 2>/dev/null | grep -q NSBluetoothAlwaysUsageDescription ||
    fail "$agent has no NSBluetoothAlwaysUsageDescription in __TEXT,__info_plist"

# 1. the radio, alone
launch "$probe_label" "$work/probe.out" "$agent" --probe "$probe_seconds"
echo "probe: running for ${probe_seconds}s (answer the Bluetooth permission prompt if one appears)"
wait_line "$work/probe.out" '^heard:' $((probe_seconds + 300)) >/dev/null || fail "the probe did not finish: is the Bluetooth prompt still unanswered?"
grep -v 'advert from' "$work/probe.out"
grep -q '^radio: up' "$work/probe.out" || fail "the radio did not come up (Bluetooth off, or permission denied?)"
grep -q '^L2CAP listener: published on psm' "$work/probe.out" || fail "no L2CAP listener was published"
grep -q 'advertising the FIPS service: yes' "$work/probe.out" || fail "advertising did not start"
launchctl bootout "gui/$uid/$probe_label" 2>/dev/null || true

# 2. fipsd and the agent
mkdir -p "$work/run"
cat > "$work/fips.yaml" <<YAML
node:
  identity: {persistent: false}
  control: {enabled: true, socket_path: "$work/run/control.sock"}
  native_api: {enabled: false}
tun: {enabled: false}
dns: {enabled: false}
transports:
  ble:
    adapter: ble0
    auto_connect: true
YAML
FIPSD_BLE_AGENT_SOCKET="$work/run/ble-agent.sock" "$fipsd" -c "$work/fips.yaml" --log-file "$work/fipsd.log" &
fipsd_pid=$!
wait_line "$work/fipsd.log" 'waiting for the fips BLE agent' 20 || fail "fipsd did not listen for the agent"
launch "$agent_label" "$work/agent.out" "$agent" --socket "$work/run/ble-agent.sock" --daemon-uid "$uid" --verbose --log-file "$work/agent.log"
wait_line "$work/fipsd.log" 'fips BLE agent connected; radio up' 60 || fail "the agent never said hello to fipsd"
wait_line "$work/fipsd.log" 'BLE transport up on ble0, psm 0x' 30 || fail "fipsd did not bring the BLE transport up (listen)"
wait_line "$work/agent.log" 'advertising the FIPS service' 30 || fail "the agent did not start advertising for fipsd (advertise)"
wait_line "$work/agent.log" 'BLE: scanning' 30 || fail "the agent did not start scanning for fipsd"
python3 "$here/control-query.py" --socket "$work/run/control.sock" --retry 20 show_transports |
    python3 -c 'import json,sys; d=json.load(sys.stdin); b=[t for t in d["data"]["transports"] if t["type"]=="ble"]; print("show_transports ble:", json.dumps(b)); sys.exit(0 if b else 1)' ||
    fail "show_transports has no BLE transport"
echo "FIPS adverts heard by the agent: $(grep -c 'FIPS advert from' "$work/agent.log" || true)"
launchctl bootout "gui/$uid/$agent_label"
wait_line "$work/fipsd.log" 'fips BLE agent disconnected; radio down' 30 || fail "fipsd did not see the agent leave"
echo "smoke-ble-agent: ok (radio up, listener published, fipsd listening on the agent's PSM, advertising and scanning)"
