#!/usr/bin/env bash
#
# test_passdb_reconciliation.sh — plan §4.2 B3 (2026-09-25).
#
# An empty journal accepts the first passdb snapshot (empty or manually
# seeded). A durable bootstrap marker then makes all later opens strict:
# passdb addition/deletion and an existing issuance-journal mismatch
# must return NH_SMB_RECONCILE_REQUIRED.
#
set -euo pipefail

script_dir="$(cd "$(dirname "$0")" && pwd)"

probe="${SMB_RECONCILE_PROBE:-}"
if [ -z "$probe" ]; then
  for cand in \
      "$script_dir/../../../build/gnome/nostr-homed/smb_reconcile_probe" \
      "$script_dir/../../../build/smb_reconcile_probe" \
      "$script_dir/../../build/smb_reconcile_probe"; do
    if [ -x "$cand" ]; then probe="$cand"; break; fi
  done
fi
if [ -z "$probe" ] || [ ! -x "$probe" ]; then
  echo "SKIP: smb_reconcile_probe not built (looked for SMB_RECONCILE_PROBE=$probe)" >&2
  exit 77
fi

TEST_DIR="$(mktemp -d)"
trap 'rm -rf "$TEST_DIR"' EXIT

STUB_BIN="$TEST_DIR/bin"
mkdir -p "$STUB_BIN"

# Mock pdbedit whose stdout is controlled by $PDBEDIT_MOCK_USERS
# (colon-terminated username per line, matching pdbedit -L output
# shape).  Any argv works; the mock ignores everything and prints the
# fixture.
cat > "$STUB_BIN/pdbedit" <<'STUB'
#!/usr/bin/env bash
if [ -n "${PDBEDIT_MOCK_USERS:-}" ]; then
  printf '%s\n' "${PDBEDIT_MOCK_USERS}"
fi
exit 0
STUB
chmod +x "$STUB_BIN/pdbedit"

# smbpasswd mock — unused in the reconciliation path but the probe's
# sweep-on-open touches it.  Accept any argv; produce no output.
cat > "$STUB_BIN/smbpasswd" <<'STUB'
#!/usr/bin/env bash
cat >/dev/null 2>&1 || true
exit 0
STUB
chmod +x "$STUB_BIN/smbpasswd"

CONF="$TEST_DIR/smb.conf.standalone"
cat > "$CONF" <<'IGNORED'
[global]
workgroup = NOSTR
IGNORED

export NH_SMB_SMBPASSWD_PATH="$STUB_BIN/smbpasswd"
export NH_SMB_PDBEDIT_PATH="$STUB_BIN/pdbedit"

# ─── Case A: truly empty first boot, then drift ───────────────────
JOURNAL_A="$TEST_DIR/smb.a.db"
export PDBEDIT_MOCK_USERS=''
out="$("$probe" "$JOURNAL_A" "$CONF")"
[ "$out" = ok ] || { echo "FAIL: empty first boot: $out"; exit 1; }
[ "$(sqlite3 "$JOURNAL_A" "SELECT value FROM metadata WHERE key='passdb_bootstrapped'")" = 1 ] || {
  echo 'FAIL: first boot did not persist bootstrap marker'; exit 1;
}
export PDBEDIT_MOCK_USERS='rogue_alice:1042:Alice'
rc=0; out="$("$probe" "$JOURNAL_A" "$CONF")" || rc=$?
[ "$rc" -eq 42 ] && [ "$out" = reconcile-required ] || {
  echo "FAIL: post-boot added passdb user accepted: rc=$rc out=$out"; exit 1;
}
echo 'PASS: empty first boot accepted; later passdb addition refused'

# ─── Case B: operator-seeded passdb adopted once, then checked ──
JOURNAL_B="$TEST_DIR/smb.b.db"
export PDBEDIT_MOCK_USERS='nostr:994:Nostr acceptance test user'
out="$("$probe" "$JOURNAL_B" "$CONF")"
[ "$out" = ok ] || { echo "FAIL: pre-seeded first boot: $out"; exit 1; }
[ "$(sqlite3 "$JOURNAL_B" 'SELECT username FROM adopted_passdb')" = nostr ] || {
  echo 'FAIL: pre-seeded account not recorded in journal'; exit 1;
}
out="$("$probe" "$JOURNAL_B" "$CONF")"
[ "$out" = ok ] || { echo "FAIL: unchanged adopted account: $out"; exit 1; }
export PDBEDIT_MOCK_USERS=''
rc=0; out="$("$probe" "$JOURNAL_B" "$CONF")" || rc=$?
[ "$rc" -eq 42 ] && [ "$out" = reconcile-required ] || {
  echo "FAIL: post-boot deleted passdb user accepted: rc=$rc out=$out"; exit 1;
}
echo 'PASS: pre-seeded account adopted; later deletion refused'

# ─── Case C: old journal with issuance history must not adopt ───
JOURNAL_C="$TEST_DIR/smb.c.db"
export PDBEDIT_MOCK_USERS=''
out="$("$probe" "$JOURNAL_C" "$CONF")"
[ "$out" = ok ] || { echo "FAIL: seed old journal schema: $out"; exit 1; }
sqlite3 "$JOURNAL_C" "DELETE FROM metadata WHERE key='passdb_bootstrapped';
INSERT INTO credentials(credential_id,username,uid,pubkey_hex,binding,issued_at_ms,expires_at_ms)
VALUES('00000000-0000-4000-8000-000000000001','issued_alice',1042,
       'aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa',
       '',1,9999999999999);"
export PDBEDIT_MOCK_USERS='rogue_alice:1042:Alice'
rc=0; out="$("$probe" "$JOURNAL_C" "$CONF")" || rc=$?
[ "$rc" -eq 42 ] && [ "$out" = reconcile-required ] || {
  echo "FAIL: existing journal drift accepted: rc=$rc out=$out"; exit 1;
}
echo 'PASS: existing issuance journal still refuses drift'

# ─── Case D: revoking an adopted account must not strand its baseline ──
JOURNAL_D="$TEST_DIR/smb.d.db"
export PDBEDIT_MOCK_USERS='nostr:994:Nostr acceptance test user'
out="$("$probe" "$JOURNAL_D" "$CONF")"
[ "$out" = ok ] || { echo "FAIL: seed adopted account: $out"; exit 1; }
out="$("$probe" "$JOURNAL_D" "$CONF" revoke nostr)"
[ "$out" = ok ] || { echo "FAIL: revoke adopted account: $out"; exit 1; }
[ -z "$(sqlite3 "$JOURNAL_D" 'SELECT username FROM adopted_passdb')" ] || {
  echo 'FAIL: revoke left adopted baseline behind'; exit 1;
}
export PDBEDIT_MOCK_USERS=''
out="$("$probe" "$JOURNAL_D" "$CONF")"
[ "$out" = ok ] || { echo "FAIL: restart after adopted revoke: $out"; exit 1; }
echo 'PASS: adopted revoke removes baseline; restart accepted'
