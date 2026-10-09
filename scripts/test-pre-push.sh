#!/usr/bin/env bash
# The fast pre-push hook (scripts/pre-push): static checks only, the full
# gate behind NOSTRC_FULL_GATE=1. scripts/test-full-gate.sh tests the gate.
set -euo pipefail
scripts="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
repo="$tmp/repo"
mkdir -p "$repo/scripts"
git init -q "$repo"
git -C "$repo" config user.name Test
git -C "$repo" config user.email test@example.invalid
cp "$scripts/pre-push" "$repo/scripts/pre-push"
printf '#!/bin/sh\necho FULL_GATE_RAN\n' > "$repo/scripts/full-gate.sh"
chmod +x "$repo/scripts/full-gate.sh"
printf 'base\n' > "$repo/file"
git -C "$repo" add .
git -C "$repo" commit -qm base
base="$(git -C "$repo" rev-parse HEAD)"
printf 'clean\n' >> "$repo/file"
git -C "$repo" commit -qam clean
clean="$(git -C "$repo" rev-parse HEAD)"
printf 'trailing   \n' >> "$repo/file"
git -C "$repo" commit -qam ws
ws="$(git -C "$repo" rev-parse HEAD)"
z=0000000000000000000000000000000000000000
hook() { (cd "$repo" && scripts/pre-push); }

echo "refs/heads/m $clean refs/heads/m $base" | hook > "$tmp/out" 2>&1 || { cat "$tmp/out"; echo "clean push blocked" >&2; exit 1; }
if echo "refs/heads/m $ws refs/heads/m $clean" | hook > "$tmp/out" 2>&1; then echo "whitespace error passed" >&2; exit 1; fi
grep -q "whitespace errors" "$tmp/out"
NOSTRC_SKIP_PRE_PUSH=1 hook <<< "refs/heads/m $ws refs/heads/m $clean" > "$tmp/out" 2>&1
grep -q SKIPPED "$tmp/out"
echo "(delete) $z refs/heads/gone $clean" | hook > /dev/null
if hook < /dev/null > "$tmp/out" 2>&1; then echo "empty input accepted" >&2; exit 1; fi
grep -q "No pre-push refs received" "$tmp/out"
echo "refs/heads/m $clean refs/heads/m $base" | NOSTRC_FULL_GATE=1 hook > "$tmp/out" 2>&1
grep -q FULL_GATE_RAN "$tmp/out"
if git -C "$repo" worktree list | grep -q /checkout; then echo "temporary checkout left behind" >&2; exit 1; fi
echo "pre-push tests passed"
