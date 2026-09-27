#!/usr/bin/env bash
#
# test_smb_conf_standalone_testparm.sh — `testparm -s` gate for the shipped
# config/smb.conf.standalone.sample (nostrc-bvka).
#
# The sample points state/private dirs at packaged paths under
# /var/lib/nostr-auth, and testparm refuses a config whose state
# directory does not exist, so running it on the pristine file made the
# result depend on whether nostrc-samba-server was installed (and on
# the caller being root: the package creates /var/lib/nostr-auth 0700).
#
#   1. Hermetic (always): a temp copy with every packaged path rewritten
#      under a scratch dir, those dirs created. This is the parse gate.
#   2. Installed (only when the packaged state dir is visible, i.e. the
#      package is installed and we can traverse it — normally root): the
#      pristine sample, so the real paths are checked too.
#
# `netbios name` is pinned on the command line only: without it testparm
# derives it from the host name and warns when that exceeds 15 chars.
# The sample deliberately sets none (a fixed name would collide between
# installs on one LAN).
#
# Exit 77 (ctest SKIP_RETURN_CODE) when testparm is not installed.
set -euo pipefail

script_dir="$(cd "$(dirname "$0")" && pwd)"
sample="${SMB_CONF_SAMPLE:-$script_dir/../../config/smb.conf.standalone.sample}"
testparm="${TESTPARM:-$(command -v testparm || true)}"
packaged_root=/var/lib/nostr-auth
packaged_log=/var/log/nostr-samba

if [ -z "$testparm" ] || [ ! -x "$testparm" ]; then
  echo "SKIP: testparm not found; install samba-common-bin to enable"
  exit 77
fi
[ -r "$sample" ] || { echo "FAIL: sample not readable: $sample"; exit 1; }

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

# run_testparm <label> <config>: rc 0 and no ERROR / unknown-parameter
# lines. (testparm reports unknown parameters but may still exit 0.)
run_testparm() {
  local label="$1" conf="$2" rc=0
  "$testparm" -s --option="netbios name=NOSTRTESTPARM" "$conf" \
    >"$tmp/$label.out" 2>"$tmp/$label.err" || rc=$?
  if [ "$rc" -ne 0 ] || grep -Eiq '^ERROR|unknown parameter' "$tmp/$label.err"; then
    echo "FAIL ($label): testparm rc=$rc on $conf"
    cat "$tmp/$label.err"
    return 1
  fi
  echo "PASS ($label): testparm -s $conf"
}

# 1. Hermetic.
root="$tmp/root"
sed -e "s#$packaged_root#$root$packaged_root#g" \
    -e "s#$packaged_log#$root$packaged_log#g" "$sample" >"$tmp/smb.conf"
if grep -Eq "(^|[[:space:]=:])($packaged_root|$packaged_log)" "$tmp/smb.conf"; then
  echo "FAIL: a packaged path survived the rewrite:"
  grep -En "$packaged_root|$packaged_log" "$tmp/smb.conf"
  exit 1
fi
# Every directory the sample names: state/private dirs, the share root,
# the log dir. 0755 matches what testparm expects for browsing.
grep -Eo "$root[^[:space:]]*" "$tmp/smb.conf" | sed 's#/log\.%m$##' | sort -u |
  while read -r dir; do
    case "$dir" in
      */smbpasswd) mkdir -p -m 0755 "$(dirname "$dir")" ;;
      *) mkdir -p -m 0755 "$dir" ;;
    esac
  done
run_testparm hermetic "$tmp/smb.conf"

# 2. Installed paths.
if [ -d "$packaged_root/samba-state" ]; then
  run_testparm installed "$sample"
else
  echo "NOTE: $packaged_root/samba-state not visible to uid $(id -u);" \
       "pristine-path check skipped (needs nostrc-samba-server installed and root)"
fi
