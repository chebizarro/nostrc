#!/bin/bash
#
# Install git hooks for nostrc
#
# Usage: scripts/install-hooks.sh

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"
HOOKS_DIR="$REPO_ROOT/.git/hooks"

echo "Installing git hooks..."

# Install pre-push hook
if [ -f "$SCRIPT_DIR/pre-push" ]; then
    cp "$SCRIPT_DIR/pre-push" "$HOOKS_DIR/pre-push"
    chmod +x "$HOOKS_DIR/pre-push"
    echo "  ✓ pre-push hook installed"
else
    echo "  ✗ pre-push hook not found in scripts/"
    exit 1
fi

echo "Done. Hooks installed in .git/hooks/"
