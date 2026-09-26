#!/usr/bin/env bash
#
# Regression: nostrc-relayd must actually bind the host from cfg.listen.
#
# Before this test, relay.toml.example documented `listen = "127.0.0.1:4848"`
# but relayd only parsed the port half of the string, so libwebsockets fell
# back to a wide `0.0.0.0` bind. This test proves both directions:
#   (a) `127.0.0.1:<port>` opens only a loopback socket;
#   (b) `0.0.0.0:<port>`   opens a wide bind (so we know (a) is not just an
#                          accidental clamp from the environment).
#
# Requires `ss` (from iproute2) which is Linux-only; caller (CMake) gates.

set -euo pipefail

if [[ $# -lt 1 ]]; then
  echo "usage: $0 <path-to-nostrc-relayd>" >&2
  exit 2
fi
RELAYD_BIN="$1"

command -v ss >/dev/null 2>&1 || {
  echo "SKIP: ss(1) unavailable" >&2
  exit 77 # CTest SKIP_RETURN_CODE
}

# Pick a free TCP port so parallel test jobs never collide.
pick_port() {
  python3 - <<'PY'
import socket
s = socket.socket()
s.bind(("127.0.0.1", 0))
print(s.getsockname()[1])
s.close()
PY
}

WORK="$(mktemp -d)"
RELAY_PID=""
cleanup() {
  if [[ -n "$RELAY_PID" ]]; then
    kill "$RELAY_PID" 2>/dev/null || true
    # Wait up to 3s for graceful exit, then SIGKILL. `wait` only works for
    # direct children, so poll /proc instead.
    for _ in $(seq 1 30); do
      kill -0 "$RELAY_PID" 2>/dev/null || break
      sleep 0.1
    done
    kill -9 "$RELAY_PID" 2>/dev/null || true
  fi
  rm -rf "$WORK"
}
trap cleanup EXIT

start_relayd() {
  local host="$1" port="$2" workdir="$3"
  cat >"$workdir/relay.toml" <<EOF
listen = "$host:$port"
storage_driver = "nostrdb"
auth = "off"
EOF
  # Background directly (no subshell) so `$!` captures the daemon's real pid.
  ( cd "$workdir" && exec "$RELAYD_BIN" >"$workdir/relayd.log" 2>&1 ) &
  RELAY_PID="$!"
  # Wait up to 5s for the port to appear in listening state.
  for _ in $(seq 1 50); do
    if ss -ltnH "sport = :$port" 2>/dev/null | grep -q LISTEN; then
      return 0
    fi
    if ! kill -0 "$RELAY_PID" 2>/dev/null; then
      echo "FAIL: relayd exited early" >&2
      cat "$workdir/relayd.log" >&2 || true
      return 1
    fi
    sleep 0.1
  done
  echo "FAIL: relayd did not listen on :$port within 5s" >&2
  cat "$workdir/relayd.log" >&2 || true
  return 1
}

stop_relayd() {
  if [[ -n "$RELAY_PID" ]]; then
    kill "$RELAY_PID" 2>/dev/null || true
    for _ in $(seq 1 30); do
      kill -0 "$RELAY_PID" 2>/dev/null || break
      sleep 0.1
    done
    kill -9 "$RELAY_PID" 2>/dev/null || true
    wait "$RELAY_PID" 2>/dev/null || true
    RELAY_PID=""
  fi
}

# ---------- (a) loopback bind honors 127.0.0.1 ----------------------------
LOOP_PORT="$(pick_port)"
LOOP_DIR="$WORK/loop"
mkdir -p "$LOOP_DIR"
start_relayd "127.0.0.1" "$LOOP_PORT" "$LOOP_DIR"

LOOP_LINES="$(ss -ltnH "sport = :$LOOP_PORT" | awk '{print $4}')"
echo "loopback ss lines:"
echo "$LOOP_LINES"

# Must contain the loopback bind:
grep -Eq '^127\.0\.0\.1:'"$LOOP_PORT"'$' <<<"$LOOP_LINES" || {
  echo "FAIL: expected 127.0.0.1:$LOOP_PORT in listen set" >&2
  exit 1
}

# Must NOT contain a wildcard bind — that is the regression:
if grep -Eq '^(0\.0\.0\.0|\*|::|\[::\]):'"$LOOP_PORT"'$' <<<"$LOOP_LINES"; then
  echo "FAIL: relayd bound wildcard while cfg.listen was 127.0.0.1" >&2
  exit 1
fi
stop_relayd

# ---------- (b) wildcard bind honors 0.0.0.0 (proves cfg is respected) ----
WIDE_PORT="$(pick_port)"
WIDE_DIR="$WORK/wide"
mkdir -p "$WIDE_DIR"
start_relayd "0.0.0.0" "$WIDE_PORT" "$WIDE_DIR"

WIDE_LINES="$(ss -ltnH "sport = :$WIDE_PORT" | awk '{print $4}')"
echo "wildcard ss lines:"
echo "$WIDE_LINES"

grep -Eq '^(0\.0\.0\.0|\*):'"$WIDE_PORT"'$' <<<"$WIDE_LINES" || {
  echo "FAIL: expected wildcard bind on :$WIDE_PORT when cfg.listen=0.0.0.0" >&2
  exit 1
}
stop_relayd

# ---------- (c) bare port is rejected at config load ---------------------
REJECT_DIR="$WORK/reject"
mkdir -p "$REJECT_DIR"
cat >"$REJECT_DIR/relay.toml" <<'EOF'
listen = "4848"
storage_driver = "nostrdb"
auth = "off"
EOF
if (cd "$REJECT_DIR" && "$RELAYD_BIN" >"$REJECT_DIR/log" 2>&1); then
  echo "FAIL: relayd accepted port-only listen (should have exited nonzero)" >&2
  cat "$REJECT_DIR/log" >&2 || true
  exit 1
fi

echo OK
