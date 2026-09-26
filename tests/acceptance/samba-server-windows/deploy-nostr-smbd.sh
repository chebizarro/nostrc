#!/bin/bash
# Bring up nostr-smbd for the Windows + GNOME Files acceptance run.
# Runs as any sudo-capable user on the server. Idempotent-ish.
#
# NOTE — this script APPLIES WORKAROUNDS for two packaging defects that
# are currently open beads (nostrc-0vuu, nostrc-45b2). When those beads
# close, delete the two sed blocks at the bottom and the "move share
# out of /var/lib/nostr-auth" block.

set -euo pipefail

# ── 1. Stop stock samba so 445/139 are free for our standalone smbd ────
sudo systemctl stop smbd nmbd 2>/dev/null || true
sudo systemctl mask smbd nmbd 2>/dev/null || true

# ── 2. Provision the nostr-smb-share system user (sysusers.d) ──────────
if ! id nostr-smb-share &>/dev/null; then
  sudo useradd --system --no-create-home --shell /usr/sbin/nologin \
       --comment "Nostr standalone-Samba share owner" nostr-smb-share
fi

# ── 3. Provision state + share dirs ────────────────────────────────────
sudo mkdir -p /var/lib/nostr-auth/samba-state
sudo chmod 0755 /var/lib/nostr-auth              # workaround for nostrc-45b2
sudo chmod 0700 /var/lib/nostr-auth/samba-state
sudo mkdir -p /srv/nostr-home                     # workaround for nostrc-45b2
sudo chown nostr-smb-share:nostr-smb-share /srv/nostr-home
sudo chmod 0750 /srv/nostr-home
sudo mkdir -p /var/log/nostr-samba /run/nostr-smbd /etc/nostr-auth /etc/nostr-auth/servers.d
sudo chmod 0755 /var/log/nostr-samba /run/nostr-smbd /etc/nostr-auth /etc/nostr-auth/servers.d

# ── 4. Seed fixture files inside the share ─────────────────────────────
sudo bash -c '
  cd /srv/nostr-home
  echo "hello from server side" > README.txt
  mkdir -p subdir
  echo "nested" > subdir/nested.txt
  chown -R nostr-smb-share:nostr-smb-share .
  chmod 0640 README.txt subdir/nested.txt
  chmod 0750 subdir
'

# ── 5. Deploy patched smb.conf and systemd unit ────────────────────────
#    The two "sed" edits below apply the acceptance workarounds:
#      - state directory / private dir move to /var/lib/nostr-auth/samba-state
#      - share path moves to /srv/nostr-home
#    plus --no-process-group is added to ExecStart per nostrc-0vuu.
SRC_CONF="$(dirname "$0")/../../../gnome/nostr-homed/config/smb.conf.standalone.sample"
SRC_UNIT="$(dirname "$0")/../../../gnome/nostr-homed/systemd/nostr-smbd.service.in"

if [[ ! -f "$SRC_CONF" || ! -f "$SRC_UNIT" ]]; then
  echo "ERROR: cannot find shipped smb.conf.standalone.sample or nostr-smbd.service.in near this script" >&2
  exit 1
fi

TMP_CONF=$(mktemp)
TMP_UNIT=$(mktemp)
cp "$SRC_CONF" "$TMP_CONF"
cp "$SRC_UNIT" "$TMP_UNIT"

# smb.conf edits — flip share to available=yes, add LAN interface,
# move state dir off /var/lib/nostr-auth, move share path to /srv/nostr-home,
# rewrite passdb backend to the new state dir. All are workarounds for
# nostrc-45b2 and should be reverted once that bead lands.
LAN_CIDR="${LAN_CIDR:-192.168.64.0/24}"
sed -i \
  -e 's|available = no|available = yes|' \
  -e "s|interfaces = 127.0.0.1/8 ::1/128|interfaces = 127.0.0.1/8 ::1/128 ${LAN_CIDR}|" \
  -e 's|passdb backend = tdbsam:/var/lib/nostr-auth/smbpasswd|passdb backend = tdbsam:/var/lib/nostr-auth/samba-state/smbpasswd|' \
  -e 's|path = /var/lib/nostr-auth/share-root|path = /srv/nostr-home|' \
  "$TMP_CONF"
# Add explicit private/state/cache dir overrides just after passdb backend
sed -i '/^    passdb backend =/a\    private dir = /var/lib/nostr-auth/samba-state\n    state directory = /var/lib/nostr-auth/samba-state\n    cache directory = /var/lib/nostr-auth/samba-state\n    pid directory = /run/nostr-smbd' \
  "$TMP_CONF"

# Systemd unit edit — nostrc-0vuu workaround (add --no-process-group) plus
# nostrc-45b2 workaround (0755 state dir mode) plus the @NH_SMB_CONF_PATH@
# template substitution.
sed -i \
  -e 's|@NH_SMB_CONF_PATH@|/etc/nostr-auth/smb.conf|g' \
  -e 's|@CMAKE_INSTALL_FULL_DOCDIR@|/usr/share/doc/nostrc|g' \
  -e 's|^ExecStart=/usr/sbin/smbd -F -s|ExecStart=/usr/sbin/smbd --foreground --no-process-group -s|' \
  -e 's|^StateDirectoryMode=0700|StateDirectoryMode=0755|' \
  "$TMP_UNIT"

sudo install -m 0644 -o root -g root "$TMP_CONF" /etc/nostr-auth/smb.conf
sudo install -m 0644 -o root -g root "$TMP_UNIT" /etc/systemd/system/nostr-smbd.service
sudo testparm -s /etc/nostr-auth/smb.conf > /dev/null

# ── 6. Seed the test account in /etc/passwd + passdb ───────────────────
if ! id nostr &>/dev/null; then
  sudo useradd --system --no-create-home --shell /usr/sbin/nologin \
       --comment "Nostr acceptance test user" nostr
fi
sudo usermod -a -G nostr-smb-share nostr

TEST_PASSWORD="${TEST_PASSWORD:-test-password-for-acceptance}"
printf '%s\n%s\n' "$TEST_PASSWORD" "$TEST_PASSWORD" | \
  sudo smbpasswd -s -a -c /etc/nostr-auth/smb.conf nostr

# ── 7. Start the daemon and verify locally ─────────────────────────────
sudo systemctl daemon-reload
sudo systemctl reset-failed nostr-smbd 2>/dev/null || true
sudo systemctl restart nostr-smbd
sleep 1
sudo systemctl is-active nostr-smbd

echo "=== local smbclient sanity check ==="
smbclient //127.0.0.1/nostr-home -U "nostr%${TEST_PASSWORD}" -c "ls" | head -8

echo "===================================================="
echo "Ready. Now run tests/acceptance/samba-server-windows/w_full.ps1"
echo "and w8_offline.ps1 from the Windows client, and g_full.sh from"
echo "any Linux client with gvfs-backends."
echo "===================================================="
