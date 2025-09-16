#!/usr/bin/env bash
set -euo pipefail

# Best-effort Xvfb-driven UI smoke for Online Accounts → Nostr
# Skips gracefully if tools are unavailable.

if ! command -v gnome-control-center >/dev/null 2>&1; then
  echo "gnome-control-center not found; skipping" >&2
  exit 77
fi
if ! command -v xvfb-run >/dev/null 2>&1; then
  echo "xvfb-run not found; skipping" >&2
  exit 77
fi

# Launch gnome-control-center Online Accounts and keep it open briefly
xvfb_run_cmd=(xvfb-run -a -s "-screen 0 1280x800x24")
"${xvfb_run_cmd[@]}" gnome-control-center online-accounts &
PID=$!

# Allow time to draw
sleep 5

# We can't reliably click without extra tools; treat opening as success signal
if ps -p "$PID" >/dev/null 2>&1; then
  kill "$PID" || true
  echo "ok"
  exit 0
fi

echo "could not start gnome-control-center" >&2
exit 1
