#!/usr/bin/env bash
# Run loop gate under GDB with debuginfod to get fully symbolized backtraces
set -euo pipefail

# Enable debuginfod for Ubuntu's debug symbol server
export DEBUGINFOD_URLS="https://debuginfod.ubuntu.com"

# Environment setup for crash reproduction
export G_MESSAGES_DEBUG=all
export GLIBC_TUNABLES=glibc.malloc.tcache_count=0:glibc.malloc.tcache_max=0
export MALLOC_CHECK_=3
export MALLOC_PERTURB_=165
export GNOSTR_STRESS_SCROLL=1
export G_SLICE=always-malloc
export G_DEBUG=gc-friendly,fatal-criticals

DURATION=${1:-360}
MAX_RUNS=${2:-10}

echo "=========================================="
echo "GDB LOOP GATE RUNNER WITH DEBUGINFOD"
echo "=========================================="
echo "Duration per run: ${DURATION}s"
echo "Max runs: ${MAX_RUNS}"
echo "Debuginfod: ${DEBUGINFOD_URLS}"
echo ""

runs=0
while [ $runs -lt $MAX_RUNS ]; do
  runs=$((runs + 1))
  echo "=========================================="
  echo "Run $runs of $MAX_RUNS"
  echo "=========================================="
  
  # Run under GDB with automatic backtrace on crash
  timeout -s INT ${DURATION} gdb -batch \
    -ex "set debuginfod enabled on" \
    -ex "set pagination off" \
    -ex "set print frame-arguments none" \
    -ex "set logging file gdb_crash_${runs}.log" \
    -ex "set logging on" \
    -ex "run" \
    -ex "echo \n===== CRASH DETECTED =====\n" \
    -ex "info threads" \
    -ex "thread apply all bt 30" \
    -ex "echo \n===== DETAILED BACKTRACE =====\n" \
    -ex "bt full 40" \
    -ex "set logging off" \
    -ex "quit" \
    --args ./_build/apps/gnostr/gnostr \
    2>&1 | tee gdb_run_${runs}.log || true
  
  # Check if we got a crash
  if grep -q "CRASH DETECTED" gdb_run_${runs}.log 2>/dev/null; then
    echo ""
    echo "=========================================="
    echo "CRASH CAPTURED WITH SYMBOLS!"
    echo "=========================================="
    echo "Log: gdb_crash_${runs}.log"
    echo "Full output: gdb_run_${runs}.log"
    exit 0
  fi
  
  echo "Run $runs completed cleanly"
  sleep 1
done

echo ""
echo "=========================================="
echo "Completed $MAX_RUNS runs without crash"
echo "=========================================="
