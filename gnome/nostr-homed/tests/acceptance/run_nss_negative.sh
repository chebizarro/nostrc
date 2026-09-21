#!/usr/bin/env bash
#
# run_nss_negative.sh — installed libnss_nostr.so.2 FAIL-CLOSED acceptance.
#
# Companion to run_nss_getent.sh. Where that script proves the module
# resolves valid users against a trusted projection, this script proves
# the module refuses to resolve when its trust chain is broken. Assertions
# exercise trusted_file() (the every-path-component root-owned, non-world-
# writable, no-symlink walk) plus the config/reader plumbing:
#
#   - /etc/nss_nostr.conf missing            -> UNAVAIL / fall through
#   - /etc/nss_nostr.conf world-writable     -> UNAVAIL
#   - /etc/nss_nostr.conf symlink            -> UNAVAIL
#   - projection_path -> non-root-owned DB   -> UNAVAIL
#   - projection_path -> world-writable DB   -> UNAVAIL
#   - projection_path -> symlink target      -> UNAVAIL
#   - unknown user (correct 0644 config+db)  -> NOTFOUND
#   - baseline (correct 0644 config+db)      -> resolves n_active
#
# Tracks beads nostrc-nxpb.4 / nostrc-nxpb.6. DESTRUCTIVE to system NSS
# config — run only on a disposable lab VM (matches run_nss_getent.sh).
#
# Usage:
#   NOSTR_ACCEPTANCE_LAB_OPT_IN=I_UNDERSTAND_THIS_RUNS_AGAINST_A_DISPOSABLE_LAB \
#     NSS_SO=/path/to/libnss_nostr.so.2 \
#     FIXTURE=/path/to/projection_v1.sql \
#     gnome/nostr-homed/tests/acceptance/run_nss_negative.sh
#
set -euo pipefail

OPT_IN=I_UNDERSTAND_THIS_RUNS_AGAINST_A_DISPOSABLE_LAB
[ "${NOSTR_ACCEPTANCE_LAB_OPT_IN:-}" = "$OPT_IN" ] \
  || { echo "refusing: set NOSTR_ACCEPTANCE_LAB_OPT_IN=$OPT_IN (disposable lab only)" >&2; exit 69; }

here="$(cd "$(dirname "$0")" && pwd)"
NSS_SO="${NSS_SO:-}"
FIXTURE="${FIXTURE:-$here/../identity/fixtures/projection_v1.sql}"
[ -n "$NSS_SO" ] && [ -f "$NSS_SO" ] \
  || { echo "set NSS_SO to the built libnss_nostr.so.2" >&2; exit 64; }
[ -f "$FIXTURE" ] \
  || { echo "fixture not found: $FIXTURE" >&2; exit 64; }
command -v sqlite3 >/dev/null \
  || { echo "sqlite3 CLI required" >&2; exit 64; }
command -v getent >/dev/null \
  || { echo "getent required" >&2; exit 64; }

SUDO="${SUDO:-sudo}"
LIBDIR="/usr/lib/$(gcc -print-multiarch 2>/dev/null || echo x86_64-linux-gnu)"
DBDIR=/var/lib/nostr-auth
DB=$DBDIR/nss.db
DB_ALT=$DBDIR/nss.db.alt
CONF=/etc/nss_nostr.conf
CONF_ALT=/etc/nss_nostr.conf.alt
INSTALLED_SO="$LIBDIR/libnss_nostr.so.2"
fails=0
step() { echo "== $* =="; }
check() {
  # $1 desc, $2 expected, $3 actual
  if [ "$2" = "$3" ]; then echo "  PASS $1"
  else echo "  FAIL $1"; echo "    expected: $2"; echo "    actual:   $3"; fails=$((fails+1))
  fi
}
# check_notfound: getent should print no line (NOTFOUND/UNAVAIL). We
# specifically want an empty stdout — the module MUST NOT synthesize a
# resolution when the trust chain is broken.
check_empty() {
  # $1 desc, $2 actual
  if [ -z "$2" ]; then echo "  PASS $1 (empty, as expected)"
  else echo "  FAIL $1"; echo "    unexpected output: $2"; fails=$((fails+1))
  fi
}

install_good_conf() {
  echo "projection_path=$DB" | $SUDO tee "$CONF" >/dev/null
  $SUDO chown root:root "$CONF"; $SUDO chmod 0644 "$CONF"
}
install_good_db() {
  $SUDO rm -f "$DB" "$DB-wal" "$DB-shm" "$DB-journal"
  $SUDO sh -c "sqlite3 '$DB' < '$FIXTURE'"
  $SUDO chown root:root "$DB"; $SUDO chmod 0644 "$DB"
}

cleanup() {
  set +e
  [ -f /etc/nsswitch.conf.nhnbak ] && $SUDO mv -f /etc/nsswitch.conf.nhnbak /etc/nsswitch.conf
  $SUDO rm -f "$INSTALLED_SO" "$CONF" "$CONF_ALT" "$DB" "$DB-wal" "$DB-shm" "$DB-journal" "$DB_ALT"
  # Best-effort: remove any test user we added for the ownership check.
  $SUDO userdel nh_nss_neg 2>/dev/null
  $SUDO ldconfig
}
trap cleanup EXIT

step "install module -> $INSTALLED_SO"
$SUDO install -m 0644 "$NSS_SO" "$INSTALLED_SO"; $SUDO ldconfig
$SUDO mkdir -p "$DBDIR"; $SUDO chmod 0755 "$DBDIR"; $SUDO chown root:root "$DBDIR"

step "wire nsswitch (nostr appended)"
$SUDO cp /etc/nsswitch.conf /etc/nsswitch.conf.nhnbak
$SUDO sed -i "/^passwd:/ { /nostr/! s/\$/ nostr/ }" /etc/nsswitch.conf
$SUDO sed -i "/^group:/  { /nostr/! s/\$/ nostr/ }" /etc/nsswitch.conf

# We test with an existing 0644 root-owned DB then flip conf/db attributes.
install_good_db

step "assert: missing /etc/nss_nostr.conf -> UNAVAIL/fall through"
$SUDO rm -f "$CONF"
check_empty "getent passwd n_active (no config)" "$(getent passwd n_active || true)"

step "assert: /etc/nss_nostr.conf world-writable -> rejected by trusted_file"
install_good_conf
$SUDO chmod 0666 "$CONF"
check_empty "getent passwd n_active (config 0666)" "$(getent passwd n_active || true)"
$SUDO chmod 0644 "$CONF"

step "assert: /etc/nss_nostr.conf is a symlink -> rejected"
$SUDO rm -f "$CONF"
echo "projection_path=$DB" | $SUDO tee "$CONF_ALT" >/dev/null
$SUDO chown root:root "$CONF_ALT"; $SUDO chmod 0644 "$CONF_ALT"
$SUDO ln -s "$CONF_ALT" "$CONF"
check_empty "getent passwd n_active (config symlink)" "$(getent passwd n_active || true)"
$SUDO rm -f "$CONF" "$CONF_ALT"

step "assert: projection DB non-root-owned -> rejected"
install_good_conf
install_good_db
$SUDO useradd --no-create-home --shell /usr/sbin/nologin nh_nss_neg 2>/dev/null || true
$SUDO chown nh_nss_neg:nh_nss_neg "$DB"
check_empty "getent passwd n_active (db owned by non-root)" "$(getent passwd n_active || true)"
$SUDO chown root:root "$DB"

step "assert: projection DB world-writable -> rejected"
$SUDO chmod 0666 "$DB"
check_empty "getent passwd n_active (db 0666)" "$(getent passwd n_active || true)"
$SUDO chmod 0644 "$DB"

step "assert: projection DB is a symlink -> rejected"
$SUDO cp "$DB" "$DB_ALT"
$SUDO chown root:root "$DB_ALT"; $SUDO chmod 0644 "$DB_ALT"
$SUDO rm -f "$DB"
$SUDO ln -s "$DB_ALT" "$DB"
check_empty "getent passwd n_active (db symlink)" "$(getent passwd n_active || true)"
$SUDO rm -f "$DB" "$DB_ALT"

step "assert: healthy 0644 root-owned config+db resolves"
install_good_db
check "getent passwd n_active (baseline)" \
  "n_active:x:200000:200000:Nostr User:/home/n_active:/bin/bash" \
  "$(getent passwd n_active)"

step "assert: unknown user under healthy config -> NOTFOUND"
check_empty "getent passwd nh_nonexistent_user (NOTFOUND)" \
  "$(getent passwd nh_nonexistent_user || true)"

step "assert: DB read-only side effects — no -wal / -shm / -journal after lookups"
for suffix in -wal -shm -journal; do
  if [ -e "$DB$suffix" ]; then
    echo "  FAIL sidecar present: $DB$suffix"; fails=$((fails+1))
  else
    echo "  PASS no sidecar $DB$suffix"
  fi
done

echo
if [ "$fails" -eq 0 ]; then
  echo "RESULT: PASS (fail-closed NSS negative acceptance on $(uname -m))"
  exit 0
else
  echo "RESULT: FAIL ($fails checks)"
  exit 1
fi
