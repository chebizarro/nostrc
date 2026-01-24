#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$PROJECT_ROOT/build}"
GNOSTR_BIN="${GNOSTR_BIN:-$BUILD_DIR/apps/gnostr/gnostr}"

BROADWAY_PORT="${BROADWAY_PORT:-8080}"
BROADWAY_DISPLAY="${BROADWAY_DISPLAY:-5}"

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

echo "Starting Broadway daemon on :$BROADWAY_DISPLAY (port $BROADWAY_PORT)..."
$BROADWAYD :$BROADWAY_DISPLAY --port $BROADWAY_PORT --address 127.0.0.1 &
BROADWAYD_PID=$!

cleanup() {
    echo "Stopping Broadway daemon..."
    kill $BROADWAYD_PID 2>/dev/null || true
    wait $BROADWAYD_PID 2>/dev/null || true
}
trap cleanup EXIT INT TERM

sleep 1

echo "Broadway server running at http://127.0.0.1:$BROADWAY_PORT"
echo "Starting gnostr..."
echo ""

export GDK_BACKEND=broadway
export BROADWAY_DISPLAY=:$BROADWAY_DISPLAY
export GSETTINGS_SCHEMA_DIR="$BUILD_DIR/apps/gnostr"

"$GNOSTR_BIN" "$@"
