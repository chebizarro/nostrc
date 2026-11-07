#!/usr/bin/env bash
# Explicit live drill; never selected by the ordinary unit suite.
set -euo pipefail
: "${SIGNET_LIVE_RELAYS:?live relays required; this is not a skipped test}"
: "${SIGNET_LIVE_CONFIG:?protected reference client INI required}"
: "${SIGNET_DAEMON_STOP:?path to operator-owned stop executable required}"
: "${SIGNET_DAEMON_START:?path to operator-owned start executable required}"
: "${SIGNET_REVOKE_CLIENT:?path to signed-management revoke executable required}"
: "${SIGNET_HEALTH_URL:?daemon loopback URL required}"
client=${1:?reference-client executable required}
[[ -x $client && -x $SIGNET_DAEMON_STOP && -x $SIGNET_DAEMON_START && -x $SIGNET_REVOKE_CLIENT ]]
case "$SIGNET_HEALTH_URL" in http://127.0.0.1:*|http://localhost:*) ;; *) exit 2;; esac
curl --fail --silent --show-error --max-time 10 "$SIGNET_HEALTH_URL/ready" >/dev/null
"$client" "$SIGNET_LIVE_CONFIG" pair
"$client" "$SIGNET_LIVE_CONFIG" deny
# Independent process, same transport key, no new connect secret.
"$client" "$SIGNET_LIVE_CONFIG" sign
"$SIGNET_DAEMON_STOP"
if curl --fail --silent --max-time 3 "$SIGNET_HEALTH_URL/health" >/dev/null; then
  echo 'daemon did not stop' >&2; exit 1
fi
# Deliberate replay quarantine: 2 * <=120s accepted skew + 30s margin.
# NOT a relay backfill/settle delay; host preflight enforces the skew bound.
sleep 270
"$SIGNET_DAEMON_START"
deadline=$((SECONDS + 120))
until curl --fail --silent --max-time 3 "$SIGNET_HEALTH_URL/ready" >/dev/null; do
  (( SECONDS < deadline )) || { echo 'daemon never became ready' >&2; exit 1; }
  sleep 1
done
"$client" "$SIGNET_LIVE_CONFIG" sign
"$SIGNET_REVOKE_CLIENT"
curl --fail --silent --show-error --max-time 10 "$SIGNET_HEALTH_URL/ready" >/dev/null
"$client" "$SIGNET_LIVE_CONFIG" revoked
echo 'Live pair/sign/denial/client restart/daemon restart/revocation: PASS'
