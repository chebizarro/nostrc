#!/usr/bin/env bash
#
# test_passdb_explicit_config.sh — plan §4.2 B3 acceptance (2026-09-25).
#
# Drops mock `smbpasswd` and `pdbedit` binaries onto $PATH that capture
# their argv into log files, then drives the passdb_tdbsam adapter
# through `tdbsam_driver` for the three write ops (set/disable/remove)
# and asserts:
#
#   * `smbpasswd` invocations carry `-c <smb.conf>` (config selector,
#     smbpasswd(8) flag)
#   * `pdbedit`   invocations carry `-s <smb.conf>` (config selector,
#     pdbedit(8) flag)
#   * `smbpasswd -s` (silent-stdin mode) is preserved on the add path
#     alongside `-c`; the two flags mean different things and MUST
#     both be on the argv.  This is the exact confusion plan
#     Finding 2 flagged.
#
# Runs against the built adapter, not against a real Samba install.
#
set -euo pipefail

script_dir="$(cd "$(dirname "$0")" && pwd)"

# Prefer the CMake-provided binary; fall back to a hand-run path so a
# developer can `bash tests/integration/test_passdb_explicit_config.sh`
# after `make tdbsam_driver`.
driver="${TDBSAM_DRIVER:-}"
if [ -z "$driver" ]; then
  for cand in \
      "$script_dir/../../../build/gnome/nostr-homed/tdbsam_driver" \
      "$script_dir/../../../build/tdbsam_driver" \
      "$script_dir/../../build/tdbsam_driver"; do
    if [ -x "$cand" ]; then driver="$cand"; break; fi
  done
fi
if [ -z "$driver" ] || [ ! -x "$driver" ]; then
  echo "SKIP: tdbsam_driver not built (looked for TDBSAM_DRIVER=$driver)" >&2
  exit 77
fi

TEST_DIR="$(mktemp -d)"
trap 'rm -rf "$TEST_DIR"' EXIT

STUB_BIN="$TEST_DIR/bin"
mkdir -p "$STUB_BIN"

SMBPASSWD_LOG="$TEST_DIR/smbpasswd.argv"
PDBEDIT_LOG="$TEST_DIR/pdbedit.argv"

cat > "$STUB_BIN/smbpasswd" <<'STUB'
#!/usr/bin/env bash
# One line per invocation, tab-separated argv.  We consume stdin so
# the parent's write() doesn't SIGPIPE (the add path pipes the
# password in).
printf '%s\n' "$*" >> "SMBPASSWD_LOG"
cat >/dev/null 2>&1 || true
exit 0
STUB
sed -i.bak "s#SMBPASSWD_LOG#$SMBPASSWD_LOG#g" "$STUB_BIN/smbpasswd"
rm -f "$STUB_BIN/smbpasswd.bak"
chmod +x "$STUB_BIN/smbpasswd"

cat > "$STUB_BIN/pdbedit" <<'STUB'
#!/usr/bin/env bash
printf '%s\n' "$*" >> "PDBEDIT_LOG"
exit 0
STUB
sed -i.bak "s#PDBEDIT_LOG#$PDBEDIT_LOG#g" "$STUB_BIN/pdbedit"
rm -f "$STUB_BIN/pdbedit.bak"
chmod +x "$STUB_BIN/pdbedit"

CONF="$TEST_DIR/smb.conf.standalone"
cat > "$CONF" <<'IGNORED'
# fixture — the stubs never read this file, but the adapter passes
# it verbatim on argv and we grep for it below.
[global]
workgroup = NOSTR
IGNORED

export NH_SMB_SMBPASSWD_PATH="$STUB_BIN/smbpasswd"
export NH_SMB_PDBEDIT_PATH="$STUB_BIN/pdbedit"
export NH_SMB_CONF="$CONF"

echo "-- set_password --"
printf 'hunter22hunter22' | "$driver" set nostr_alice
echo "-- disable --"
"$driver" disable nostr_alice
echo "-- remove --"
"$driver" remove nostr_alice

fail=0
assert_contains() {
  local label="$1" file="$2" needle="$3"
  if ! grep -qF -- "$needle" "$file"; then
    echo "FAIL: $label — expected '$needle' in $file"
    echo "  ---- $file ----"
    sed 's/^/    /' "$file"
    fail=1
  fi
}
assert_not_contains() {
  local label="$1" file="$2" needle="$3"
  if grep -qF -- "$needle" "$file"; then
    echo "FAIL: $label — unexpected '$needle' in $file"
    fail=1
  fi
}

# smbpasswd argv on ALL invocations MUST carry `-c <CONF>`.
if [ ! -s "$SMBPASSWD_LOG" ]; then
  echo "FAIL: smbpasswd argv log empty — the mock was never invoked"
  fail=1
fi
while IFS= read -r line; do
  case " $line " in
    *" -c $CONF "*) ;;
    *) echo "FAIL: smbpasswd invocation missing '-c $CONF': $line"; fail=1 ;;
  esac
done < "$SMBPASSWD_LOG"

# smbpasswd add-path still carries `-a` and `-s` (silent-stdin — a
# DIFFERENT flag from pdbedit's `-s` config selector; plan Finding 2).
assert_contains "smbpasswd add -a"  "$SMBPASSWD_LOG" "-a"
assert_contains "smbpasswd add -s"  "$SMBPASSWD_LOG" "-s"
# Sanity: smbpasswd disable path (has -d) shows up.
assert_contains "smbpasswd disable -d" "$SMBPASSWD_LOG" "-d"

# pdbedit argv on ALL invocations MUST carry `-s <CONF>` and `-x -u`.
if [ ! -s "$PDBEDIT_LOG" ]; then
  echo "FAIL: pdbedit argv log empty — the mock was never invoked"
  fail=1
fi
while IFS= read -r line; do
  case " $line " in
    *" -s $CONF "*) ;;
    *) echo "FAIL: pdbedit invocation missing '-s $CONF': $line"; fail=1 ;;
  esac
done < "$PDBEDIT_LOG"
assert_contains "pdbedit remove -x -u" "$PDBEDIT_LOG" "-x -u nostr_alice"
# pdbedit MUST NOT be called with `-c` (that's smbpasswd's flag; a
# regression would surface as a silent argv swap).
assert_not_contains "pdbedit no -c" "$PDBEDIT_LOG" "-c "

if [ "$fail" -ne 0 ]; then
  echo "---- smbpasswd log ----"
  sed 's/^/  /' "$SMBPASSWD_LOG"
  echo "---- pdbedit log ----"
  sed 's/^/  /' "$PDBEDIT_LOG"
  exit 1
fi
echo "PASS: config-selector flags asserted for smbpasswd (-c) + pdbedit (-s)"
