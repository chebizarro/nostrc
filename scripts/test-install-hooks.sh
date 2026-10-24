#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
repo="$tmp/repo"
mkdir -p "$repo/scripts" "$repo/.beads/hooks"
git init -q "$repo"
cp "$SCRIPT_DIR/install-hooks.sh" "$repo/scripts/install-hooks.sh"
cat > "$repo/scripts/pre-push" <<'EOF'
#!/bin/sh
printf 'BUILD\n' >> "$TRACE"
cat >> "$TRACE"
EOF
chmod +x "$repo/scripts/pre-push"
cat > "$repo/.beads/hooks/pre-push" <<'EOF'
#!/bin/sh
# --- BEGIN BEADS INTEGRATION ---
printf 'BEADS\n' >> "$TRACE"
cat >> "$TRACE"
# --- END BEADS INTEGRATION ---
EOF
chmod +x "$repo/.beads/hooks/pre-push"

git -C "$repo" config core.hooksPath .beads/hooks
"$repo/scripts/install-hooks.sh"
second_result="$("$repo/scripts/install-hooks.sh")"
case "$second_result" in
    *'already installed'*) ;;
    *) echo 'Installer did not report an existing hook accurately' >&2; exit 1 ;;
esac
[ "$(grep -Fc 'BEGIN NOSTRC BUILD VERIFICATION' "$repo/.beads/hooks/pre-push")" -eq 1 ]
[ "$(grep -Fc 'BEGIN NOSTRC PUSH INPUT REPLAY' "$repo/.beads/hooks/pre-push")" -eq 1 ]
printf 'candidate-ref\n' | (cd "$repo" && TRACE="$tmp/trace" .beads/hooks/pre-push)
printf 'BEADS\ncandidate-ref\nBUILD\ncandidate-ref\n' | cmp - "$tmp/trace"

fail_repo="$tmp/fail-repo"
mkdir -p "$fail_repo/scripts" "$fail_repo/.beads/hooks"
git init -q "$fail_repo"
cp "$repo/scripts/"* "$fail_repo/scripts/"
cat > "$fail_repo/.beads/hooks/pre-push" <<'EOF'
#!/bin/sh
# --- BEGIN BEADS INTEGRATION ---
printf 'BEADS\n' >> "$TRACE"
cat >/dev/null
exit 1
# --- END BEADS INTEGRATION ---
EOF
chmod +x "$fail_repo/.beads/hooks/pre-push"
git -C "$fail_repo" config core.hooksPath .beads/hooks
"$fail_repo/scripts/install-hooks.sh"
if printf 'candidate-ref\n' | (cd "$fail_repo" && TRACE="$tmp/fail-trace" .beads/hooks/pre-push); then
    echo 'Build gate ran after a failed Beads hook' >&2
    exit 1
fi
printf 'BEADS\n' | cmp - "$tmp/fail-trace"

legacy_repo="$tmp/legacy-repo"
mkdir -p "$legacy_repo/scripts" "$legacy_repo/.beads/hooks"
git init -q "$legacy_repo"
cp "$repo/scripts/"* "$legacy_repo/scripts/"
cat > "$legacy_repo/.beads/hooks/pre-push" <<'EOF'
#!/bin/sh
# --- BEGIN BEADS INTEGRATION ---
printf 'BEADS\n' >> "$TRACE"
cat >> "$TRACE"
# --- END BEADS INTEGRATION ---

# --- BEGIN NOSTRC BUILD VERIFICATION ---
nostrc_root="$(git rev-parse --show-toplevel)" || exit 1
"$nostrc_root/scripts/pre-push" "$@"
# --- END NOSTRC BUILD VERIFICATION ---
EOF
chmod +x "$legacy_repo/.beads/hooks/pre-push"
git -C "$legacy_repo" config core.hooksPath .beads/hooks
"$legacy_repo/scripts/install-hooks.sh" >/dev/null
case "$("$legacy_repo/scripts/install-hooks.sh")" in
    *'already installed'*) ;;
    *) echo 'Legacy hook upgrade was not idempotent' >&2; exit 1 ;;
esac
[ "$(grep -Fc 'BEGIN NOSTRC PUSH INPUT REPLAY' "$legacy_repo/.beads/hooks/pre-push")" -eq 1 ]
printf 'legacy-ref\n' | (cd "$legacy_repo" && TRACE="$tmp/legacy-trace" .beads/hooks/pre-push)
printf 'BEADS\nlegacy-ref\nBUILD\nlegacy-ref\n' | cmp - "$tmp/legacy-trace"

git -C "$repo" config core.hooksPath .other/hooks
mkdir -p "$repo/.other/hooks"
printf '#!/bin/sh\necho CUSTOM\n' > "$repo/.other/hooks/pre-push"
if "$repo/scripts/install-hooks.sh" >/dev/null 2>&1; then
    echo 'Installer overwrote an unknown hook' >&2
    exit 1
fi
grep -Fq 'CUSTOM' "$repo/.other/hooks/pre-push"

git -C "$repo" config --unset core.hooksPath
"$repo/scripts/install-hooks.sh"
cmp "$repo/scripts/pre-push" "$repo/.git/hooks/pre-push"
# Upgrade only exact former standalone repository hooks, never customized copies.
standalone="$tmp/standalone"
mkdir -p "$standalone/scripts" "$standalone/.git/hooks"
git init -q "$standalone"
cp "$SCRIPT_DIR/install-hooks.sh" "$SCRIPT_DIR/pre-push" "$standalone/scripts/"
cp "$SCRIPT_DIR/fixtures/pre-push-selective-old.txt" "$standalone/.git/hooks/pre-push"
chmod +x "$standalone/.git/hooks/pre-push"
[ "$(git -C "$standalone" hash-object .git/hooks/pre-push)" = 2c541d0c442c52980e4ab3513d12b39267ba4c2c ]
cat > "$standalone/scripts/full-gate.sh" <<'FORBIDDEN'
#!/bin/sh
echo 'installer retained a build-capable hook' >&2
exit 93
FORBIDDEN
cp "$standalone/scripts/full-gate.sh" "$standalone/scripts/test-gate.sh"
chmod +x "$standalone/scripts/"*.sh
"$standalone/scripts/install-hooks.sh"
cmp "$standalone/scripts/pre-push" "$standalone/.git/hooks/pre-push"
(cd "$standalone" && NOSTRC_FULL_GATE=1 NOSTRC_SELECTIVE_GATE=1 .git/hooks/pre-push \
    <<< '(delete) 0000000000000000000000000000000000000000 refs/heads/gone 1111111111111111111111111111111111111111')
cp "$SCRIPT_DIR/fixtures/pre-push-selective-old.txt" "$standalone/.git/hooks/pre-push"
printf '\n# developer customization\n' >> "$standalone/.git/hooks/pre-push"
if "$standalone/scripts/install-hooks.sh" >/dev/null 2>&1; then
    echo 'Installer overwrote a customized former hook' >&2; exit 1
fi
grep -q 'developer customization' "$standalone/.git/hooks/pre-push"
echo 'install-hooks tests passed'
