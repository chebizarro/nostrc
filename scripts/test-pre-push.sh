#!/usr/bin/env bash
# Static-only hook contract: no environment flag can launch a build/test runner.
set -euo pipefail
unset NOSTRC_FULL_GATE NOSTRC_SELECTIVE_GATE NOSTRC_SKIP_PRE_PUSH
scripts="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
repo="$tmp/repo"
mkdir -p "$repo/scripts" "$tmp/bin"
git init -q "$repo"
git -C "$repo" config user.name Test
git -C "$repo" config user.email test@example.invalid
cp "$scripts/pre-push" "$repo/scripts/pre-push"
export FORBIDDEN_TRACE="$tmp/forbidden"
cat > "$tmp/forbidden-runner" <<'FORBIDDEN'
#!/bin/sh
printf '%s\n' "$0 $*" >> "$FORBIDDEN_TRACE"
exit 93
FORBIDDEN
chmod +x "$tmp/forbidden-runner"
for tool in cmake ctest ninja make docker gcc clang cc go cargo meson; do
    cp "$tmp/forbidden-runner" "$tmp/bin/$tool"
done
for runner in full-gate.sh test-gate.sh linux-gate.sh; do
    cp "$tmp/forbidden-runner" "$repo/scripts/$runner"
done
# Permit only the actual static-check invocation, never Python test/planner work.
STATIC_PYTHON="$(command -v python3)"
export STATIC_PYTHON
cat > "$tmp/bin/python3" <<'PYTHON'
#!/bin/sh
case "${1:-}" in
    */scripts/check-unsequenced-args.py|*/scripts/check-gui-test-gdk-frame.py|*/scripts/check-linux-ci-packages.py|*/gnome/groundhog/tests/check_privacy.py)
        exec "$STATIC_PYTHON" "$@" ;;
    *) printf 'python3 %s\n' "$*" >> "$FORBIDDEN_TRACE"; exit 93 ;;
esac
PYTHON
chmod +x "$tmp/bin/python3"
export PATH="$tmp/bin:$PATH"
printf 'print("STATIC_GUARD_RAN")\n' > "$repo/scripts/check-unsequenced-args.py"
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
hook() { (cd "$repo" && scripts/pre-push "$@"); }
refs_clean="refs/heads/m $clean refs/heads/m $base"
refs_ws="refs/heads/m $ws refs/heads/m $clean"

hook <<< "$refs_clean" > "$tmp/out" 2>&1 || { cat "$tmp/out"; echo 'clean push blocked' >&2; exit 1; }
grep -q STATIC_GUARD_RAN "$tmp/out"
if hook <<< "$refs_ws" > "$tmp/out" 2>&1; then echo 'whitespace error passed' >&2; exit 1; fi
grep -q 'whitespace errors' "$tmp/out"
if hook < /dev/null > "$tmp/out" 2>&1; then echo 'empty input accepted' >&2; exit 1; fi
grep -q 'No pre-push refs received' "$tmp/out"
echo "(delete) $z refs/heads/gone $clean" | hook > /dev/null
echo "(delete) $(printf '%064d' 0) refs/heads/gone $clean" | hook > /dev/null

# Old switches are inert even together; only the explicit static skip is honored.
for full in 0 1; do
    for selective in 0 1; do
        for skip in 0 1; do
            NOSTRC_FULL_GATE="$full" NOSTRC_SELECTIVE_GATE="$selective" NOSTRC_SKIP_PRE_PUSH="$skip" \
                hook origin url <<< "$refs_clean" > "$tmp/out" 2>&1 || { cat "$tmp/out"; exit 1; }
            if [ "$skip" = 1 ]; then
                grep -q 'static checks SKIPPED' "$tmp/out"
                if grep -q STATIC_GUARD_RAN "$tmp/out"; then echo 'static skip ignored' >&2; exit 1; fi
            else
                grep -q STATIC_GUARD_RAN "$tmp/out"
                if NOSTRC_FULL_GATE="$full" NOSTRC_SELECTIVE_GATE="$selective" hook <<< "$refs_ws" > "$tmp/out" 2>&1; then
                    echo 'legacy flag bypassed whitespace failure' >&2; exit 1
                fi
            fi
        done
    done
done

# A dirty caller cannot change which candidate static checker is validated.
printf 'raise SystemExit(17)\n' > "$repo/scripts/check-unsequenced-args.py"
hook <<< "$refs_clean" > "$tmp/out" 2>&1
grep -q STATIC_GUARD_RAN "$tmp/out"
git -C "$repo" checkout -q "$clean" -- file
git -C "$repo" add scripts/check-unsequenced-args.py
git -C "$repo" commit -qm static-failure
bad="$(git -C "$repo" rev-parse HEAD)"
printf 'print("DIRTY_GUARD_MUST_NOT_RUN")\n' > "$repo/scripts/check-unsequenced-args.py"
if hook <<< "refs/heads/m $bad refs/heads/m $clean" > "$tmp/out" 2>&1; then
    echo 'candidate static failure passed' >&2; exit 1
fi
grep -q 'check-unsequenced-args.py failed' "$tmp/out"
if grep -q DIRTY_GUARD_MUST_NOT_RUN "$tmp/out"; then echo 'dirty caller checked' >&2; exit 1; fi
printf '%s\n%s\n' "$refs_clean" "(delete) $z refs/heads/gone $clean" | hook > /dev/null
if git -C "$repo" worktree list | grep -q /checkout; then echo 'temporary checkout left behind' >&2; exit 1; fi
if [ -e "$FORBIDDEN_TRACE" ]; then cat "$FORBIDDEN_TRACE"; echo 'hook launched forbidden work' >&2; exit 1; fi
echo 'pre-push static-only tests passed'
