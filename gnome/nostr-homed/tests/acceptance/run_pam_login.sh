#!/usr/bin/env bash
#
# run_pam_login.sh — end-to-end broker+PAM authentication acceptance (B4/B6).
#
# Seeds an authority with a local-vault account, runs nostr-authd, installs
# pam_nostr.so, and drives pamtester through the real Linux PAM stack: correct
# passphrase succeeds; wrong passphrase and unknown user are denied; acct_mgmt
# passes for the active account. Restores system PAM/module state on exit.
# DESTRUCTIVE to system PAM config — disposable lab only. Tracks nostrc-zcll.5.
#
# Requires (built): $BIN/{pam_nostr.so,nostr-authd,nh-seed-authority}. Tools:
# pamtester, sqlite3 (linked), sudo. Usage:
#   NOSTR_ACCEPTANCE_LAB_OPT_IN=I_UNDERSTAND_THIS_RUNS_AGAINST_A_DISPOSABLE_LAB \
#     BIN=/path/to/build/gnome/nostr-homed \
#     gnome/nostr-homed/tests/acceptance/run_pam_login.sh
#
set -u
OPT_IN=I_UNDERSTAND_THIS_RUNS_AGAINST_A_DISPOSABLE_LAB
[ "${NOSTR_ACCEPTANCE_LAB_OPT_IN:-}" = "$OPT_IN" ] \
  || { echo "refusing: set NOSTR_ACCEPTANCE_LAB_OPT_IN=$OPT_IN (disposable lab only)" >&2; exit 69; }
BIN="${BIN:?set BIN to the build dir containing pam_nostr.so/nostr-authd/nh-seed-authority}"
for a in pam_nostr.so nostr-authd nh-seed-authority; do
  [ -f "$BIN/$a" ] || { echo "missing artifact: $BIN/$a" >&2; exit 64; }
done
command -v pamtester >/dev/null || { echo "pamtester required" >&2; exit 64; }
SUDO="${SUDO:-sudo}"
PASS="correct horse battery staple"
DIR=/var/lib/nostr-auth-acc
SECDIR="/usr/lib/$(gcc -print-multiarch 2>/dev/null || echo x86_64-linux-gnu)/security"
SOCK=/run/nostr-auth/auth.sock
AUTHD=""
fails=0
check() { if [ "$2" = "$3" ]; then echo "  PASS $1"; else echo "  FAIL $1 (expected $2, got $3)"; fails=$((fails+1)); fi; }

cleanup() {
  set +e
  [ -n "$AUTHD" ] && $SUDO kill "$AUTHD" 2>/dev/null
  $SUDO rm -f "$SECDIR/pam_nostr.so" /etc/pam.d/nostr-acc
  $SUDO rm -rf "$DIR" /run/nostr-auth
}
trap cleanup EXIT

$SUDO rm -rf "$DIR"; $SUDO mkdir -m 0700 "$DIR"
$SUDO "$BIN/nh-seed-authority" "$DIR" n_alice "$PASS" >/dev/null || { echo "seed failed"; exit 1; }
$SUDO install -m 0644 "$BIN/pam_nostr.so" "$SECDIR/pam_nostr.so"
$SUDO mkdir -p /run/nostr-auth; $SUDO chmod 0700 /run/nostr-auth
$SUDO "$BIN/nostr-authd" "$SOCK" "$DIR" >/dev/null 2>&1 &
AUTHD=$!
for i in $(seq 1 50); do $SUDO test -S "$SOCK" && break; sleep 0.1; done
$SUDO test -S "$SOCK" || { echo "broker socket not up"; exit 1; }
printf 'auth     required pam_nostr.so socket=%s\naccount  required pam_nostr.so socket=%s\n' "$SOCK" "$SOCK" \
  | $SUDO tee /etc/pam.d/nostr-acc >/dev/null

pass_ok=$(echo "$PASS" | $SUDO pamtester nostr-acc n_alice authenticate >/dev/null 2>&1 && echo ok || echo fail)
check "authenticate: correct passphrase" ok "$pass_ok"
acct_ok=$($SUDO pamtester nostr-acc n_alice acct_mgmt >/dev/null 2>&1 && echo ok || echo fail)
check "acct_mgmt: active account" ok "$acct_ok"
wrong=$(echo "nope nope nope nope" | $SUDO pamtester nostr-acc n_alice authenticate >/dev/null 2>&1 && echo ok || echo fail)
check "authenticate: wrong passphrase denied" fail "$wrong"
unknown=$(echo "$PASS" | $SUDO pamtester nostr-acc nobody_x authenticate >/dev/null 2>&1 && echo ok || echo fail)
check "authenticate: unknown user denied" fail "$unknown"

echo
if [ "$fails" -eq 0 ]; then echo "RESULT: PASS (broker+PAM login on $(uname -m))"; exit 0
else echo "RESULT: FAIL ($fails checks)"; exit 1; fi
