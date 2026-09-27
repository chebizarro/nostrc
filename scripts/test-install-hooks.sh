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
EOF
chmod +x "$repo/scripts/pre-push"
cat > "$repo/.beads/hooks/pre-push" <<'EOF'
#!/bin/sh
# --- BEGIN BEADS INTEGRATION ---
printf 'BEADS\n' >> "$TRACE"
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
(cd "$repo" && TRACE="$tmp/trace" .beads/hooks/pre-push)
printf 'BEADS\nBUILD\n' | cmp - "$tmp/trace"

fail_repo="$tmp/fail-repo"
mkdir -p "$fail_repo/scripts" "$fail_repo/.beads/hooks"
git init -q "$fail_repo"
cp "$repo/scripts/"* "$fail_repo/scripts/"
cat > "$fail_repo/.beads/hooks/pre-push" <<'EOF'
#!/bin/sh
# --- BEGIN BEADS INTEGRATION ---
printf 'BEADS\n' >> "$TRACE"
exit 1
# --- END BEADS INTEGRATION ---
EOF
chmod +x "$fail_repo/.beads/hooks/pre-push"
git -C "$fail_repo" config core.hooksPath .beads/hooks
"$fail_repo/scripts/install-hooks.sh"
if (cd "$fail_repo" && TRACE="$tmp/fail-trace" .beads/hooks/pre-push); then
    echo 'Build gate ran after a failed Beads hook' >&2
    exit 1
fi
printf 'BEADS\n' | cmp - "$tmp/fail-trace"

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
echo 'install-hooks tests passed'
