#!/usr/bin/env bash
#
# run_nss_getent.sh — installed libnss_nostr.so.2 acceptance check (A5 evidence).
#
# Installs a prebuilt NSS module against a seeded read-only projection snapshot,
# wires nsswitch, asserts getent/id resolution and NOTFOUND semantics, then
# restores everything. DESTRUCTIVE to system NSS config — run only on a
# disposable lab VM. Tracks beads nostrc-nxpb.4 / nostrc-nxpb.6.
#
# Usage (on the Linux target, as a sudo-capable user):
#   NOSTR_ACCEPTANCE_LAB_OPT_IN=I_UNDERSTAND_THIS_RUNS_AGAINST_A_DISPOSABLE_LAB \
#     NSS_SO=/path/to/libnss_nostr.so.2 FIXTURE=/path/to/projection_v1.sql \
#     gnome/nostr-homed/tests/acceptance/run_nss_getent.sh
#
set -euo pipefail

OPT_IN=I_UNDERSTAND_THIS_RUNS_AGAINST_A_DISPOSABLE_LAB
[ "${NOSTR_ACCEPTANCE_LAB_OPT_IN:-}" = "$OPT_IN" ] \
  || { echo "refusing: set NOSTR_ACCEPTANCE_LAB_OPT_IN=$OPT_IN (disposable lab only)" >&2; exit 69; }

here="$(cd "$(dirname "$0")" && pwd)"
NSS_SO="${NSS_SO:-}"
FIXTURE="${FIXTURE:-$here/../identity/fixtures/projection_v1.sql}"
[ -n "$NSS_SO" ] && [ -f "$NSS_SO" ] || { echo "set NSS_SO to the built libnss_nostr.so.2" >&2; exit 64; }
[ -f "$FIXTURE" ] || { echo "fixture not found: $FIXTURE" >&2; exit 64; }
command -v sqlite3 >/dev/null || { echo "sqlite3 CLI required" >&2; exit 64; }

SUDO="${SUDO:-sudo}"
LIBDIR="/usr/lib/$(gcc -print-multiarch 2>/dev/null || echo x86_64-linux-gnu)"
DBDIR=/var/lib/nostr-auth
DB=$DBDIR/nss.db
CONF=/etc/nss_nostr.conf
INSTALLED_SO="$LIBDIR/libnss_nostr.so.2"
fails=0
check() { # desc, expected, actual
  if [ "$2" = "$3" ]; then echo "  PASS $1"; else echo "  FAIL $1"; echo "    expected: $2"; echo "    actual:   $3"; fails=$((fails+1)); fi
}

cleanup() {
  set +e
  [ -f /etc/nsswitch.conf.nhbak ] && $SUDO mv -f /etc/nsswitch.conf.nhbak /etc/nsswitch.conf
  $SUDO rm -f "$INSTALLED_SO" "$CONF" "$DB"
  $SUDO ldconfig
}
trap cleanup EXIT

echo "== install module -> $INSTALLED_SO =="
$SUDO install -m 0644 "$NSS_SO" "$INSTALLED_SO"; $SUDO ldconfig
echo "== seed read-only projection -> $DB =="
$SUDO mkdir -p "$DBDIR"; $SUDO chmod 0755 "$DBDIR"; $SUDO rm -f "$DB"
$SUDO sh -c "sqlite3 '$DB' < '$FIXTURE'"
$SUDO chown root:root "$DB"; $SUDO chmod 0644 "$DB"
echo "projection_path=$DB" | $SUDO tee "$CONF" >/dev/null
$SUDO chown root:root "$CONF"; $SUDO chmod 0644 "$CONF"
echo "== wire nsswitch =="
$SUDO cp /etc/nsswitch.conf /etc/nsswitch.conf.nhbak
$SUDO sed -i "/^passwd:/ { /nostr/! s/\$/ nostr/ }" /etc/nsswitch.conf
$SUDO sed -i "/^group:/ { /nostr/! s/\$/ nostr/ }" /etc/nsswitch.conf

echo "== assertions =="
check "getent passwd n_active (by name)" \
  "n_active:x:200000:200000:Nostr User:/home/n_active:/bin/bash" "$(getent passwd n_active)"
check "getent passwd 200000 (by uid)" \
  "n_active:x:200000:200000:Nostr User:/home/n_active:/bin/bash" "$(getent passwd 200000)"
check "getent passwd n_disabled (disabled stays resolvable)" \
  "n_disabled:x:200001:200001:Nostr User:/home/n_disabled:/bin/bash" "$(getent passwd n_disabled)"
check "getent group n_active" "n_active:x:200000:" "$(getent group n_active)"
check "id n_active" "uid=200000(n_active) gid=200000(n_active) groups=200000(n_active)" "$(id n_active)"
check "getent passwd <unknown> is NOTFOUND" "" "$(getent passwd nh_nonexistent_user || true)"

echo
if [ "$fails" -eq 0 ]; then echo "RESULT: PASS (installed NSS getent/id on $(uname -m))"; exit 0
else echo "RESULT: FAIL ($fails checks)"; exit 1; fi
