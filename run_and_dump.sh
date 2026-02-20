#!/usr/bin/env bash
# run_and_dump.sh - Run repro.sh and capture backtrace from any core dump
# Produces bt.core.*.txt files for each crash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BIN="./build/relteeth/apps/gnostr/gnostr"

echo "=== Heap Corruption Debug Loop ==="
echo "=== $(date -Iseconds) ==="
echo ""

# Run the reproduction script
"$SCRIPT_DIR/repro.sh" || true

# Find the newest core dump
core="$(ls -t core.* 2>/dev/null | head -n 1 || true)"

if [[ -n "${core}" ]]; then
  echo ""
  echo "=== Found core: ${core} ==="
  outfile="bt.${core}.txt"
  
  "$SCRIPT_DIR/tools/btcore.sh" "$BIN" "${core}" | tee "$outfile"
  
  echo ""
  echo "=== Backtrace saved to: $outfile ==="
  echo "=== Core file: $core ==="
else
  echo ""
  echo "=== No core found ==="
  echo "Check: ulimit -c unlimited"
  echo "Check: cat /proc/sys/kernel/core_pattern"
  echo ""
  echo "To enable core dumps:"
  echo "  ulimit -c unlimited"
  echo "  sudo sysctl -w kernel.core_pattern=core.%e.%p.%t"
fi
