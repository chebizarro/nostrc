#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Manual live-daemon D-Bus passkey interop harness.
#
# Starts a private D-Bus session bus, runs a real signetd against throwaway
# stores, drives net.signet.Passkeys with gdbus, then verifies the resulting
# WebAuthn attestation/assertion with the independent python-fido2 checker used
# by the Phase 0/3 interop tests.
#
# By default it uses build-signet-testhooks/ so it never flips the normal build/
# cache to SIGNET_ENABLE_TEST_HOOKS=ON.
#
# This is intentionally not part of the default ctest run. It needs dbus-daemon,
# gdbus, a passkey-enabled signetd build, and a host that can run the daemon.

set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
repo="$(cd "$here/../../.." && pwd)"
phase0="$repo/signet/tests/phase0"
build_dir="${SIGNET_TESTHOOKS_BUILD_DIR:-${BUILD_DIR:-$repo/build-signet-testhooks}}"
signetd="${SIGNETD:-$build_dir/signet/signetd}"
result_dir="${SIGNET_LIVE_DBUS_OUT:-$build_dir/signet/tests/live_dbus_interop}"
agent_id="${SIGNET_LIVE_DBUS_AGENT_ID:-agent-live-dbus}"
# Deterministic non-secret test keys for this disposable harness only.
db_key="${SIGNET_LIVE_DBUS_DB_KEY:-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa}"
bunker_nsec="${SIGNET_LIVE_DBUS_BUNKER_NSEC:-1111111111111111111111111111111111111111111111111111111111111111}"
sync_key="${SIGNET_LIVE_DBUS_SYNC_KEY:-0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef}"

need() {
  if ! command -v "$1" >/dev/null 2>&1; then
    echo "BLOCKER: required command '$1' not found" >&2
    return 1
  fi
}

need dbus-daemon
need gdbus
need python3
need cmake

echo "== ensuring passkey-enabled signetd with test hooks at $build_dir =="
cmake -S "$repo" -B "$build_dir" -DSIGNET_ENABLE_PASSKEYS=ON -DSIGNET_ENABLE_TEST_HOOKS=ON
cmake --build "$build_dir" --target signetd

if [ ! -x "$signetd" ]; then
  echo "BLOCKER: signetd is not executable at $signetd" >&2
  exit 1
fi

tmp="$(mktemp -d "${TMPDIR:-/tmp}/signet-live-dbus.XXXXXX")"
bus_pid=""
daemon_pid=""

cleanup() {
  set +e
  if [ -n "${daemon_pid:-}" ]; then
    kill "$daemon_pid" >/dev/null 2>&1 || true
    wait "$daemon_pid" >/dev/null 2>&1 || true
  fi
  if [ -n "${bus_pid:-}" ]; then
    kill "$bus_pid" >/dev/null 2>&1 || true
    wait "$bus_pid" >/dev/null 2>&1 || true
  fi
  rm -rf "$tmp"
}
trap cleanup EXIT

show_daemon_log() {
  local log="$1"
  if [ -f "$log" ]; then
    echo "---- signetd log: $log ----" >&2
    sed -n '1,220p' "$log" >&2 || true
    echo "---- end signetd log ----" >&2
  fi
}

write_config() {
  local db="$1"
  local cfg="$2"
  local audit="$3"
  cat >"$cfg" <<EOF_CFG
[server]
log_level = debug
health_port = 0

[store]
db_path = $db

[nostr]
# Required by signetd config validation; this harness only exercises local D-Bus.
relays = ws://127.0.0.1:9
identity = live-dbus-interop

[audit]
path = $audit
stdout = false

[dbus]
unix_enabled = true
tcp_enabled = false

[nip5l]
enabled = false

[ssh_agent]
enabled = false

[passkeys]
enabled = true
backend = software-openssl
aaguid = 80c64041-9927-4901-957f-e0032db96bee
attestation = none
allow_headless_uv = false
virtual_ctap = false
sync_key = $sync_key
EOF_CFG
}

variant_first() {
  python3 - "$1" <<'PY'
import ast, sys
try:
    value = ast.literal_eval(sys.argv[1])
    print(value[0])
except Exception as exc:  # noqa: BLE001
    raise SystemExit(f"failed to parse gdbus output {sys.argv[1]!r}: {exc}")
PY
}

dbus_call_raw() {
  local method="$1"
  shift
  gdbus call --session --timeout=1 \
    --dest net.signet.Signer \
    --object-path /net/signet/Signer \
    --method "net.signet.Passkeys.$method" "$@"
}

dbus_call_json() {
  variant_first "$(dbus_call_raw "$@")"
}

wait_for_dbus() {
  local log="$1"
  local i
  for i in $(seq 1 80); do
    if dbus_call_raw GetInfo >/dev/null 2>&1; then
      return 0
    fi
    if ! kill -0 "$daemon_pid" >/dev/null 2>&1; then
      echo "BLOCKER: signetd exited before acquiring D-Bus name" >&2
      show_daemon_log "$log"
      return 1
    fi
    sleep 0.25
  done
  echo "BLOCKER: timed out waiting for net.signet.Signer on private session bus" >&2
  show_daemon_log "$log"
  return 1
}

start_daemon() {
  local db="$1"
  local cfg="$2"
  local log="$3"
  local audit="$4"
  write_config "$db" "$cfg" "$audit"
  SIGNET_DB_KEY="$db_key" \
  SIGNET_BUNKER_NSEC="$bunker_nsec" \
  SIGNET_DBUS_SESSION_BUS=1 \
  SIGNET_DBUS_TEST_AGENT_ID="$agent_id" \
  SIGNET_DBUS_TEST_PROVISION_AGENT=1 \
  SIGNET_DBUS_TEST_GRANT_PASSKEYS=1 \
  SIGNET_DBUS_TEST_SKIP_RELAY=1 \
  "$signetd" -c "$cfg" >"$log" 2>&1 &
  daemon_pid=$!
  wait_for_dbus "$log"
}

stop_daemon() {
  if [ -n "${daemon_pid:-}" ]; then
    kill "$daemon_pid" >/dev/null 2>&1 || true
    wait "$daemon_pid" >/dev/null 2>&1 || true
    daemon_pid=""
  fi
}

strings "$signetd" >"$tmp/signetd.strings"
if ! grep -q 'SIGNET_DBUS_TEST_AGENT_ID' "$tmp/signetd.strings"; then
  echo "BLOCKER: $signetd was not built with SIGNET_ENABLE_TEST_HOOKS=ON" >&2
  exit 1
fi

# Start a private session bus.
bus_info="$tmp/bus.info"
if ! dbus-daemon --session --address="unix:tmpdir=$tmp" --fork --print-address=1 --print-pid=1 >"$bus_info" 2>"$tmp/dbus.err"; then
  echo "BLOCKER: dbus-daemon failed to start a private session bus" >&2
  cat "$tmp/dbus.err" >&2 || true
  exit 1
fi
while IFS= read -r line; do
  case "$line" in
    unix:*|tcp:*|nonce-tcp:*) DBUS_SESSION_BUS_ADDRESS="$line" ;;
    '' ) ;;
    * ) bus_pid="$line" ;;
  esac
done <"$bus_info"
export DBUS_SESSION_BUS_ADDRESS
if [ -z "${DBUS_SESSION_BUS_ADDRESS:-}" ] || [ -z "${bus_pid:-}" ]; then
  echo "BLOCKER: could not parse dbus-daemon address/pid from:" >&2
  cat "$bus_info" >&2 || true
  exit 1
fi

echo "== private D-Bus session bus =="
echo "address=$DBUS_SESSION_BUS_ADDRESS"
echo "pid=$bus_pid"

# Prepare python-fido2 verifier venv (same location as the existing checks).
venv="$phase0/_build/venv"
if [ ! -x "$venv/bin/python" ]; then
  python3 -m venv "$venv"
fi
if ! "$venv/bin/python" -c "import fido2" 2>/dev/null; then
  "$venv/bin/pip" install --quiet --disable-pip-version-check fido2==2.2.1
fi

python3 - "$tmp" <<'PY'
import base64, json, pathlib, sys
out = pathlib.Path(sys.argv[1])
reg = bytes((0x20 + i) & 0xff for i in range(32))
ass = bytes((0xa0 + i) & 0xff for i in range(32))
user = bytes((0x60 + i) & 0xff for i in range(8))
mk = {
    "rpId": "example.com",
    "clientDataHash": base64.b64encode(reg).decode(),
    "userHandle": base64.b64encode(user).decode(),
    "userName": "interop-agent",
    "userDisplayName": "Interop Agent",
    "discoverable": True,
    "userVerification": "preferred",
    "pubKeyCredParams": [-7],
}
(out / "make_credential.json").write_text(json.dumps(mk, separators=(",", ":")))
(out / "assert_hash.b64").write_text(base64.b64encode(ass).decode())
PY
mk_req="$(cat "$tmp/make_credential.json")"
assert_hash_b64="$(cat "$tmp/assert_hash.b64")"

# Daemon A: create credential, assert, export.
db_a="$tmp/signet-a.db"
cfg_a="$tmp/signet-a.conf"
log_a="$tmp/signet-a.log"
audit_a="$tmp/audit-a.log"
echo "== starting signetd A =="
start_daemon "$db_a" "$cfg_a" "$log_a" "$audit_a"

info_json="$(dbus_call_json GetInfo)"
echo "GetInfo: $info_json"

mk_json="$(dbus_call_json MakeCredential "$mk_req")"
echo "$mk_json" >"$tmp/make_credential.response.json"
echo "MakeCredential: $(python3 -c 'import json,sys; j=json.load(open(sys.argv[1])); print("credentialId_len=%d attestationObject_len=%d" % (len(j["credentialId"]), len(j["attestationObject"])))' "$tmp/make_credential.response.json")"

cred_b64="$(python3 - "$tmp/make_credential.response.json" <<'PY'
import json, sys
print(json.load(open(sys.argv[1]))["credentialId"])
PY
)"
ga_req="$(python3 - "$assert_hash_b64" "$cred_b64" <<'PY'
import json, sys
print(json.dumps({
    "rpId": "example.com",
    "clientDataHash": sys.argv[1],
    "userVerification": "preferred",
    "allowCredentials": [sys.argv[2]],
}, separators=(",", ":")))
PY
)"
ga_json_a="$(dbus_call_json GetAssertion "$ga_req")"
echo "$ga_json_a" >"$tmp/get_assertion.a.response.json"
echo "GetAssertion(A): $(python3 -c 'import json,sys; j=json.load(open(sys.argv[1])); print("authData_len=%d signature_len=%d" % (len(j["authData"]), len(j["signature"])))' "$tmp/get_assertion.a.response.json")"

export_req="$(python3 - "$cred_b64" <<'PY'
import json, sys
print(json.dumps({"credentialId": sys.argv[1]}, separators=(",", ":")))
PY
)"
export_json="$(dbus_call_json ExportCredential "$export_req")"
echo "$export_json" >"$tmp/export.response.json"
container_b64="$(python3 - "$tmp/export.response.json" <<'PY'
import json, sys
j = json.load(open(sys.argv[1]))
print(j["container"])
PY
)"
echo "ExportCredential: $(python3 -c 'import json,base64,sys; j=json.load(open(sys.argv[1])); print("format=%s container_bytes=%d" % (j.get("format"), len(base64.b64decode(j["container"]))))' "$tmp/export.response.json")"
stop_daemon

# Daemon B: fresh DB, import the exported container, assert with imported key.
db_b="$tmp/signet-b.db"
cfg_b="$tmp/signet-b.conf"
log_b="$tmp/signet-b.log"
audit_b="$tmp/audit-b.log"
echo "== starting signetd B (fresh DB) =="
start_daemon "$db_b" "$cfg_b" "$log_b" "$audit_b"
import_req="$(python3 - "$container_b64" <<'PY'
import json, sys
print(json.dumps({"container": sys.argv[1]}, separators=(",", ":")))
PY
)"
import_json="$(dbus_call_json ImportCredential "$import_req")"
echo "$import_json" >"$tmp/import.response.json"
echo "ImportCredential: $import_json"

ga_json_b="$(dbus_call_json GetAssertion "$ga_req")"
echo "$ga_json_b" >"$tmp/get_assertion.b.response.json"
echo "GetAssertion(B/imported): $(python3 -c 'import json,sys; j=json.load(open(sys.argv[1])); print("authData_len=%d signature_len=%d" % (len(j["authData"]), len(j["signature"])))' "$tmp/get_assertion.b.response.json")"

artifact="$tmp/live_dbus_artifacts.json"
python3 - "$tmp/make_credential.response.json" "$tmp/get_assertion.b.response.json" "$assert_hash_b64" "$artifact" <<'PY'
import base64, json, sys
mk = json.load(open(sys.argv[1]))
ga = json.load(open(sys.argv[2]))
assert_hash = base64.b64decode(sys.argv[3])

def b64(member, obj):
    return base64.b64decode(obj[member])

cred = b64("credentialId", mk)
att = b64("attestationObject", mk)
auth_assert = b64("authData", ga)
sig = b64("signature", ga)
cose = b64("publicKeyCose", mk)
# Current Signet COSE_Key encoding is deterministic and checked by the Phase 3
# emitter: a5 ... -2: h'X' at offsets 10..41, -3: h'Y' at offsets 45..76.
out = {
    "rpId": "example.com",
    "credentialId": cred.hex(),
    "attestationObject": att.hex(),
    "authDataAssert": auth_assert.hex(),
    "clientDataHashAssert": assert_hash.hex(),
    "signature": sig.hex(),
    "pubX": cose[10:42].hex(),
    "pubY": cose[45:77].hex(),
}
open(sys.argv[4], "w").write(json.dumps(out, indent=2) + "\n")
PY

echo "== independent verification (python-fido2) =="
"$venv/bin/python" "$phase0/verify_external.py" "$artifact"

mkdir -p "$result_dir"
cp "$artifact" "$result_dir/live_dbus_artifacts.json"
cp "$log_a" "$result_dir/signet-a.log"
cp "$log_b" "$result_dir/signet-b.log"
cp "$tmp/make_credential.response.json" "$result_dir/make_credential.response.json"
cp "$tmp/get_assertion.a.response.json" "$result_dir/get_assertion.a.response.json"
cp "$tmp/export.response.json" "$result_dir/export.response.json"
cp "$tmp/import.response.json" "$result_dir/import.response.json"
cp "$tmp/get_assertion.b.response.json" "$result_dir/get_assertion.b.response.json"

echo "== live D-Bus interop PASS =="
echo "artifact=$result_dir/live_dbus_artifacts.json"
echo "daemon_a_log=$result_dir/signet-a.log"
echo "daemon_b_log=$result_dir/signet-b.log"
