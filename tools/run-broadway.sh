#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$PROJECT_ROOT/build}"
GNOSTR_BIN="${GNOSTR_BIN:-$BUILD_DIR/apps/gnostr/gnostr}"

BROADWAY_PORT="${BROADWAY_PORT:-8080}"
BROADWAY_DISPLAY="${BROADWAY_DISPLAY:-5}"
BROADWAY_PIDFILE="/tmp/broadway-${BROADWAY_DISPLAY}.pid"

if [ ! -f "$GNOSTR_BIN" ]; then
    echo "Error: gnostr binary not found at $GNOSTR_BIN"
    echo "Build the project first or set GNOSTR_BIN environment variable"
    exit 1
fi

BROADWAYD=$(command -v gtk4-broadwayd || command -v broadwayd || true)
if [ -z "$BROADWAYD" ]; then
    echo "Error: gtk4-broadwayd or broadwayd not found in PATH"
    exit 1
fi

# Check if Broadway daemon is already running
if [ -f "$BROADWAY_PIDFILE" ]; then
    EXISTING_PID=$(cat "$BROADWAY_PIDFILE")
    if kill -0 "$EXISTING_PID" 2>/dev/null; then
        echo "Broadway daemon already running (PID $EXISTING_PID) at http://127.0.0.1:$BROADWAY_PORT"
    else
        echo "Stale PID file found, removing..."
        rm -f "$BROADWAY_PIDFILE"
    fi
fi

# Start Broadway daemon if not running (detached, survives script exit)
if [ ! -f "$BROADWAY_PIDFILE" ] || ! kill -0 "$(cat "$BROADWAY_PIDFILE")" 2>/dev/null; then
    echo "Starting persistent Broadway daemon on :$BROADWAY_DISPLAY (port $BROADWAY_PORT)..."
    nohup $BROADWAYD :$BROADWAY_DISPLAY --port $BROADWAY_PORT --address 127.0.0.1 >/dev/null 2>&1 &
    BROADWAYD_PID=$!
    echo $BROADWAYD_PID > "$BROADWAY_PIDFILE"
    sleep 1
    echo "Broadway daemon started (PID $BROADWAYD_PID) at http://127.0.0.1:$BROADWAY_PORT"
    echo "Use tools/stop-broadway.sh to stop the daemon when done."
fi

echo ""
echo "Starting gnostr (Broadway at http://127.0.0.1:$BROADWAY_PORT)..."
echo "Note: Broadway daemon will persist after gnostr exits (for rebuild/debug cycles)"
echo ""

export GDK_BACKEND=broadway
export BROADWAY_DISPLAY=:$BROADWAY_DISPLAY
export GSETTINGS_SCHEMA_DIR="$BUILD_DIR/apps/gnostr"

# Run gnostr (Broadway daemon persists after this exits)
"$GNOSTR_BIN" "$@"
