#!/usr/bin/env bash
set -euo pipefail

if ! command -v dbus-run-session >/dev/null 2>&1; then
  echo "dbus-run-session not found; skipping" >&2
  exit 77
fi

dbus-run-session -- bash -lc "$*"
