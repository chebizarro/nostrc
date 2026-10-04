#!/usr/bin/env bash
# Capture the isolated Xvfb desktop after each manual acceptance step.
set -euo pipefail
: "${W27_RUN_ID:?Set W27_RUN_ID}"
[[ $# -eq 1 && $1 =~ ^[A-Za-z0-9_-]+$ ]] || {
  echo "usage: W27_RUN_ID=... $0 <step-label>" >&2; exit 2;
}
run_dir=${W27_RUN_DIR:-"$HOME/gh-run/$W27_RUN_ID"}
mkdir -p "$run_dir/screenshots"
file="$run_dir/screenshots/$(date -u +%Y%m%dT%H%M%SZ)-$1.png"
DISPLAY=${W27_DISPLAY:-:99} import -window root "$file"
printf '%s\n' "$file"
