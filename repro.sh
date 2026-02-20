#!/usr/bin/env bash
# repro.sh - Run gnostr with heap corruption detection enabled
# Edit BIN and ARGS to match your crash scenario
set -euo pipefail

# === CONFIGURATION ===
# Use the "relteeth" build (RelWithDebInfo + frame pointers + no-strict-aliasing)
BIN="./build/relteeth/apps/gnostr/gnostr"
ARGS=()

# === HEAP CORRUPTION DETECTION ===
# MALLOC_CHECK_=3: abort on any heap corruption detected
# MALLOC_PERTURB_=165: fill freed memory with 0xA5 pattern (catches use-after-free)
export MALLOC_CHECK_=3
export MALLOC_PERTURB_=165

# === REPRODUCIBILITY ===
export TZ=UTC
export LC_ALL=C

# === OPTIONAL: GTK/GLib debugging ===
# Uncomment to enable additional GLib checks
# export G_DEBUG=fatal-criticals
# export G_SLICE=always-malloc

# === RUN ===
# taskset -c 0: pin to CPU 0 for consistent timing
# setarch -R: disable ASLR for reproducible addresses
echo "=== Running: $BIN ${ARGS[*]:-}"
echo "=== MALLOC_CHECK_=$MALLOC_CHECK_ MALLOC_PERTURB_=$MALLOC_PERTURB_"
echo "=== Pinned to CPU 0, ASLR disabled"
echo ""

taskset -c 0 setarch "$(uname -m)" -R "$BIN" "${ARGS[@]}"
