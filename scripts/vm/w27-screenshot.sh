#!/usr/bin/env bash
# Capture the isolated Xvfb desktop after each manual acceptance step.
set -euo pipefail
# shellcheck source=scripts/vm/w27-paths.sh
source "$(dirname -- "${BASH_SOURCE[0]}")/w27-paths.sh"
[[ $# -eq 1 && $1 =~ ^[A-Za-z0-9_-]{1,64}$ ]] || {
  echo "usage: W27_RUN_ID=YYYYMMDDTHHMMSS $0 <step-label>" >&2; exit 2;
}
w27_init_run_paths
w27_run_subdir "$W27_RUN_PATH/screenshots"
display=${W27_DISPLAY:-:99}
[[ $display =~ ^:[0-9]+$ ]] || w27_fail 'W27_DISPLAY must be an X11 display like :99'
file="$W27_RUN_PATH/screenshots/$(date -u +%Y%m%dT%H%M%SZ)-$1.png"
w27_run_file "$file"
DISPLAY=$display import -window root "$file"
printf '%s\n' "$file"
