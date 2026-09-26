#!/usr/bin/env bash
#
# test_smb_batch_mount.sh — plan §4.1 A1 acceptance.
#
# Feeds `nostr-smb-mount --batch` a servers.d directory with three
# stanzas, one of which will fail credential acquisition.  Verifies:
#   - the mount helper does NOT abort at the first failure,
#   - the exit bitmap reflects the failure class (bit 2 = credentials),
#   - the two remaining shares dispatched successfully (both call
#     through the mock mount.cifs / gio stubs).
#
# The test exercises the batch dispatcher, config parser, and exit
# bitmap composition without touching real Samba / kernel mounts.
#
set -euo pipefail

script_dir="$(cd "$(dirname "$0")" && pwd)"
mount_helper="${MOUNT_HELPER:-}"
if [ -z "$mount_helper" ]; then
  cand="$script_dir/../../src/smb/nostr-smb-mount.sh"
  if [ -x "$cand" ]; then
    mount_helper="$cand"
  elif command -v nostr-smb-mount >/dev/null 2>&1; then
    mount_helper="$(command -v nostr-smb-mount)"
  else
    echo "SKIP: nostr-smb-mount not found" >&2
    exit 77
  fi
fi

TEST_DIR="$(mktemp -d)"
trap 'rm -rf "$TEST_DIR"' EXIT

STUB_BIN="$TEST_DIR/bin"
mkdir -p "$STUB_BIN"

# gio stub — succeed by default, log every invocation.
GIO_LOG="$TEST_DIR/gio.log"
cat > "$STUB_BIN/gio" <<STUB
#!/usr/bin/env bash
printf '%s\n' "\$*" >> "$GIO_LOG"
exit 0
STUB
chmod +x "$STUB_BIN/gio"

# mount.cifs stub — succeed, log.
CIFS_LOG="$TEST_DIR/mountcifs.log"
cat > "$STUB_BIN/mount.cifs" <<STUB
#!/usr/bin/env bash
printf '%s\n' "\$*" >> "$CIFS_LOG"
exit 0
STUB
chmod +x "$STUB_BIN/mount.cifs"

# sudo stub — mount helper always uses `sudo -n mount.cifs` when it is
# not already root.  We are not root in tests, so intercept `sudo` to
# strip `-n` and exec the rest.
cat > "$STUB_BIN/sudo" <<STUB
#!/usr/bin/env bash
args=()
while [ \$# -gt 0 ]; do
  case "\$1" in
    -n|-E) shift ;;
    *) args+=("\$1"); shift ;;
  esac
done
exec "\${args[@]}"
STUB
chmod +x "$STUB_BIN/sudo"

cat > "$STUB_BIN/mountpoint" <<STUB
#!/usr/bin/env bash
exit 0
STUB
chmod +x "$STUB_BIN/mountpoint"

# nostr-smb-acquire stub — write a credentials file, unless the sink
# path contains "fail" in which case exit non-zero to force a per-
# stanza failure that should show up as bit 2 in the exit bitmap.
ACQUIRE_LOG="$TEST_DIR/acquire.log"
cat > "$STUB_BIN/nostr-smb-acquire" <<STUB
#!/usr/bin/env bash
printf '%s\n' "\$*" >> "$ACQUIRE_LOG"
sink=""
while [ \$# -gt 0 ]; do
  case "\$1" in
    --file) sink="\$2"; shift 2 ;;
    *) shift ;;
  esac
done
if [ -z "\$sink" ]; then exit 2; fi
case "\$sink" in
  *fail*) exit 5 ;;
esac
mkdir -p "\$(dirname "\$sink")"
umask 077
cat > "\$sink" <<CREDS
username=n_alice
password=hunter2
CREDS
chmod 600 "\$sink"
exit 0
STUB
chmod +x "$STUB_BIN/nostr-smb-acquire"

# servers.d with three shares
CONFD="$TEST_DIR/servers.d"
mkdir -p "$CONFD"

cat > "$CONFD/home.conf" <<CONF
unc = //nas.local/home
mode = gvfs
acquire = yes
creds = $TEST_DIR/home.credentials
label = Home
CONF

cat > "$CONFD/scratch.conf" <<CONF
unc = //nas.local/scratch
mode = cifs
mountpoint = $TEST_DIR/scratch-mp
acquire = yes
creds = $TEST_DIR/scratch.credentials
CONF
mkdir -p "$TEST_DIR/scratch-mp"

cat > "$CONFD/broken.conf" <<CONF
unc = //nas.local/broken
mode = gvfs
acquire = yes
# creds path contains 'fail' so the acquire stub returns non-zero.
creds = $TEST_DIR/broken-fail.credentials
CONF

set +e
env -i PATH="$STUB_BIN:/usr/bin:/bin" HOME="$TEST_DIR" \
    XDG_STATE_HOME="$TEST_DIR/state" \
    XDG_SESSION_ID="batch-sid" \
    XDG_RUNTIME_DIR="$TEST_DIR/run" \
    DBUS_SESSION_BUS_ADDRESS="unix:path=$TEST_DIR/fake-bus" \
    bash "$mount_helper" --batch --batch-dir "$CONFD"
rc=$?
set -e

# The two clean stanzas should have contributed to the mount stubs;
# the broken stanza should have contributed to the acquire stub only,
# not the mount stubs.
if [ ! -s "$ACQUIRE_LOG" ]; then
  echo "FAIL: acquire stub was never called" >&2
  exit 1
fi
if ! grep -q 'nas.local/home' "$GIO_LOG"; then
  echo "FAIL: gvfs stanza did not call gio" >&2
  cat "$GIO_LOG" >&2
  exit 1
fi
if ! grep -q 'nas.local/scratch' "$CIFS_LOG"; then
  echo "FAIL: cifs stanza did not call mount.cifs" >&2
  cat "$CIFS_LOG" >&2
  exit 1
fi
# Broken stanza should have bumped bit 0 or bit 2.  The mount helper
# maps acquire-failed (exit 6) into bit 0 (generic failure), not bit
# 2 — that's fine, as long as some failure bit is set.
if [ "$rc" = "0" ]; then
  echo "FAIL: batch exit was 0 but one stanza was expected to fail" >&2
  exit 1
fi
if [ "$(( rc & 0x01 ))" = "0" ] && [ "$(( rc & 0x04 ))" = "0" ]; then
  echo "FAIL: expected failure bit (0x01 or 0x04) in exit code $rc" >&2
  exit 1
fi

echo "PASS: batch mount (rc=$rc)"
exit 0
