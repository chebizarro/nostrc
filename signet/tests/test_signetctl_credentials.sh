#!/bin/sh
set -eu

ctl=$1
tmpdir=$(mktemp -d "${TMPDIR:-/tmp}/signetctl-credential-test.XXXXXX")
trap 'rm -rf "$tmpdir"' EXIT INT TERM
unset SIGNET_BUNKER_NSEC SIGNET_BUNKER_NSEC_FILE SIGNET_PROVISIONER_NSEC SIGNET_PROVISIONER_NSEC_FILE
unset SIGNET_DB_KEY SIGNET_DB_KEY_FILE SIGNET_BACKUP_KEY SIGNET_BACKUP_KEY_FILE
export SIGNET_BUNKER_PUBKEY=79be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798
export SIGNET_RELAYS=wss://127.0.0.1:1

expect_status() {
  expected=$1
  shift
  set +e
  "$@" >"$tmpdir/out" 2>"$tmpdir/err"
  actual=$?
  set -e
  if [ "$actual" -ne "$expected" ]; then
    echo "expected exit $expected, got $actual: $*" >&2
    cat "$tmpdir/err" >&2
    exit 1
  fi
}

expect_status 2 "$ctl" create-credential
expect_status 2 "$ctl" create-credential owner --type api_token --label label --stdin --file "$tmpdir/x"
expect_status 2 "$ctl" rotate-credential id
expect_status 2 "$ctl" delete-credential id
expect_status 2 "$ctl" deliver-credential
expect_status 2 "$ctl" deliver-credential owner id
expect_status 2 "$ctl" deliver-credential owner id --out "$tmpdir/a" --out "$tmpdir/b"
expect_status 2 "$ctl" create-credential owner --type ssh_key --label label --stdin

fixture_secret='fixture-secret-must-never-appear'
printf '%s' "$fixture_secret" >"$tmpdir/world"
chmod 0644 "$tmpdir/world"
expect_status 1 "$ctl" create-credential owner --type api_token --label label --file "$tmpdir/world"

chmod 0600 "$tmpdir/world"
ln -s "$tmpdir/world" "$tmpdir/link"
expect_status 1 "$ctl" import-credential owner --type credential --label label --file "$tmpdir/link"

set +e
printf '%s' "$fixture_secret" |
  SIGNET_PROVISIONER_NSEC= "$ctl" create-credential owner \
    --type api_token --label label --stdin >"$tmpdir/out" 2>"$tmpdir/err"
actual=$?
set -e
[ "$actual" -eq 1 ] || {
  echo "expected missing provisioner to exit 1, got $actual" >&2
  exit 1
}
if grep -F "$fixture_secret" "$tmpdir/out" "$tmpdir/err" >/dev/null 2>&1; then
  echo "credential payload leaked to CLI output" >&2
  exit 1
fi

# Invalid arguments must be refused before config, secret reads or relay I/O.
expect_status 2 "$ctl" -c "$tmpdir/missing" adopt-existing owner --sec "$fixture_secret"
! grep -F "$fixture_secret" "$tmpdir/out" "$tmpdir/err"
expect_status 2 "$ctl" restore-existing owner --sec "$fixture_secret"
expect_status 2 "$ctl" -c "$tmpdir/missing" adopt-existing owner --sec "$fixture_secret" --sec -
expect_status 2 "$ctl" adopt-existing owner --sec - --sec -
expect_status 2 "$ctl" restore-existing owner --sec-from-store x --sec-from-store y
expect_status 2 "$ctl" restore-existing owner --sec-from-store x --sec -
! grep -F "$fixture_secret" "$tmpdir/out" "$tmpdir/err"
expect_status 1 "$ctl" -c "$tmpdir/missing" list-agents --verify

# Real corrupt encrypted/plain stores exercise the local CLI exit status.
fixture=$2
"$fixture" --cli-fixture "$tmpdir/state.db"
printf '[store]\ndb_path=%s\n' "$tmpdir/state.db" >"$tmpdir/local.conf"
printf '%s\n' 0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef >"$tmpdir/master"
chmod 0400 "$tmpdir/master"
export SIGNET_DB_KEY_FILE="$tmpdir/master"
expect_status 1 "$ctl" -c "$tmpdir/local.conf" list-agents --verify
grep DECRYPT_FAIL "$tmpdir/out" >/dev/null
expect_status 1 "$ctl" -c "$tmpdir/local.conf" verify-audit
grep BROKEN "$tmpdir/err" >/dev/null
# --sec-from-store uses the same protected-file resolver (not the old env path).


# A protected provisioner file allows stdin validation to run, without network.
printf '%s\n' 2222222222222222222222222222222222222222222222222222222222222222 >"$tmpdir/provisioner"
chmod 0600 "$tmpdir/provisioner"
export SIGNET_PROVISIONER_NSEC_FILE="$tmpdir/provisioner"
expect_status 1 "$ctl" -c "$tmpdir/local.conf" restore-existing corrupt --sec-from-store "$tmpdir/state.db"
grep '32-byte agent secret' "$tmpdir/err" >/dev/null
set +e
printf 'too-short\n' | "$ctl" -c "$tmpdir/local.conf" adopt-existing owner --sec - >"$tmpdir/out" 2>"$tmpdir/err"
actual=$?
set -e
[ "$actual" -eq 1 ]
grep '32-byte agent secret' "$tmpdir/err" >/dev/null
! grep -E 'Published|relay.*connect|too-short' "$tmpdir/out" "$tmpdir/err"
echo "signetctl credential exit/input tests: PASS"
