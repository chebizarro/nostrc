#!/bin/bash
#
# Diagnostic script to monitor gnostr thread count and profile fetching
#

echo "Starting gnostr diagnostics..."
echo "================================"
echo ""

# Start gnostr in background
echo "Starting GNOSTR_LIVE=TRUE ./build/apps/gnostr/gnostr..."
cd /home/bizarro/Documents/Projects/nostrc
GNOSTR_LIVE=TRUE ./build/apps/gnostr/gnostr > /tmp/gnostr_diag.log 2>&1 &
GNOSTR_PID=$!

echo "PID: $GNOSTR_PID"
echo ""

# Monitor for 30 seconds
for i in {1..30}; do
    sleep 1
    
    # Check if still running
    if ! kill -0 $GNOSTR_PID 2>/dev/null; then
        echo "❌ App crashed at ${i}s!"
        break
    fi
    
    # Count threads
    THREADS=$(ps -eLf | grep -w $GNOSTR_PID | wc -l)
    
    # Check for key events
    EOSE_COUNT=$(grep -c "EOSE received" /tmp/gnostr_diag.log)
    PROFILE_COUNT=$(grep -c "PROFILE.*Fetching" /tmp/gnostr_diag.log)
    GOROUTINE_COMPLETE=$(grep -c "GOROUTINE.*Complete" /tmp/gnostr_diag.log)
    
    printf "[%2ds] Threads: %3d | EOSE: %2d | Profiles: %2d | Goroutines done: %2d\n" \
           $i $THREADS $EOSE_COUNT $PROFILE_COUNT $GOROUTINE_COMPLETE
    
    # Alert if threads explode
    if [ $THREADS -gt 100 ]; then
        echo "⚠️  WARNING: Thread count > 100!"
    fi
done

echo ""
echo "Stopping gnostr..."
kill $GNOSTR_PID 2>/dev/null
wait $GNOSTR_PID 2>/dev/null

echo ""
echo "=== Summary ==="
echo "Log file: /tmp/gnostr_diag.log"
echo ""
echo "EOSE messages:"
grep "EOSE received" /tmp/gnostr_diag.log | head -10
echo ""
echo "Profile fetches:"
grep "PROFILE.*Fetching" /tmp/gnostr_diag.log | head -5
echo ""
echo "Goroutine completions:"
grep "GOROUTINE.*Complete" /tmp/gnostr_diag.log | head -5
echo ""
echo "Errors:"
grep -i "error\|warning\|critical" /tmp/gnostr_diag.log | head -10
