#!/usr/bin/env bash
# Loop gate runner - converts intermittent crashes into deterministic signal
# Runs app repeatedly until crash detected or N clean runs

set -euo pipefail

# Environment setup
export G_MESSAGES_DEBUG=all
export GLIBC_TUNABLES=glibc.malloc.tcache_count=0:glibc.malloc.tcache_max=0
export MALLOC_CHECK_=3
export MALLOC_PERTURB_=165
export GNOSTR_STRESS_SCROLL=1

# Leave fatal-warnings OFF - we only want it when trapping a specific warning
unset G_DEBUG || true

# Test duration per run (6 minutes = 360 seconds)
DURATION=${1:-360}
MAX_CLEAN_RUNS=${2:-100}

echo "=========================================="
echo "LOOP GATE RUNNER"
echo "=========================================="
echo "Duration per run: ${DURATION}s"
echo "Max clean runs: ${MAX_CLEAN_RUNS}"
echo "Will stop on first crash signature detected"
echo ""

runs=0
clean_runs=0

while [ $clean_runs -lt $MAX_CLEAN_RUNS ]; do
  runs=$((runs+1))
  echo "=== RUN $runs (clean: $clean_runs) ==="
  logfile="gate_run_${runs}.log"

  # Run with timeout and SIGINT for graceful shutdown
  timeout -s INT ${DURATION} ./_build/apps/gnostr/gnostr \
    > >(stdbuf -oL tee "$logfile") 2>&1 || true

  echo ""
  echo "Analyzing run $runs..."

  # Check for profile cache corruption (PRIMARY TARGET)
  if grep -q "\[PROFILE_CACHE\] CORRUPTION DETECTED" "$logfile"; then
    echo ""
    echo "=========================================="
    echo "HIT: PROFILE CACHE CORRUPTION"
    echo "=========================================="
    echo "Log file: $logfile"
    echo "Run: $runs"
    echo ""
    echo "Next steps:"
    echo "  1. Run Experiment A: GNOSTR_PROFILE_CACHE_MAX=1000000 ./scripts/fence_gate_test.sh"
    echo "  2. Run Experiment B: GNOSTR_PROFILE_CACHE_LEAK=1 ./scripts/fence_gate_test.sh"
    echo ""
    grep -A 20 "\[PROFILE_CACHE\] CORRUPTION" "$logfile" || true
    exit 0
  fi

  # Check for NoteCardRow finalize warning (SECONDARY TARGET)
  if grep -q "Finalizing NostrGtkNoteCardRow.*still has children" "$logfile"; then
    echo ""
    echo "=========================================="
    echo "HIT: NOTECARD ROW FINALIZE WARNING"
    echo "=========================================="
    echo "Log file: $logfile"
    echo "Run: $runs"
    echo ""
    echo "Next steps:"
    echo "  1. Check dump_children logs in $logfile"
    echo "  2. Rerun with: export G_DEBUG=fatal-warnings,fatal-criticals && ./_build/apps/gnostr/gnostr"
    echo ""
    grep -B 5 -A 5 "Finalizing NostrGtkNoteCardRow" "$logfile" | head -30 || true
    exit 0
  fi

  # Check for heap/refcount signatures
  if grep -Eq "malloc_consolidate|unaligned fastbin|G_IS_OBJECT.*failed|g_atomic_ref_count_dec.*0" "$logfile"; then
    echo ""
    echo "=========================================="
    echo "HIT: HEAP/REFCOUNT SIGNATURE"
    echo "=========================================="
    echo "Log file: $logfile"
    echo "Run: $runs"
    echo ""
    grep -E "malloc_consolidate|unaligned fastbin|G_IS_OBJECT|g_atomic_ref_count" "$logfile" | head -20 || true
    exit 0
  fi

  # Check for segfault
  if grep -q "Segmentation fault\|dumped core" "$logfile"; then
    echo ""
    echo "=========================================="
    echo "HIT: SEGMENTATION FAULT"
    echo "=========================================="
    echo "Log file: $logfile"
    echo "Run: $runs"
    echo ""
    tail -50 "$logfile" || true
    exit 0
  fi

  clean_runs=$((clean_runs+1))
  echo "RUN $runs: CLEAN ($clean_runs/$MAX_CLEAN_RUNS)"
  echo ""
done

echo ""
echo "=========================================="
echo "GATE PASSED: $MAX_CLEAN_RUNS CLEAN RUNS"
echo "=========================================="
echo "Total runs: $runs"
echo "No crashes detected"
exit 0
