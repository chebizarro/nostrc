#!/bin/sh
# SPDX-License-Identifier: MIT
set -eu
case "${1:-}" in upgrade|rollback-image|rollback-state) ;; *)
  echo 'usage: release.sh upgrade|rollback-image|rollback-state /opt/signet/releases/<commit> [--confirm RESTORE-REVOKED-AUTHORITY]' >&2
  exit 2;;
esac
exec python3 "$(dirname "$0")/ops.py" "$@"
