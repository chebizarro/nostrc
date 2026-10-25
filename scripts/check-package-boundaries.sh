#!/usr/bin/env bash
# Static package guard; --build-dir also checks real object exports with nm.
set -euo pipefail
root=$(git -C "$(dirname "$0")" rev-parse --show-toplevel)
exec python3 "$root/scripts/check-package-boundaries.py" --root "$root" "$@"
