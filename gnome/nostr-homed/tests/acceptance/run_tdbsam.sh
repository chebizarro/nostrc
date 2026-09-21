#!/usr/bin/env bash
#
# run_tdbsam.sh — real tdbsam passdb adapter acceptance (D bucket).
#
# Drives the built tdbsam_driver against a live Samba install to prove
# that nh_smb_passdb_tdbsam_ops.set_password / .disable / .remove do
# the right thing to smbpasswd + pdbedit end-to-end.  Also proves
# password-authentication against the running smbd (smbclient -L).
#
# DESTRUCTIVE: installs samba packages if missing, overwrites
# /etc/samba/smb.conf (backup restored on exit), creates and deletes a
# throwaway POSIX user, restarts smbd/nmbd.  Run only on a disposable
# lab VM.  Tracks beads nostrc-rb0e.7.
#
# Usage (on the Linux target, as a sudo-capable user):
#   NOSTR_ACCEPTANCE_LAB_OPT_IN=I_UNDERSTAND_THIS_RUNS_AGAINST_A_DISPOSABLE_LAB \
#     DRIVER=/path/to/build/tdbsam_driver \
#     gnome/nostr-homed/tests/acceptance/run_tdbsam.sh
#
# Env overrides:
#   DRIVER              path to the built tdbsam_driver binary (required)
#   SUDO                sudo (default) — set to '' if already root
#   NH_TEST_USER        POSIX username (default: nh_smbtest)
#   NH_SMB_SMBPASSWD_PATH  smbpasswd override (propagated to the driver)
#   NH_SMB_PDBEDIT_PATH    pdbedit override (propagated to the driver)
#
set -euo pipefail

OPT_IN=I_UNDERSTAND_THIS_RUNS_AGAINST_A_DISPOSABLE_LAB
[ "${NOSTR_ACCEPTANCE_LAB_OPT_IN:-}" = "$OPT_IN" ] \
  || { echo "refusing: set NOSTR_ACCEPTANCE_LAB_OPT_IN=$OPT_IN (disposable lab only)" >&2; exit 69; }

DRIVER="${DRIVER:-}"
[ -n "$DRIVER" ] && [ -x "$DRIVER" ] \
  || { echo "set DRIVER=/path/to/tdbsam_driver (built with -DNOSTR_HOMED_ENABLE_SMB=ON)" >&2; exit 64; }

SUDO="${SUDO-sudo}"
NH_TEST_USER="${NH_TEST_USER:-nh_smbtest}"
SMB_CONF=/etc/samba/smb.conf
SMB_CONF_BAK=/etc/samba/smb.conf.nhbak
fails=0
step() { echo "== $* =="; }
pass() { echo "  PASS $*"; }
fail() { echo "  FAIL $*"; fails=$((fails+1)); }
check_rc() { # desc, expected_rc, actual_rc
  if [ "$2" = "$3" ]; then pass "$1 (rc=$3)"; else fail "$1 (expected rc=$2, got rc=$3)"; fi
}

# --- teardown --------------------------------------------------------
cleanup() {
  set +e
  # Remove the SMB account (idempotent — may already be gone).
  $SUDO "${NH_SMB_PDBEDIT_PATH:-/usr/bin/pdbedit}" -x -u "$NH_TEST_USER" >/dev/null 2>&1
  # Delete the POSIX user (idempotent).
  $SUDO userdel -r "$NH_TEST_USER" 2>/dev/null || $SUDO userdel "$NH_TEST_USER" 2>/dev/null
  # Restore smb.conf.
  if [ -f "$SMB_CONF_BAK" ]; then
    $SUDO mv -f "$SMB_CONF_BAK" "$SMB_CONF"
  fi
  # Best-effort restart so we leave Samba consistent with the restored conf.
  $SUDO systemctl restart smbd 2>/dev/null || $SUDO service smbd restart 2>/dev/null || true
  $SUDO systemctl restart nmbd 2>/dev/null || $SUDO service nmbd restart 2>/dev/null || true
}
trap cleanup EXIT

# --- prereqs ---------------------------------------------------------
step "ensure samba + smbclient present"
if ! command -v smbpasswd >/dev/null || ! command -v pdbedit >/dev/null \
   || ! command -v smbclient >/dev/null; then
  if command -v apt-get >/dev/null; then
    $SUDO DEBIAN_FRONTEND=noninteractive apt-get update
    $SUDO DEBIAN_FRONTEND=noninteractive apt-get install -y \
      samba samba-common-bin smbclient
  else
    echo "smbpasswd/pdbedit/smbclient missing and apt-get unavailable" >&2
    exit 64
  fi
fi
command -v smbpasswd >/dev/null || { echo "smbpasswd still missing" >&2; exit 64; }
command -v pdbedit  >/dev/null || { echo "pdbedit still missing"  >&2; exit 64; }
command -v smbclient >/dev/null || { echo "smbclient still missing" >&2; exit 64; }

# --- smb.conf --------------------------------------------------------
step "back up smb.conf and install tdbsam-only config"
$SUDO mkdir -p /etc/samba
if [ -f "$SMB_CONF" ] && [ ! -f "$SMB_CONF_BAK" ]; then
  $SUDO cp -a "$SMB_CONF" "$SMB_CONF_BAK"
fi
$SUDO tee "$SMB_CONF" >/dev/null <<'CONF'
[global]
   workgroup = WORKGROUP
   server string = nh-tdbsam-acceptance
   security = user
   passdb backend = tdbsam
   map to guest = never
   log level = 1
   disable netbios = yes
   smb ports = 445

[nh_share]
   comment = nh acceptance share
   path = /tmp/nh_smb_share
   read only = yes
   guest ok = no
CONF
$SUDO mkdir -p /tmp/nh_smb_share
$SUDO chmod 0755 /tmp/nh_smb_share

step "restart smbd"
$SUDO systemctl restart smbd 2>/dev/null || $SUDO service smbd restart

# --- throwaway POSIX user --------------------------------------------
step "create throwaway POSIX user $NH_TEST_USER"
if ! id -u "$NH_TEST_USER" >/dev/null 2>&1; then
  $SUDO useradd -M -s /usr/sbin/nologin "$NH_TEST_USER"
fi

# --- generate a random password to drive through the adapter --------
PW="$(head -c 24 /dev/urandom | base64 | tr -d '=+/\n' | cut -c1-20)"
[ -n "$PW" ] || { echo "failed to mint random password" >&2; exit 71; }

# --- drive: set_password ---------------------------------------------
step "drive set_password via tdbsam_driver"
set +e
printf '%s\n' "$PW" | $SUDO -E "$DRIVER" set "$NH_TEST_USER"
rc=$?
set -e
check_rc "driver set exits 0" 0 "$rc"

# --- assert account is present ---------------------------------------
step "assert pdbedit -L lists $NH_TEST_USER"
if $SUDO pdbedit -L 2>/dev/null | cut -d: -f1 | grep -qx "$NH_TEST_USER"; then
  pass "pdbedit lists $NH_TEST_USER"
else
  fail "pdbedit did NOT list $NH_TEST_USER"
fi

# --- assert smbclient authenticates ----------------------------------
step "assert smbclient authenticates as $NH_TEST_USER"
set +e
out="$(smbclient -L //localhost -U "$NH_TEST_USER%$PW" -p 445 2>&1)"
rc=$?
set -e
if [ "$rc" = "0" ]; then
  pass "smbclient -L authenticates (rc=0)"
else
  fail "smbclient -L failed to authenticate (rc=$rc)"
  printf '    output:\n%s\n' "$out" | sed 's/^/      /'
fi

# --- drive: disable + assert auth now fails --------------------------
step "drive disable via tdbsam_driver"
set +e
$SUDO -E "$DRIVER" disable "$NH_TEST_USER"
rc=$?
set -e
check_rc "driver disable exits 0" 0 "$rc"

step "assert smbclient no longer authenticates"
set +e
smbclient -L //localhost -U "$NH_TEST_USER%$PW" -p 445 >/dev/null 2>&1
rc=$?
set -e
if [ "$rc" != "0" ]; then
  pass "smbclient rejected disabled account (rc=$rc)"
else
  fail "smbclient still authenticated after disable"
fi

# --- drive: remove + assert account is gone --------------------------
step "drive remove via tdbsam_driver"
set +e
$SUDO -E "$DRIVER" remove "$NH_TEST_USER"
rc=$?
set -e
check_rc "driver remove exits 0" 0 "$rc"

step "assert pdbedit -L no longer lists $NH_TEST_USER"
if $SUDO pdbedit -L 2>/dev/null | cut -d: -f1 | grep -qx "$NH_TEST_USER"; then
  fail "pdbedit still lists $NH_TEST_USER after remove"
else
  pass "pdbedit no longer lists $NH_TEST_USER"
fi

step "assert smbclient still cannot authenticate (removed)"
set +e
smbclient -L //localhost -U "$NH_TEST_USER%$PW" -p 445 >/dev/null 2>&1
rc=$?
set -e
if [ "$rc" != "0" ]; then
  pass "smbclient rejected removed account (rc=$rc)"
else
  fail "smbclient still authenticated after remove"
fi

# Wipe the local plaintext copy of the password.
PW=""
unset PW

echo
if [ "$fails" -eq 0 ]; then
  echo "RESULT: PASS (real tdbsam adapter acceptance on $(uname -m))"
  exit 0
else
  echo "RESULT: FAIL ($fails checks)"
  exit 1
fi
