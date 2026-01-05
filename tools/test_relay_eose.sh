#!/bin/bash
#
# Test relay EOSE compliance per NIP-01 spec
# 
# According to NIP-01, relays MUST send EOSE for ALL subscriptions,
# regardless of filter type or event kind.
#
# Requires: websocat (install with: cargo install websocat)
#

set -euo pipefail

RELAY_URL="${1:-wss://relay.damus.io}"
TEST_TYPE="${2:-kind0}"
TIMEOUT=10

echo "╔════════════════════════════════════════════════════════════╗"
echo "║         Nostr Relay EOSE Compliance Test (NIP-01)         ║"
echo "╚════════════════════════════════════════════════════════════╝"
echo ""
echo "🔍 Testing relay: $RELAY_URL"
echo "   Test type: $TEST_TYPE"
echo "   Timeout: ${TIMEOUT}s"
echo ""

# Check if websocat is installed
if ! command -v websocat &> /dev/null; then
    echo "❌ Error: websocat not found"
    echo "   Install with: cargo install websocat"
    echo "   Or: sudo apt install websocat"
    exit 1
fi

# Generate subscription based on test type
case "$TEST_TYPE" in
    kind0)
        FILTER='{"kinds":[0],"limit":5}'
        TEST_NAME="Kind 0 (Profile Metadata)"
        ;;
    kind1)
        FILTER='{"kinds":[1],"limit":5}'
        TEST_NAME="Kind 1 (Text Notes)"
        ;;
    empty)
        FILTER='{"limit":5}'
        TEST_NAME="Empty Filter (Any Kind)"
        ;;
    *)
        echo "❌ Unknown test type: $TEST_TYPE"
        echo "   Valid types: kind0, kind1, empty"
        exit 1
        ;;
esac

REQ_MSG='["REQ","test-sub-1",'$FILTER']'

echo "📤 Sending REQ: $REQ_MSG"
echo "⏳ Waiting for EOSE..."
echo ""

# Create a temporary file for output
TMPFILE=$(mktemp)
trap "rm -f $TMPFILE" EXIT

# Send REQ and capture output with timeout
START_TIME=$(date +%s)
EOSE_RECEIVED=0
EVENT_COUNT=0

(
    echo "$REQ_MSG"
    sleep $TIMEOUT
) | timeout $TIMEOUT websocat "$RELAY_URL" 2>/dev/null | while IFS= read -r line; do
    # Check for EOSE
    if echo "$line" | grep -q '"EOSE"'; then
        EOSE_TIME=$(date +%s)
        ELAPSED=$((EOSE_TIME - START_TIME))
        echo "✅ EOSE RECEIVED after ${ELAPSED}s"
        echo "$line"
        echo ""
        echo "EOSE_RECEIVED" > "$TMPFILE"
        break
    fi
    
    # Check for EVENT
    if echo "$line" | grep -q '"EVENT"'; then
        EVENT_COUNT=$((EVENT_COUNT + 1))
        echo "📨 Event $EVENT_COUNT received"
    fi
    
    # Check for other message types
    if echo "$line" | grep -q '"NOTICE"'; then
        echo "ℹ️  NOTICE: $line"
    fi
    if echo "$line" | grep -q '"CLOSED"'; then
        echo "⚠️  CLOSED: $line"
    fi
done

echo ""
echo "=== Test Results ==="
echo "Relay: $RELAY_URL"
echo "Test: $TEST_NAME"
echo "Events received: $EVENT_COUNT"

if [ -f "$TMPFILE" ] && grep -q "EOSE_RECEIVED" "$TMPFILE"; then
    echo "EOSE: ✅ RECEIVED"
    echo "Status: ✅ COMPLIANT with NIP-01"
else
    echo "EOSE: ❌ NOT RECEIVED (timeout after ${TIMEOUT}s)"
    echo "Status: ❌ VIOLATES NIP-01 spec"
fi
echo "==================="
echo ""

echo "💡 Tip: Test multiple relays to compare compliance:"
echo "   $0 wss://relay.damus.io kind0"
echo "   $0 wss://relay.primal.net kind0"
echo "   $0 wss://nos.lol kind0"
echo "   $0 wss://relay.sharegap.net kind0"
