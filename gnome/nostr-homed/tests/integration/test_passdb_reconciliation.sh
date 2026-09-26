#!/usr/bin/env bash
#
# test_passdb_reconciliation.sh — plan §4.2 B3 (2026-09-25).
#
# Startup reconciliation refuses readiness on drift.  Hand-corrupted
# passdb fixture: our mock `pdbedit -L` returns a synthetic account
# list including a user that is NOT in the SQLite issuance journal.
# The authority MUST return NH_SMB_RECONCILE_REQUIRED at open time
# rather than auto-repairing (plan §4.2 B3).
#
# Additionally exercises the clean case: with the passdb enumeration
# matching the journal exactly, open() succeeds.  This anchors the
# test to REAL behaviour — a symmetric no-drift path — so a bug that
# unconditionally returns RECONCILE_REQUIRED doesn't silently pass.
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

# ─── Case A: drift (passdb has a user with no active journal row) ──

JOURNAL_A="$TEST_DIR/smb.a.db"
# Fresh journal has ZERO active credentials.  The mock's user list
# has one — that's the exact drift the plan §4.2 B3 test spec calls
# out (unknown account in passdb).
export PDBEDIT_MOCK_USERS='rogue_alice:1042:Alice'

rc=0
out="$("$probe" "$JOURNAL_A" "$CONF")" || rc=$?
if [ "$rc" -ne 42 ]; then
  echo "FAIL: case A expected exit 42 (RECONCILE_REQUIRED), got $rc, stdout=$out"
  exit 1
fi
if [ "$out" != "reconcile-required" ]; then
  echo "FAIL: case A expected rc name 'reconcile-required', got '$out'"
  exit 1
fi
echo "PASS: case A — drift refused readiness (RECONCILE_REQUIRED)"

# ─── Case B: clean (both sides empty → open succeeds) ──────────────
JOURNAL_B="$TEST_DIR/smb.b.db"
export PDBEDIT_MOCK_USERS=''  # empty passdb, empty journal → match

rc=0
out="$("$probe" "$JOURNAL_B" "$CONF")" || rc=$?
if [ "$rc" -ne 0 ]; then
  echo "FAIL: case B expected exit 0 (OK), got $rc, stdout=$out"
  exit 1
fi
if [ "$out" != "ok" ]; then
  echo "FAIL: case B expected rc name 'ok', got '$out'"
  exit 1
fi
echo "PASS: case B — no drift, open() accepted readiness"

echo "PASS: startup reconciliation refuses drift, accepts clean state"
