#!/usr/bin/env bash
# --static is source/contract validation, NOT deployment acceptance.
# --live checks an installed disposable host; never silently skips capabilities.
set -euo pipefail
here=$(cd -- "$(dirname -- "$0")" && pwd)
case "${1:-}" in
  --static) exec python3 "$here/test_deployment_contract.py" ;;
  --compose) exec python3 "$here/test_deployment_contract.py" --compose ;;
  --image)
    image=${2:?image to inspect required}
    docker run --rm --platform linux/amd64 --network none --user 0:0 \
      --tmpfs /data --tmpfs /backups --tmpfs /run/secrets --entrypoint sh -i "$image" <<'SH'
set -eu
key=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef
mkdir -p /data /backups /run/secrets
printf '%s' "$key" > /run/secrets/signet_db_key
printf '%s' "$key" > /run/secrets/signet_backup_key
printf "PRAGMA key='%s'; CREATE TABLE proof (value TEXT); INSERT INTO proof VALUES ('fixture');\n" "$key" | sqlcipher -bail /data/signet.db
cp /data/signet.db /backups/fixture.db
chown -R 10001:10001 /data /backups /run/secrets
chmod 0400 /run/secrets/*
before=$(sha256sum /data/signet.db)
su -s /bin/sh signet -c 'signet-inspect-store state' > /tmp/inspection
head -1 /tmp/inspection | grep -qx ok
su -s /bin/sh signet -c 'signet-inspect-store backup /backups/fixture.db' > /tmp/backup-inspection
cmp /tmp/inspection /tmp/backup-inspection
test "$before" = "$(sha256sum /data/signet.db)"
printf 'wrong-fixture-key' > /run/secrets/signet_db_key
if su -s /bin/sh signet -c 'signet-inspect-store state'; then exit 1; fi
mv /data/signet.db /data/saved.db
if su -s /bin/sh signet -c 'signet-inspect-store state'; then exit 1; fi
test ! -e /data/signet.db
! ldd /usr/local/bin/signetd | grep -q 'not found'
! ldd /usr/local/bin/signetctl | grep -q 'not found'
echo 'Runtime SQLCipher inspection, wrong-key and no-create refusals: PASS'
SH
    ;;
  --live)
    : "${SIGNET_LIVE_RELAYS:?explicit live relay selection required}"
    [[ $(uname -s) == Linux && $(id -u) == 0 ]]
    test -x /opt/signet/current/signet/deploy/backup.sh
    python3 /opt/signet/current/signet/deploy/ops.py health
    # The live route requires an actual encrypted export AND off-host upload.
    /opt/signet/current/signet/deploy/backup.sh
    ss -lntp
    echo 'Live health/digest/online backup upload passed; fresh-host restore and off-host exposure require owner drill.'
    ;;
  *) echo 'usage: test_deployment_smoke.sh --static|--compose|--image IMAGE|--live' >&2; exit 2 ;;
esac
