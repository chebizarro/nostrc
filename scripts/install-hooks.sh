#!/bin/bash
# Install static-only push checks without replacing an active Beads hook.
# Legacy BUILD VERIFICATION markers are retained solely for installer compatibility.

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
elif [[ "$(git -C "$REPO_ROOT" hash-object "$HOOK")" = 2c541d0c442c52980e4ab3513d12b39267ba4c2c ||
        "$(git -C "$REPO_ROOT" hash-object "$HOOK")" = 10e898a4c13f0ab91e476b506def52eb9e2b357e ]]; then
    # Exact former repository copies only; never overwrite a customized hook.
    cp "$SOURCE" "$HOOK"
    result="upgraded known standalone hook"
elif grep -Fq 'BEGIN BEADS INTEGRATION' "$HOOK"; then
    legacy='"$nostrc_root/scripts/pre-push" "$@"'
    replay='"$nostrc_root/scripts/pre-push" "$@" < "$nostrc_push_input"'
    if grep -Fq 'BEGIN NOSTRC BUILD VERIFICATION' "$HOOK" &&
       ! grep -Fxq "$legacy" "$HOOK" && ! grep -Fxq "$replay" "$HOOK"; then
        echo "Refusing to modify unknown build chain in $HOOK" >&2
        exit 1
    fi
    if grep -Fq 'BEGIN NOSTRC PUSH INPUT REPLAY' "$HOOK" &&
       grep -Fxq "$replay" "$HOOK"; then
        result="already installed"
    else
        first_line="$(head -n 1 "$HOOK")"
        case "$first_line" in
            '#!'*) ;;
            *) echo "Refusing hook without shebang at $HOOK" >&2; exit 1 ;;
        esac
        body="$(mktemp "$HOOK.XXXXXX")"
        replacement="$(mktemp "$HOOK.XXXXXX")"
        trap 'rm -f "$body" "$replacement"' EXIT
        awk '
            $0 == "\"$nostrc_root/scripts/pre-push\" \"$@\"" {
                print "\"$nostrc_root/scripts/pre-push\" \"$@\" < \"$nostrc_push_input\""
                next
            }
            { print }
        ' "$HOOK" > "$body"
        if grep -Fq 'BEGIN NOSTRC PUSH INPUT REPLAY' "$body"; then
            cat "$body" > "$replacement"
        else
            printf '%s\n' "$first_line" > "$replacement"
            cat >> "$replacement" <<'REPLAY'
# --- BEGIN NOSTRC PUSH INPUT REPLAY ---
nostrc_push_input="$(mktemp)" || exit 1
trap 'rm -f "$nostrc_push_input"' EXIT
cat > "$nostrc_push_input" || exit 1
exec < "$nostrc_push_input"
# --- END NOSTRC PUSH INPUT REPLAY ---
REPLAY
            tail -n +2 "$body" >> "$replacement"
        fi
        if ! grep -Fq 'BEGIN NOSTRC BUILD VERIFICATION' "$replacement"; then
            cat >> "$replacement" <<'BUILD'

# --- BEGIN NOSTRC BUILD VERIFICATION ---
nostrc_root="$(git rev-parse --show-toplevel)" || exit 1
"$nostrc_root/scripts/pre-push" "$@" < "$nostrc_push_input"
# --- END NOSTRC BUILD VERIFICATION ---
BUILD
            result="chained after Beads hook"
        else
            result="upgraded Beads hook chain"
        fi
        chmod 755 "$replacement"
        mv "$replacement" "$HOOK"
        rm -f "$body"
        trap - EXIT
    fi
else
    echo "Refusing to replace existing pre-push hook at $HOOK" >&2
    exit 1
fi
chmod +x "$HOOK"
echo "Pre-push static checks $result in $HOOK"
