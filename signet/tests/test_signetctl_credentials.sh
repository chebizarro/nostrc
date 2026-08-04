#!/bin/sh
set -eu

ctl=$1
tmpdir=$(mktemp -d "${TMPDIR:-/tmp}/signetctl-credential-test.XXXXXX")
trap 'rm -rf "$tmpdir"' EXIT INT TERM

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

echo "signetctl credential exit/input tests: PASS"
