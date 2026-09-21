#!/usr/bin/env bash
#
# run_smb_mount_vm.sh — D7 desktop SMB mount acceptance (beads nostrc-rb0e.12).
#
# Proves the last mile of the SMB flow on a Linux VM:
#     nh-seed-authority (bootstrap Nostr account)
#  -> nostr-authd       (broker on unique sockets, tdbsam passdb)
#  -> nostr-smb-acquire (mint short-lived SMB password into 0600 creds file)
#  -> nostr-smb-mount   (mount the share with the minted credential)
#  -> write + read a file inside the mount           <-- positive control
#  -> re-acquire (rotates credential, revoking the old password)
#  -> nostr-smb-mount with the STALE creds file      <-- negative control
#
# The negative control demonstrates that a revoked / rotated credential
# fails to mount, i.e. Samba honours the authority's "one active
# credential per user; rotation is revocation" contract from
# gnome/nostr-homed/src/smb/smb_credential.h.
#
# DESTRUCTIVE, disposable-VM-only:
#   - Installs samba, samba-common-bin, smbclient, cifs-utils if missing.
#   - Writes /etc/samba/smb.conf (backed up to /etc/samba/smb.conf.d7bak,
#     restored on exit).
#   - Creates a throwaway POSIX user (default: n_smbd7 — must start
#     with "n_" per nh_identity_username_is_valid) and share dir
#     /srv/nostrshare-d7 (removed on exit).
#   - Runs its OWN nostr-authd on unique sockets under /run/nostr-smb-d7,
#     authority under /var/lib/nostr-smb-d7 (removed on exit).
#   - Restarts smbd; leaves it running against restored config on exit.
#
# Isolation from concurrent D-item work on the same VM:
#   Every runtime path is namespaced with a "-d7" suffix so no other
#   agent's broker, authority, journal, socket, or share collides with
#   this run.  The only shared state touched is /etc/samba/smb.conf and
#   the smbd unit — both are backed up and restarted at exit.
#
# Usage (on the VM, as a sudo-capable user):
#   NOSTR_ACCEPTANCE_LAB_OPT_IN=I_UNDERSTAND_THIS_RUNS_AGAINST_A_DISPOSABLE_LAB \
#     BUILD=/tmp/smb-mount-build \
#     gnome/nostr-homed/tests/acceptance/run_smb_mount_vm.sh
#
# Required env (or defaults):
#   BUILD                 CMake build dir with nostr-authd, nh-seed-authority,
#                         nostr-smb-acquire, tdbsam_driver already built
#                         (default: /tmp/smb-mount-build)
#   MOUNT_HELPER          Path to nostr-smb-mount (default: <srcdir>/gnome/
#                         nostr-homed/src/smb/nostr-smb-mount.sh)
#   NH_TEST_USER          POSIX + SMB username (default: n_smbd7)
#   NH_MOUNTPOINT         Directory to mount onto (default: /mnt/nh_share_d7)
#
set -euo pipefail

# ---------------------------------------------------------------------------
# Opt-in gate — this script is destructive.
# ---------------------------------------------------------------------------
OPT_IN=I_UNDERSTAND_THIS_RUNS_AGAINST_A_DISPOSABLE_LAB
[ "${NOSTR_ACCEPTANCE_LAB_OPT_IN:-}" = "$OPT_IN" ] || {
  echo "refusing: set NOSTR_ACCEPTANCE_LAB_OPT_IN=$OPT_IN (disposable lab only)" >&2
  exit 69
}

SUDO="${SUDO-sudo}"
BUILD="${BUILD:-/tmp/smb-mount-build}"
NH_TEST_USER="${NH_TEST_USER:-n_smbd7}"
NH_MOUNTPOINT="${NH_MOUNTPOINT:-/mnt/nh_share_d7}"
PASSPHRASE="${PASSPHRASE:-correct horse battery staple}"

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
HOMED_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
MOUNT_HELPER="${MOUNT_HELPER:-$HOMED_DIR/src/smb/nostr-smb-mount.sh}"

# --- unique isolation namespace ---------------------------------------------
NS_TAG=d7
RUNTIME_DIR=/run/nostr-smb-${NS_TAG}
AUTHORITY_DIR=/var/lib/nostr-smb-${NS_TAG}
SHARE_DIR=/srv/nostrshare-${NS_TAG}
AUTH_SOCK="$RUNTIME_DIR/auth.sock"
USER_SOCK="$RUNTIME_DIR/user.sock"
SMB_JOURNAL="$AUTHORITY_DIR/smb.db"
SMB_CONF=/etc/samba/smb.conf
SMB_CONF_BAK=/etc/samba/smb.conf.${NS_TAG}bak
SHARE_NAME=nh_share_${NS_TAG}
CREDS_DIR="/tmp/nostr-smb-mount-${NS_TAG}"
CREDS_FILE="$CREDS_DIR/credentials"
CREDS_STALE="$CREDS_DIR/credentials.stale"
AUTHD_LOG=/tmp/nostr-authd-${NS_TAG}.log
AUTHD_PIDFILE=/tmp/nostr-authd-${NS_TAG}.pid

fails=0
step() { echo; echo "== $* =="; }
pass() { echo "  PASS $*"; }
fail() { echo "  FAIL $*"; fails=$((fails+1)); }

# --- teardown ---------------------------------------------------------------
cleanup() {
  set +e
  echo
  echo "== cleanup =="
  # Unmount share (twice, in case a stale bind lingered).
  if command -v mountpoint >/dev/null 2>&1 && mountpoint -q "$NH_MOUNTPOINT" 2>/dev/null; then
    $SUDO umount "$NH_MOUNTPOINT" 2>/dev/null
    sleep 0.2
    $SUDO umount -f "$NH_MOUNTPOINT" 2>/dev/null
  fi
  # Stop OUR broker only — never touch other agents' authd (unique socket).
  if [ -f "$AUTHD_PIDFILE" ]; then
    local pid; pid="$(cat "$AUTHD_PIDFILE" 2>/dev/null || true)"
    [ -n "$pid" ] && $SUDO kill "$pid" 2>/dev/null
    sleep 0.3
    [ -n "$pid" ] && $SUDO kill -9 "$pid" 2>/dev/null
  fi
  # Belt-and-braces: any authd bound to OUR socket path only.
  $SUDO pkill -f "nostr-authd .* $USER_SOCK" 2>/dev/null
  $SUDO rm -f "$AUTHD_PIDFILE"
  # Remove OUR runtime + authority (unique paths).
  $SUDO rm -rf "$RUNTIME_DIR" "$AUTHORITY_DIR" "$SHARE_DIR"
  $SUDO rm -rf "$CREDS_DIR"
  # Remove the SMB passdb entry we may have created.
  $SUDO pdbedit -x -u "$NH_TEST_USER" >/dev/null 2>&1
  $SUDO userdel -r "$NH_TEST_USER" 2>/dev/null || \
    $SUDO userdel "$NH_TEST_USER" 2>/dev/null
  # Belt-and-braces: seed_authority pins home_root to /home; a prior aborted
  # run may have left /home/$NH_TEST_USER behind, and home_prepare treats a
  # pre-existing directory there as "ambiguous" and bails.  Remove it.
  $SUDO rm -rf "/home/$NH_TEST_USER"
  $SUDO find /home -maxdepth 1 -name ".nostr-*" -type d -exec rm -rf {} + 2>/dev/null
  # Restore smb.conf and bounce smbd so we leave the box consistent.
  if [ -f "$SMB_CONF_BAK" ]; then
    $SUDO mv -f "$SMB_CONF_BAK" "$SMB_CONF"
  fi
  $SUDO systemctl restart smbd 2>/dev/null || \
    $SUDO service smbd restart 2>/dev/null || true
  $SUDO rmdir "$NH_MOUNTPOINT" 2>/dev/null
}
trap cleanup EXIT

# --- prereqs ---------------------------------------------------------------
step "check tooling"
have() { command -v "$1" >/dev/null 2>&1; }
need_pkgs=""
for p in smbpasswd pdbedit smbclient mount.cifs; do
  have "$p" || need_pkgs="$need_pkgs $p"
done
if [ -n "$need_pkgs" ]; then
  echo "  installing:$need_pkgs (via apt-get)"
  $SUDO DEBIAN_FRONTEND=noninteractive apt-get update -qq
  $SUDO DEBIAN_FRONTEND=noninteractive apt-get install -y -qq \
    samba samba-common-bin smbclient cifs-utils
fi
for p in smbpasswd pdbedit smbclient mount.cifs; do
  have "$p" || { echo "  missing after install: $p" >&2; exit 64; }
done

# Locate built binaries.
AUTHD="$BUILD/gnome/nostr-homed/nostr-authd"
SEED="$BUILD/gnome/nostr-homed/nh-seed-authority"
ACQUIRE="$BUILD/gnome/nostr-homed/nostr-smb-acquire"
for p in "$AUTHD" "$SEED" "$ACQUIRE"; do
  [ -x "$p" ] || { echo "  missing: $p — did you build with -DNOSTR_HOMED_ENABLE_AUTH_INSTALL=ON -DNOSTR_HOMED_ENABLE_SMB=ON?" >&2; exit 65; }
done
[ -x "$MOUNT_HELPER" ] || { echo "  missing: $MOUNT_HELPER" >&2; exit 65; }
echo "  authd=$AUTHD"
echo "  acquire=$ACQUIRE"
echo "  helper=$MOUNT_HELPER"

# --- provision the share + smb.conf ----------------------------------------
step "provision Samba share [$SHARE_NAME] at $SHARE_DIR"
if [ -f "$SMB_CONF" ] && [ ! -f "$SMB_CONF_BAK" ]; then
  $SUDO cp -a "$SMB_CONF" "$SMB_CONF_BAK"
fi
$SUDO mkdir -p "$SHARE_DIR"
# Own the share dir as the throwaway POSIX user so writes land as them.
$SUDO tee "$SMB_CONF" >/dev/null <<CONF
[global]
   workgroup = WORKGROUP
   server string = nh-d7-mount-acceptance
   security = user
   passdb backend = tdbsam
   map to guest = never
   log level = 1
   disable netbios = yes
   smb ports = 445

[$SHARE_NAME]
   comment = D7 acceptance share
   path = $SHARE_DIR
   read only = no
   guest ok = no
   force user = $NH_TEST_USER
   create mask = 0644
   directory mask = 0755
CONF

# --- throwaway POSIX user --------------------------------------------------
# The Nostr-homed authority allocates uids from the default range
# NH_IDENTITY_DEFAULT_UID_MIN=200000.  The broker's SO_PEERCRED lookup on
# user.sock matches the caller's real uid against that authority, so the
# POSIX login must be pinned to uid=200000 (with a matching gid).  -o
# allows the high number on Debian/Ubuntu (default UID_MAX=60000).
NH_TARGET_UID="${NH_TARGET_UID:-200000}"
NH_TARGET_GID="${NH_TARGET_GID:-200000}"
step "create throwaway POSIX user $NH_TEST_USER (uid=$NH_TARGET_UID)"
if ! getent group "$NH_TARGET_GID" >/dev/null 2>&1; then
  $SUDO groupadd -o -g "$NH_TARGET_GID" "$NH_TEST_USER"
fi
if ! id -u "$NH_TEST_USER" >/dev/null 2>&1; then
  $SUDO useradd -o -u "$NH_TARGET_UID" -g "$NH_TARGET_GID" \
    -M -s /usr/sbin/nologin "$NH_TEST_USER"
fi
$SUDO chown "$NH_TEST_USER":"$NH_TEST_USER" "$SHARE_DIR"
$SUDO chmod 0770 "$SHARE_DIR"

step "restart smbd against fresh smb.conf"
$SUDO systemctl restart smbd 2>/dev/null || $SUDO service smbd restart

# --- runtime + authority on unique paths -----------------------------------
step "provision runtime dir $RUNTIME_DIR (0711) and authority $AUTHORITY_DIR (0700)"
$SUDO install -d -m 0711 "$RUNTIME_DIR"
$SUDO install -d -m 0700 "$AUTHORITY_DIR"
$SUDO chown root:root "$RUNTIME_DIR" "$AUTHORITY_DIR"

# --- seed the authority with n_alice-style test account --------------------
# Purge any stray home dir from a previous aborted run BEFORE seeding —
# nh_identity_home_prepare's HOME_CREATE path treats a pre-existing
# /home/$NH_TEST_USER as filesystem-ambiguous and refuses.
$SUDO rm -rf "/home/$NH_TEST_USER"
$SUDO find /home -maxdepth 1 -name ".nostr-*" -type d -exec rm -rf {} + 2>/dev/null || true

step "seed authority for $NH_TEST_USER"
$SUDO "$SEED" "$AUTHORITY_DIR" "$NH_TEST_USER" "$PASSPHRASE" >/tmp/seed-${NS_TAG}.log 2>&1 \
  || { cat /tmp/seed-${NS_TAG}.log; exit 1; }
grep -q "seeded $NH_TEST_USER" /tmp/seed-${NS_TAG}.log || { cat /tmp/seed-${NS_TAG}.log; exit 1; }
head -1 /tmp/seed-${NS_TAG}.log | sed 's/^/  /'

# --- start nostr-authd on unique sockets -----------------------------------
step "start nostr-authd on $AUTH_SOCK + $USER_SOCK"
$SUDO rm -f "$AUTH_SOCK" "$USER_SOCK"
# 0666 user.sock is created by authd; auth.sock 0600 is root-only.  We run
# authd as root so it can seat the tdbsam entry via smbpasswd/pdbedit.
$SUDO sh -c "\"$AUTHD\" \"$AUTH_SOCK\" \"$AUTHORITY_DIR\" \"$USER_SOCK\" \"$SMB_JOURNAL\" >\"$AUTHD_LOG\" 2>&1 & echo \$! >\"$AUTHD_PIDFILE\""
# Wait for the sockets to appear.
for _ in $(seq 1 40); do
  if [ -S "$USER_SOCK" ] && [ -S "$AUTH_SOCK" ]; then break; fi
  sleep 0.1
done
[ -S "$USER_SOCK" ] || { echo "user.sock did not appear"; tail -40 "$AUTHD_LOG"; exit 1; }
$SUDO chmod 0666 "$USER_SOCK"
echo "  authd pid=$(cat "$AUTHD_PIDFILE")"
grep -m1 . "$AUTHD_LOG" | sed 's/^/  /'

# --- mint credential via nostr-smb-acquire ---------------------------------
step "mint credential via nostr-smb-acquire -> $CREDS_FILE"
$SUDO install -d -m 0700 -o "$NH_TEST_USER" -g "$NH_TEST_USER" "$CREDS_DIR"
# Drive acquire as the seeded uid so SO_PEERCRED on user.sock resolves to
# n_smbd7's account.  setpriv is preferred over `sudo -u` because it
# preserves NOSTR_SMB_ACQUIRE_PASSPHRASE without needing --preserve-env.
$SUDO env NOSTR_HOMED_PASSPHRASE="$PASSPHRASE" \
  setpriv --reuid="$NH_TARGET_UID" --regid="$NH_TARGET_GID" --clear-groups \
    "$ACQUIRE" --socket "$USER_SOCK" --provider local \
               --file "$CREDS_FILE" 2>/tmp/acquire-${NS_TAG}.log \
  || { echo "acquire failed:"; sed 's/^/    /' /tmp/acquire-${NS_TAG}.log; exit 1; }
sed 's/^/  /' /tmp/acquire-${NS_TAG}.log

# The creds file is 0600 owned by n_smbd7 inside a 0700 dir owned by
# n_smbd7 — traversable to root only.  Re-parent to root:root so mount.cifs
# (invoked via sudo) can both stat and read the file.  Contents (the
# short-lived password) are untouched.
$SUDO chown -R root:root "$CREDS_DIR"
$SUDO chmod 0700 "$CREDS_DIR"

$SUDO test -f "$CREDS_FILE" || { echo "creds file not written"; exit 1; }
mode="$($SUDO stat -c '%a' "$CREDS_FILE")"
[ "$mode" = "600" ] || fail "creds file mode is $mode, want 600"
$SUDO grep -q "^username=$NH_TEST_USER$" "$CREDS_FILE" \
  || fail "creds file missing username=$NH_TEST_USER"
$SUDO grep -q "^password=" "$CREDS_FILE" \
  || fail "creds file missing password="

# --- provision mountpoint --------------------------------------------------
step "ensure mountpoint $NH_MOUNTPOINT exists"
$SUDO install -d -m 0755 "$NH_MOUNTPOINT"

# ==========================================================================
# POSITIVE CONTROL: mint -> mount -> write -> read -> umount
# ==========================================================================
step "POSITIVE: mount //127.0.0.1/$SHARE_NAME with minted credential"
POS_LOG=/tmp/mount-pos-${NS_TAG}.log
if $SUDO "$MOUNT_HELPER" --creds "$CREDS_FILE" --uid "$(id -u)" --gid "$(id -g)" \
    "//127.0.0.1/$SHARE_NAME" "$NH_MOUNTPOINT" >"$POS_LOG" 2>&1; then
  pass "mount.cifs accepted minted credential"
else
  fail "mount.cifs rejected minted credential"
  sed 's/^/    /' "$POS_LOG"
fi

if mountpoint -q "$NH_MOUNTPOINT"; then
  pass "$NH_MOUNTPOINT is a mountpoint"
else
  fail "$NH_MOUNTPOINT is NOT a mountpoint"
fi

# Write + read a file across the mount.
step "POSITIVE: write + read hello-d7.txt"
PAYLOAD="hello-from-d7-$(date -u +%s)"
if $SUDO -u "$NH_TEST_USER" sh -c "printf '%s\n' '$PAYLOAD' > '$NH_MOUNTPOINT/hello-d7.txt'"; then
  pass "wrote $NH_MOUNTPOINT/hello-d7.txt"
else
  # fallback: mount options may not have given our uid write rights; try as root.
  if $SUDO sh -c "printf '%s\n' '$PAYLOAD' > '$NH_MOUNTPOINT/hello-d7.txt'"; then
    pass "wrote $NH_MOUNTPOINT/hello-d7.txt (as root via mount)"
  else
    fail "could not write via the mount"
  fi
fi

if $SUDO cat "$NH_MOUNTPOINT/hello-d7.txt" 2>/dev/null | grep -qx "$PAYLOAD"; then
  pass "readback matches payload"
else
  fail "readback did NOT match payload"
  $SUDO ls -la "$NH_MOUNTPOINT" 2>/dev/null | sed 's/^/    /'
fi

# Confirm the write landed on the server-side share dir (not shadow).
if $SUDO cat "$SHARE_DIR/hello-d7.txt" 2>/dev/null | grep -qx "$PAYLOAD"; then
  pass "file present on server-side $SHARE_DIR"
else
  fail "file NOT present on server-side $SHARE_DIR"
fi

step "POSITIVE: unmount"
if $SUDO umount "$NH_MOUNTPOINT"; then
  pass "unmounted"
else
  fail "unmount failed"
fi

# ==========================================================================
# NEGATIVE CONTROL: rotate credential (=revocation of the old one), then
# attempt to mount using the STALE credentials file — must fail.
# ==========================================================================
step "NEGATIVE: save stale creds and rotate by re-acquiring"
$SUDO cp -a "$CREDS_FILE" "$CREDS_STALE"
# Hand the creds dir back to the seed uid for the rotation write, then
# reclaim it as root so mount.cifs (via sudo) can still read it.
$SUDO chown -R "$NH_TEST_USER":"$NH_TEST_USER" "$CREDS_DIR"
$SUDO env NOSTR_HOMED_PASSPHRASE="$PASSPHRASE" \
  setpriv --reuid="$NH_TARGET_UID" --regid="$NH_TARGET_GID" --clear-groups \
    "$ACQUIRE" --socket "$USER_SOCK" --provider local \
               --file "$CREDS_FILE" 2>/tmp/acquire2-${NS_TAG}.log \
  || { echo "second acquire failed:"; sed 's/^/    /' /tmp/acquire2-${NS_TAG}.log; exit 1; }
$SUDO chown -R root:root "$CREDS_DIR"
$SUDO chmod 0700 "$CREDS_DIR"
sed 's/^/  /' /tmp/acquire2-${NS_TAG}.log

# Sanity: the two files must differ (rotation minted a new password).
if $SUDO diff -q "$CREDS_FILE" "$CREDS_STALE" >/dev/null; then
  fail "creds unchanged after rotation (test is not meaningful)"
else
  pass "credential rotated (stale != current)"
fi

step "NEGATIVE: mount with stale credentials must FAIL"
NEG_LOG=/tmp/mount-neg-${NS_TAG}.log
if $SUDO "$MOUNT_HELPER" --creds "$CREDS_STALE" --uid "$(id -u)" --gid "$(id -g)" \
    "//127.0.0.1/$SHARE_NAME" "$NH_MOUNTPOINT" >"$NEG_LOG" 2>&1; then
  fail "mount.cifs INCORRECTLY accepted a revoked credential"
  $SUDO umount "$NH_MOUNTPOINT" 2>/dev/null
else
  rc=$?
  pass "mount.cifs rejected revoked credential (rc=$rc)"
  # Look for the expected auth diagnostic in the mount helper log.
  if grep -qiE "permission denied|logon failure|nt_status_logon_failure|nt_status_access_denied|nt_status_wrong_password" "$NEG_LOG"; then
    pass "diagnostic indicates authentication failure"
  else
    echo "  (mount log:)"; sed 's/^/    /' "$NEG_LOG"
  fi
fi

# Belt-and-braces: also assert the stale creds are refused by smbclient
# directly, so we know it's the authoritative Samba rejection and not a
# cifs-utils quirk.
step "NEGATIVE: smbclient must ALSO reject stale credentials"
stale_user="$($SUDO awk -F= '/^username=/{print $2; exit}' "$CREDS_STALE" | tr -d '[:space:]')"
stale_pw="$($SUDO awk -F= '/^password=/{sub(/^password=/,""); print; exit}' "$CREDS_STALE")"
if smbclient -L //127.0.0.1 -U "$stale_user%$stale_pw" -p 445 >/dev/null 2>&1; then
  fail "smbclient still accepted stale credential"
else
  pass "smbclient rejected stale credential"
fi

# Wipe local plaintext.
stale_pw=""; unset stale_pw

echo
if [ "$fails" -eq 0 ]; then
  echo "RESULT: PASS (D7 desktop SMB mount acceptance on $(uname -m))"
  exit 0
else
  echo "RESULT: FAIL ($fails checks)"
  exit 1
fi
