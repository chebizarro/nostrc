#!/usr/bin/env bash
set -euo pipefail
# Thin shim to match prompt's directory layout; forwards to integration script.
exec bash "$(dirname "$0")/../integration/run_nostrfs_basic.sh"
