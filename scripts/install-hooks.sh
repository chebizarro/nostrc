#!/bin/bash
# Install the repository build gate without replacing an active Beads hook.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"
HOOKS_DIR="$(git -C "$REPO_ROOT" rev-parse --git-path hooks)"
case "$HOOKS_DIR" in
    /*) ;;
    *) HOOKS_DIR="$REPO_ROOT/$HOOKS_DIR" ;;
esac
HOOK="$HOOKS_DIR/pre-push"
SOURCE="$SCRIPT_DIR/pre-push"

if [ ! -f "$SOURCE" ] || [ ! -x "$SOURCE" ]; then
    echo "Executable pre-push hook not found in scripts/" >&2
    exit 1
fi
mkdir -p "$HOOKS_DIR"

if [ ! -f "$HOOK" ] || cmp -s "$SOURCE" "$HOOK"; then
    cp "$SOURCE" "$HOOK"
    result="installed"
elif grep -Fq 'BEGIN BEADS INTEGRATION' "$HOOK"; then
    if ! grep -Fq 'BEGIN NOSTRC BUILD VERIFICATION' "$HOOK"; then
        cat >> "$HOOK" <<'EOF'

# --- BEGIN NOSTRC BUILD VERIFICATION ---
nostrc_root="$(git rev-parse --show-toplevel)" || exit 1
"$nostrc_root/scripts/pre-push" "$@"
# --- END NOSTRC BUILD VERIFICATION ---
EOF
        result="chained after Beads hook"
    else
        result="already installed"
    fi
else
    echo "Refusing to replace existing pre-push hook at $HOOK" >&2
    exit 1
fi
chmod +x "$HOOK"
echo "Pre-push build verification $result in $HOOK"
