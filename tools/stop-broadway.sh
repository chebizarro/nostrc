#!/bin/bash
set -e

BROADWAY_DISPLAY="${BROADWAY_DISPLAY:-5}"
BROADWAY_PIDFILE="/tmp/broadway-${BROADWAY_DISPLAY}.pid"

if [ ! -f "$BROADWAY_PIDFILE" ]; then
    echo "No Broadway daemon PID file found at $BROADWAY_PIDFILE"
    echo "Broadway daemon may not be running, or was started manually."
    exit 1
fi

BROADWAY_PID=$(cat "$BROADWAY_PIDFILE")

if ! kill -0 "$BROADWAY_PID" 2>/dev/null; then
    echo "Broadway daemon (PID $BROADWAY_PID) is not running (stale PID file)"
    rm -f "$BROADWAY_PIDFILE"
    exit 1
fi

echo "Stopping Broadway daemon (PID $BROADWAY_PID)..."
kill "$BROADWAY_PID"

# Wait for process to exit
for i in {1..10}; do
    if ! kill -0 "$BROADWAY_PID" 2>/dev/null; then
        echo "Broadway daemon stopped successfully"
        rm -f "$BROADWAY_PIDFILE"
        exit 0
    fi
    sleep 0.5
done

# Force kill if still running
if kill -0 "$BROADWAY_PID" 2>/dev/null; then
    echo "Broadway daemon did not stop gracefully, force killing..."
    kill -9 "$BROADWAY_PID" 2>/dev/null || true
    sleep 1
fi

rm -f "$BROADWAY_PIDFILE"
echo "Broadway daemon stopped"
