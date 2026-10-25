#!/usr/bin/env bash
# Disposable §13 h conformance. Owns only its unique Compose project + temp dir.
set -euo pipefail
root=$(cd "$(dirname "$0")" && pwd)
relayd=${NOSTRC_RELAYD_BINARY:-${1:-}}
if [[ -z "$relayd" || ! -x "$relayd" ]]; then
  echo "Usage: $0 /absolute/path/to/nostrc-relayd (with nostrdb storage)" >&2
  exit 2
fi
relayd=$(cd "$(dirname "$relayd")" && pwd)/$(basename "$relayd")
for tool in docker openssl curl python3; do command -v "$tool" >/dev/null; done
docker info >/dev/null
docker compose version >/dev/null
umask 077
work=$(mktemp -d "${TMPDIR:-/tmp}/sv9-vsp01.XXXXXX")
project="sv9-vsp01-$(basename "$work" | tr '[:upper:].' '[:lower:]-')"
relay_pid=
compose() {
  docker compose -p "$project" -f "$root/docker-compose.yml" \
    -f "$root/docker-compose.vsp01.yml" "$@"
}
cleanup() {
  rc=$?
  trap - EXIT INT TERM
  if (( rc != 0 )); then
    compose logs --no-color >&2 || true
    [[ ! -f "$work/relayd.log" ]] || cat "$work/relayd.log" >&2
  fi
  local cleanup_rc=0
  compose down --volumes --remove-orphans || cleanup_rc=$?
  if [[ -n "$relay_pid" ]]; then kill "$relay_pid" 2>/dev/null || true; wait "$relay_pid" || true; fi
  rm -rf "$work"
  if (( cleanup_rc != 0 )); then
    echo "Cleanup failed for Compose project $project" >&2
    rc=1
  fi
  exit "$rc"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
read -r VSP01_PORT relay_port < <(python3 - <<'PY'
import socket
with socket.socket() as a, socket.socket() as b:
    a.bind(('127.0.0.1', 0)); b.bind(('127.0.0.1', 0))
    print(a.getsockname()[1], b.getsockname()[1])
PY
)
export VSP01_PORT
export VSP01_ALLOWLIST="$work/allowlist.txt" VSP01_CERT_DIR="$work/certs"
# Public test vector sk=1 only. Never use this identity outside the lab.
printf '%s\n' 79be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798 > "$VSP01_ALLOWLIST"
mkdir "$VSP01_CERT_DIR"
openssl req -x509 -newkey rsa:2048 -nodes -days 1 -subj /CN=localhost \
  -keyout "$VSP01_CERT_DIR/privkey.pem" -out "$VSP01_CERT_DIR/fullchain.pem" >/dev/null 2>&1
cat > "$work/relay.toml" <<EOF
listen = "${VSP01_RELAY_BIND:-127.0.0.1}:$relay_port"
storage_driver = "nostrdb"
auth = "optional"
# The finite conformance matrix sends many denials back-to-back. Keep the
# production limiter enabled, with capacity for this test's control frames.
rate_ops_per_sec = 200
rate_burst = 400
relay_url = "ws://host.docker.internal:$relay_port"
vault_allowlist = "$VSP01_ALLOWLIST"
EOF
(cd "$work" && exec "$relayd") > "$work/relayd.log" 2>&1 &
relay_pid=$!
compose up -d
# Readiness only; conformance never uses a delay as EOSE/OK completion.
curl --silent --show-error --fail --retry 30 --retry-connrefused --retry-delay 1 \
  --retry-max-time 60 --max-time 5 -H 'Accept: application/nostr+json' \
  "http://127.0.0.1:$relay_port/" >/dev/null
# The self-signed cert is isolated to this disposable lab. Exercise TLS ingress.
curl --insecure --silent --show-error --fail --retry 30 --retry-all-errors \
  --retry-delay 1 --retry-max-time 90 --max-time 5 \
  "https://127.0.0.1:$VSP01_PORT/" >/dev/null
compose exec -T vsp-auth deno run --cached-only --allow-net \
  /app/vsp01-conformance.ts http://nginx "ws://host.docker.internal:$relay_port"
echo 'PASS VSP-01 lab conformance (§13 h); tearing down task-owned resources'
