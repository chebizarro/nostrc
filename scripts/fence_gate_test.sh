#!/bin/bash
# Gate test for generation fencing - proper logging and exit code handling

set -e
set -o pipefail

# Test duration in seconds (default 10 minutes)
DURATION=${1:-600}

# Output log file
LOG_FILE="fence_test_$(date +%Y%m%d_%H%M%S).log"

echo "=========================================="
echo "GENERATION FENCING GATE TEST"
echo "=========================================="
echo "Duration: ${DURATION}s ($(($DURATION / 60)) minutes)"
echo "Log file: ${LOG_FILE}"
echo "Scenario: Stress scroll with thread view open"
echo ""
echo "Environment:"
echo "  G_DEBUG=fatal-criticals"
echo "  GNOSTR_STRESS_SCROLL=1"
echo "  MALLOC_CHECK_=3"
echo "  MALLOC_PERTURB_=165"
echo "  GLIBC_TUNABLES=glibc.malloc.tcache_count=0:glibc.malloc.tcache_max=0"
echo ""
echo "Victory condition: Many [FENCE] stale drops, zero crashes"
echo "=========================================="
echo ""

# Set up environment
export G_DEBUG=fatal-criticals
export GNOSTR_STRESS_SCROLL=1
export MALLOC_CHECK_=3
export MALLOC_PERTURB_=165
export GLIBC_TUNABLES=glibc.malloc.tcache_count=0:glibc.malloc.tcache_max=0

# Run test with timeout and capture full output
echo "Starting test..."
timeout --signal=TERM ${DURATION} ./_build/apps/gnostr/gnostr 2>&1 | tee "${LOG_FILE}"
EXIT_CODE=$?

echo ""
echo "=========================================="
echo "TEST COMPLETE"
echo "=========================================="
echo "Exit code: ${EXIT_CODE}"
echo ""

# Interpret exit code
if [ ${EXIT_CODE} -eq 0 ]; then
    echo "✅ Clean exit (app closed normally)"
elif [ ${EXIT_CODE} -eq 143 ]; then
    echo "✅ SIGTERM exit (timeout reached, no crash)"
elif [ ${EXIT_CODE} -eq 137 ]; then
    echo "⚠️  SIGKILL exit (timeout with --signal=KILL)"
else
    echo "❌ CRASH detected (exit code ${EXIT_CODE})"
fi

echo ""
echo "=========================================="
echo "LOG ANALYSIS"
echo "=========================================="

# Count fence drops
FENCE_COUNT=$(grep -c "\[FENCE\]" "${LOG_FILE}" || true)
echo "Fence drops: ${FENCE_COUNT}"

# Show fence breakdown by component
echo ""
echo "Fence drops by component:"
grep "\[FENCE\]" "${LOG_FILE}" | sed 's/.*\[FENCE\]\[//' | sed 's/\].*//' | sort | uniq -c | sort -rn || true

# Check for crash indicators
echo ""
echo "Crash indicators:"
MALLOC_ERRORS=$(grep -c "malloc_consolidate\|corrupted\|double free" "${LOG_FILE}" || true)
GLIB_ERRORS=$(grep -c "G_IS_OBJECT\|g_atomic_ref_count_dec\|GLib-GObject-CRITICAL" "${LOG_FILE}" || true)
SEGFAULTS=$(grep -c "SIGSEGV\|Segmentation fault" "${LOG_FILE}" || true)

echo "  malloc errors: ${MALLOC_ERRORS}"
echo "  GLib errors: ${GLIB_ERRORS}"
echo "  segfaults: ${SEGFAULTS}"

TOTAL_ERRORS=$((MALLOC_ERRORS + GLIB_ERRORS + SEGFAULTS))

echo ""
echo "=========================================="
echo "VERDICT"
echo "=========================================="

if [ ${EXIT_CODE} -eq 0 ] || [ ${EXIT_CODE} -eq 143 ]; then
    if [ ${TOTAL_ERRORS} -eq 0 ]; then
        if [ ${FENCE_COUNT} -gt 0 ]; then
            echo "🎉 VICTORY: ${FENCE_COUNT} stale drops, zero crashes!"
        else
            echo "✅ PASS: No crashes (but no fence drops observed - may need more stress)"
        fi
    else
        echo "⚠️  PARTIAL: No crash exit, but ${TOTAL_ERRORS} errors in logs"
    fi
else
    echo "❌ FAIL: Crashed with exit code ${EXIT_CODE}"
fi

echo ""
echo "Full log saved to: ${LOG_FILE}"
echo ""

# Exit with test result
if [ ${EXIT_CODE} -eq 0 ] || [ ${EXIT_CODE} -eq 143 ]; then
    if [ ${TOTAL_ERRORS} -eq 0 ]; then
        exit 0
    else
        exit 1
    fi
else
    exit 1
fi
