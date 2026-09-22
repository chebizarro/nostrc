#!/bin/bash
# phone-test-rig-teardown.sh — restore gnome-dev to stock after the
# NIP-46 phone-test rig documented in
# docs/reviews/phone-test-rig-2026-09-22.md.
#
# Idempotent: safe to re-run.  Run on the guest as a user with passwordless
# sudo, e.g. `ssh gnome-dev sudo bash /path/to/phone-test-rig-teardown.sh`.

set -euo pipefail

echo "==> stopping and disabling broker"
sudo systemctl disable --now nostr-authd 2>/dev/null || true
sudo rm -f /etc/systemd/system/multi-user.target.wants/nostr-authd.service
sudo systemctl daemon-reload || true

echo "==> removing PAM profile"
if [ -f /usr/share/pam-configs/nostr ]; then
    sudo pam-auth-update --package --remove nostr 2>&1 | tail -2 || true
fi

echo "==> restoring nsswitch.conf"
if [ -f /etc/nsswitch.conf.rig-backup ]; then
    sudo cp /etc/nsswitch.conf.rig-backup /etc/nsswitch.conf
else
    sudo sed -i 's/^passwd:.*/passwd:         files systemd sss/' /etc/nsswitch.conf
    sudo sed -i 's/^group:.*/group:          files systemd sss/' /etc/nsswitch.conf
fi
grep -E '^(passwd|group):' /etc/nsswitch.conf

echo "==> removing broker binaries and libraries"
sudo rm -f /usr/sbin/nostr-authd
sudo rm -f /usr/sbin/nh-seed-authority /usr/sbin/nh-seed-nip46-authority
sudo rm -f /usr/sbin/nostr-homed-seed
sudo rm -f /usr/bin/nostr-smb-mount /usr/bin/nostr-smb-acquire
sudo rm -f /usr/local/bin/qr_signer_standin
sudo rm -f /usr/lib/x86_64-linux-gnu/security/pam_nostr.so
sudo rm -f /usr/lib/x86_64-linux-gnu/libnss_nostr.so.2
sudo rm -f /usr/lib/x86_64-linux-gnu/libnostr-json.so /usr/lib/x86_64-linux-gnu/libnostr-json.so.1
sudo rm -f /usr/lib/systemd/system/nostr-authd.service
sudo rm -f /usr/share/pam-configs/nostr
sudo rm -f /etc/nss_nostr.conf
sudo rm -rf /etc/nostr-auth
sudo rm -rf /usr/share/nostr-homed /usr/share/doc/nostrc
sudo rm -f /usr/lib/x86_64-linux-gnu/pkgconfig/libnostr-json.pc
sudo rm -f /usr/lib/x86_64-linux-gnu/pkgconfig/nostr-homed.pc
sudo ldconfig || true

echo "==> removing account and home"
if getent passwd n_bizarro >/dev/null 2>&1; then
    sudo pkill -u n_bizarro 2>/dev/null || true
fi
sudo rm -rf /home/n_bizarro

echo "==> wiping authority + smb DBs"
sudo rm -rf /var/lib/nostr-auth
sudo rm -rf /run/nostr-auth

echo "==> disabling greeter extension for GDM"
sudo rm -f /etc/dconf/db/gdm.d/10-nostr-login-qr
sudo dconf update || true
sudo rm -rf /usr/share/gnome-shell/extensions/nostr-login-qr@nostrc

echo "==> restarting gdm"
sudo systemctl restart gdm

sleep 3
echo "==> verification"
echo "passwd:  $(grep '^passwd:' /etc/nsswitch.conf)"
echo "pam_nostr in common-auth: $(grep -c pam_nostr /etc/pam.d/common-auth)"
echo "nostr-authd unit: $(systemctl status nostr-authd 2>&1 | head -1)"
echo "getent n_bizarro: $(getent passwd n_bizarro || echo 'gone')"
echo "gdm: $(systemctl is-active gdm)"
echo
echo "Teardown complete.  See docs/reviews/phone-test-rig-2026-09-22.md for context."
