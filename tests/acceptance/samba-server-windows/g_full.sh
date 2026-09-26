#!/bin/bash
set +e
export XDG_RUNTIME_DIR=/tmp/gvfs-runtime.$$
mkdir -p $XDG_RUNTIME_DIR
chmod 0700 $XDG_RUNTIME_DIR

pkill -f gvfsd-smb 2>/dev/null
pkill -f "gio mount" 2>/dev/null
sleep 1

exec dbus-run-session -- bash -c "
echo '=== G1: mount + list ==='
printf 'nostr\nWORKGROUP\ntest-password-for-acceptance\n' | timeout 20 gio mount 'smb://192.168.64.3/nostr-home' 2>&1 | grep -v -iE 'dbus-daemon|Activating|Successfully|Message:|goa-daemon'
gio mount --list 2>&1 | grep -iE 'nostr-home'

echo
echo '=== G1: list ==='
gio list 'smb://192.168.64.3/nostr-home' 2>&1

echo '=== G1: cat README.txt ==='
gio cat 'smb://192.168.64.3/nostr-home/README.txt' 2>&1

echo
echo '=== G2: copy IN (local -> share) ==='
echo 'gvfs probe from GNOME Files backend' > /tmp/g2_local.txt
sha256sum /tmp/g2_local.txt | awk '{print \"local sha256:\",\$1}'
timeout 15 gio copy /tmp/g2_local.txt 'smb://192.168.64.3/nostr-home/g2_probe.txt' 2>&1
echo '-- share contents after copy IN --'
gio list 'smb://192.168.64.3/nostr-home' | grep g2_probe

echo '=== G2: cat back over SMB ==='
gio cat 'smb://192.168.64.3/nostr-home/g2_probe.txt' 2>&1

echo '=== G2: copy OUT (share -> local) ==='
timeout 15 gio copy 'smb://192.168.64.3/nostr-home/g2_probe.txt' /tmp/g2_readback.txt 2>&1
sha256sum /tmp/g2_readback.txt | awk '{print \"readback sha256:\",\$1}'
if diff -q /tmp/g2_local.txt /tmp/g2_readback.txt >/dev/null; then
    echo 'G2 ROUND-TRIP: PASS (byte-identical)'
else
    echo 'G2 ROUND-TRIP: FAIL'; diff -u /tmp/g2_local.txt /tmp/g2_readback.txt
fi

echo '=== G2: delete via gio remove ==='
timeout 15 gio remove 'smb://192.168.64.3/nostr-home/g2_probe.txt' 2>&1
echo '-- gio info on deleted file (should fail) --'
timeout 15 gio info 'smb://192.168.64.3/nostr-home/g2_probe.txt' 2>&1 || echo 'info-fail-as-expected'

echo
echo '=== G3 (bonus): unmount ==='
timeout 15 gio mount -u 'smb://192.168.64.3/nostr-home' 2>&1 || true
gio mount --list 2>&1 | grep -iE 'nostr-home' && echo 'still mounted (?)' || echo 'unmounted OK'
"
