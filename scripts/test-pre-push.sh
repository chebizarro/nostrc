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
cat > "$repo/scripts/full-gate.sh" <<'FULL'
#!/bin/sh
printf 'FULL_GATE_RAN args=[%s] stdin=[%s]\n' "$*" "$(cat)"
FULL
chmod +x "$repo/scripts/full-gate.sh"
cat > "$repo/scripts/test-gate.sh" <<'GATE'
#!/bin/bash
set -eu
printf 'SELECTIVE %s\n' "$*"
while [ $# -gt 0 ]; do
    if [ "$1" = --refs-file ]; then
        test -s "$2"
        cat "$2"
        break
    fi
    shift
done
exit "${GATE_RC:-0}"
GATE
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
NOSTRC_SELECTIVE_GATE=1 hook <<< "refs/heads/m $clean refs/heads/m $base" > "$tmp/out" 2>&1
grep -q 'SELECTIVE .*--pre-push --refs-file' "$tmp/out"
grep -q "refs/heads/m $clean refs/heads/m $base" "$tmp/out"
NOSTRC_SELECTIVE_GATE=1 NOSTRC_SKIP_PRE_PUSH=1 hook <<< "refs/heads/m $clean refs/heads/m $base" > "$tmp/out" 2>&1
grep -q SKIPPED "$tmp/out"
grep -q 'SELECTIVE .*--explain' "$tmp/out"
# Generic zero OID recognition is not tied to SHA-1 length.
echo "(delete) $(printf '%064d' 0) refs/heads/gone $clean" | hook > /dev/null
if git -C "$repo" worktree list | grep -q /checkout; then echo "temporary checkout left behind" >&2; exit 1; fi

# W12 readiness guards (nostrc-0cfsg.37): the legacy static hook stays the
# default, the shared diagnostic and the full gate are explicit, and unsetting
# the opt-in is the whole rollback. These pin today's contract; they do not
# authorize or perform a selective-default cutover.
refs_clean="refs/heads/m $clean refs/heads/m $base"
refs_ws="refs/heads/m $ws refs/heads/m $clean"
for value in unset 0 "" true yes 2; do
    if [ "$value" = unset ]; then
        hook <<< "$refs_clean" > "$tmp/out" 2>&1
    else
        NOSTRC_SELECTIVE_GATE="$value" hook <<< "$refs_clean" > "$tmp/out" 2>&1
    fi
    if grep -q SELECTIVE "$tmp/out"; then echo "diagnostic ran for NOSTRC_SELECTIVE_GATE=$value" >&2; exit 1; fi
done
# Only the exact opt-in value enables it; the rollback is unsetting it again.
NOSTRC_SELECTIVE_GATE=1 hook <<< "$refs_clean" > "$tmp/out" 2>&1
grep -q SELECTIVE "$tmp/out"
hook <<< "$refs_clean" > "$tmp/out" 2>&1
if grep -q SELECTIVE "$tmp/out"; then echo "diagnostic persisted after opt-in removed" >&2; exit 1; fi

# A failing opt-in diagnostic blocks the push, also as the plan-only
# companion of an announced static skip; it never masks a static failure.
if NOSTRC_SELECTIVE_GATE=1 GATE_RC=3 hook <<< "$refs_clean" > "$tmp/out" 2>&1; then
    echo "failed shared diagnostic did not block the push" >&2; exit 1
fi
grep -q 'SELECTIVE .*--pre-push --refs-file' "$tmp/out"
if NOSTRC_SELECTIVE_GATE=1 NOSTRC_SKIP_PRE_PUSH=1 GATE_RC=3 hook <<< "$refs_clean" > "$tmp/out" 2>&1; then
    echo "failed plan-only diagnostic did not block the push" >&2; exit 1
fi
if NOSTRC_SELECTIVE_GATE=1 hook <<< "$refs_ws" > "$tmp/out" 2>&1; then
    echo "diagnostic opt-in bypassed a static failure" >&2; exit 1
fi
grep -q "whitespace errors" "$tmp/out"
if grep -q SELECTIVE "$tmp/out"; then echo "diagnostic ran after static failure" >&2; exit 1; fi

# The full override wins over the static skip and the diagnostic opt-in, and
# receives git's arguments and the pushed ref lines unchanged.
for env_name in NOSTRC_SKIP_PRE_PUSH NOSTRC_SELECTIVE_GATE; do
    env NOSTRC_FULL_GATE=1 "$env_name=1" bash -c 'cd "$1" && shift && scripts/pre-push "$@"' _ "$repo" origin url \
        <<< "$refs_clean" > "$tmp/out" 2>&1
    grep -qF "FULL_GATE_RAN args=[origin url] stdin=[$refs_clean]" "$tmp/out"
    if grep -q -e SKIPPED -e SELECTIVE "$tmp/out"; then echo "full override ran a different path ($env_name)" >&2; exit 1; fi
done

# The documented contract stays in step with the hook: legacy default, the
# opt-in diagnostic is additive, and the full-gate override exists.
root="$(cd "$scripts/.." && pwd)"
for doc in AGENTS.md docs/TESTING.md; do
    grep -q 'NOSTRC_SELECTIVE_GATE=1' "$root/$doc" || { echo "$doc lost the diagnostic opt-in" >&2; exit 1; }
done
grep -q 'NOSTRC_FULL_GATE=1' "$root/AGENTS.md" || { echo "AGENTS.md lost the full-gate override" >&2; exit 1; }
grep -qi 'never replaces' "$root/AGENTS.md" || { echo "AGENTS.md lost the additive-only rule" >&2; exit 1; }
grep -qi 'not a selective-default switch' "$root/docs/TESTING.md" || { echo "TESTING.md lost the no-default-switch statement" >&2; exit 1; }
echo "pre-push tests passed"
